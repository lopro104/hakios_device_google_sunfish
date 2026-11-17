/*
 * Bluetooth on the Pixel 4a: the WCN3990's Bluetooth half, on a GENI UART (QUP0 serial engine 3, 0x88c000; GPIO 38 CTS,
 * 39 RTS, 40 TX, 41 RX, function qup03), powered by the supplies it shares with the Wi-Fi half (PM6150L L1 xtal, L2 core,
 * L10 PA; PM6150 L10 I/O; RPMh-managed). From Android's device tree (soc/bt_wcn3990, qcom,qup_uart@0x88c000) and its clock
 * driver (gcc-sdmmagpie.c). Firmware: crbtfw21.tlv (rampatch) and crnv21.bin (NVM), the btqca download.
 *
 *   bt=1   a look, reading only: the UART's clocks in the clock controller (their votes and whether they run), the pins,
 *          the four supplies in the PMICs; the serial engine's own registers only if its clocks run (an access to it with
 *          its clock off would hang the bus)
 *   bt=2   ... and the serial engine's clock made 7.3728 MHz (GPLL0 even, M/N 384/15625, gcc-sdmmagpie.c's table: 115200
 *          baud x 16 x 4) and voted on, the pins given to it (qup03, Android's pulls), its identity read: protocol 2 is a
 *          UART. Nothing is sent.
 *   bt=3   ... and the first words with the chip, as upstream hci_qca does for the WCN3990: the UART set up in FIFO mode (the
 *          register values of the Mi 10S port's GENI UART, hakios-thyme, which drives the same block), the power pulses
 *          (0xC0 at 2400 baud: boot mode; 0xFC at 115200: on; 100 ms), then HCI Reset, Read Local Version and the QCA
 *          version command (0xFC00, 0x19); every event received is said in hex. GENI samples 32 times a bit on this SoC
 *          (msm_geni_serial.c, no HALF_SAMPLING): 115200 is the 7.3728 MHz clock / 32 / 2 (bt_div=N overrides the 2).
 *   bt=4   ... and the firmware: 3,000,000 baud (QCA command 0xfc48, index 14; the clock 96 MHz, M/N 8/25), the rampatch
 *          crbtfw21.tlv and the NVM crnv21.bin downloaded (btqca: 0xfc00 0x1e in 243-byte segments; the NVM's tag 17 given
 *          the speed in byte 2, as btqca does before the WCN3991, and its in-band sleep bit cleared: no IBS to serve),
 *          HCI Reset, the version again, the address read (said only as present or not), then 10 s of an LE scan:
 *          how many advertisers were heard (no address or name is logged).
 */
#include <sys/param.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/fcntl.h>
#include <sys/vnode.h>
#include <sys/kauth.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern kern_return_t thread_terminate(thread_t);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);

static uint32_t level;

static void say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

/* ---- the PMICs, read only (the arbiter's observer channel, as the other drivers here) ---- */

#define SP_CORE_PA              0x0c440000u
#define SP_OBS_PA               0x0e600000u

static vm_offset_t sp_core, sp_obs;

static int
pmic_read(uint32_t sid, uint32_t addr)
{
	uint32_t ppid = sid << 8 | (addr >> 8), count, apid = ~0u, status = 0;
	vm_offset_t ch;
	bool done = false;

	if (sp_core == 0) {
		sp_core = ml_io_map(SP_CORE_PA, 0x2000);
		sp_obs = ml_io_map(SP_OBS_PA, 0x10000);
	}
	if (sp_core == 0 || sp_obs == 0) {
		return -1;
	}
	count = rd(sp_core + 4) & 0x7ffu;
	for (uint32_t i = 0; i < count && i < 512; i++) {
		uint32_t map = rd(sp_core + 0x900u + 4u * i);

		if (map != 0 && ((map >> 8) & 0xfffu) == ppid) {
			apid = i;
		}
	}
	if (apid == ~0u) {
		return -1;
	}
	ch = sp_obs + 0x80u * apid;
	hakios_spmi_lock();
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)ch = (1u << 27) | ((addr & 0xffu) << 4);
	__asm__ volatile ("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000 && !done; i++) {
		status = rd(ch + 0x08u);
		done = status & 1u;
		if (!done) {
			delay(1);
		}
	}
	int value = done && !(status & 0x0eu) ? (int)(rd(ch + 0x18u) & 0xffu) : -1;
	hakios_spmi_unlock();
	return value;
}

/* ---- the look ---- */

#define GCC_PA                  0x00100000ul
#define GCC_VOTE                0x5200c         /* APCS_CLOCK_BRANCH_ENA_VOTE_1: the QUP0 clocks' votes */
#define TLMM_SOUTH              0x03d00000ul    /* (pinctrl-sdmmagpie.c: gpio 38..41's tile, checked below) */
#define UART_PA                 0x0088c000ul

static void
look(void)
{
	static const struct { const char *name; uint32_t cbcr, vote_bit; } clocks[] = {
		{ "the serial engine's clock (wrap0 s3)", 0x173c0, 13 }, { "wrap0 master AHB", 0x17004, 6 },
		{ "wrap0 slave AHB", 0x17008, 7 }, { "wrap0 core", 0x1700c, 8 }, { "wrap0 core 2x", 0x17014, 9 },
	};
	static const struct { const char *name; uint32_t sid, base; } supplies[] = {
		{ "xtal (PM6150L L1)", 4, 0x4000 }, { "core (PM6150L L2)", 4, 0x4100 }, { "PA (PM6150L L10)", 4, 0x4900 },
		{ "I/O (PM6150 L10)", 0, 0x4900 },
	};
	static const char *const pins[] = { "CTS", "RTS", "TX", "RX" };
	vm_offset_t gcc = ml_io_map(GCC_PA + 0x17000, 0x1000), vote = ml_io_map(GCC_PA + 0x52000, 0x1000);
	bool running = true;

	if (gcc == 0 || vote == 0) {
		say("bt: the clock controller cannot be mapped\n");
		return;
	}
	for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
		uint32_t cbcr = rd(gcc + (clocks[i].cbcr & 0xfff)), voted = (rd(vote + (GCC_VOTE & 0xfff)) >> clocks[i].vote_bit) & 1;

		running = running && !(cbcr & (1u << 31));
		say("bt: clock %s: CBCR %#x (%s), voted %s\n", clocks[i].name, cbcr, (cbcr & (1u << 31)) ? "off" : "RUNS",
		    voted ? "yes" : "no");
	}
	for (unsigned p = 0; p < 4; p++) {
		/* gpio 38..41: which tile? pinctrl-sdmmagpie.c puts 34/35 in the south one; read each where it says it is */
		vm_offset_t page = ml_io_map(TLMM_SOUTH + 0x1000ul * (38 + p), 0x1000);

		if (page != 0) {
			uint32_t cfg = rd(page), io = rd(page + 4);

			say("bt: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", 38 + p, pins[p], (cfg >> 2) & 15, cfg & 3,
			    (((cfg >> 6) & 7) + 1) * 2, (cfg & (1u << 9)) ? "output" : "input", io & 1);
		}
	}
	for (unsigned i = 0; i < sizeof(supplies) / sizeof(supplies[0]); i++) {
		int status = pmic_read(supplies[i].sid, supplies[i].base + 0x08), enable = pmic_read(supplies[i].sid, supplies[i].base + 0x46),
		    vlo = pmic_read(supplies[i].sid, supplies[i].base + 0x40), vhi = pmic_read(supplies[i].sid, supplies[i].base + 0x41);

		say("bt: supply %s: status %#x (%s), enable %#x, voltage %d mV\n", supplies[i].name, status,
		    status >= 0 && (status & 0x80) ? "READY" : "not ready", enable, vlo >= 0 && vhi >= 0 ? (vhi << 8 | vlo) : -1);
	}
	if (!running) {
		say("bt: the serial engine's clocks do not all run: its registers are left alone\n");
		return;
	}
	vm_offset_t se = ml_io_map(UART_PA, 0x4000);

	if (se != 0) {
		say("bt: the serial engine: GENI_FW_REVISION %#x, GENI_STATUS %#x, SE_HW_PARAM_0 %#x, protocol %u\n", rd(se + 0x68),
		    rd(se + 0x40), rd(se + 0xe24), (rd(se + 0x68) >> 8) & 0xff);
	}
}

#define RCG_S3                  0x173c4         /* the serial engine's clock source: CMD, CFG, M, N, D */

static bool
clock_on(void)
{
	vm_offset_t gcc = ml_io_map(GCC_PA + 0x17000, 0x1000), vote = ml_io_map(GCC_PA + 0x52000, 0x1000);
	vm_offset_t rcg;
	uint32_t m = 384, n = 15625, mask = 0xffff;

	if (gcc == 0 || vote == 0) {
		return false;
	}
	rcg = gcc + (RCG_S3 & 0xfff);
	/* source 6 (GPLL0 even), divider 1 (field 1 = 2 x 1 - 1), M/N in dual-edge mode (2 << 12) */
	*(volatile uint32_t *)(rcg + 0x8) = m & mask;
	*(volatile uint32_t *)(rcg + 0xc) = ~(n - m) & mask;
	*(volatile uint32_t *)(rcg + 0x10) = ~n & mask;
	*(volatile uint32_t *)(rcg + 0x4) = 6u << 8 | 1u | 2u << 12;
	*(volatile uint32_t *)rcg |= 1u;                        /* UPDATE */
	for (unsigned i = 0; i < 1000 && (rd(rcg) & 1u); i++) {
		delay(1);
	}
	*(volatile uint32_t *)(vote + (GCC_VOTE & 0xfff)) |= 1u << 13;
	for (unsigned i = 0; i < 1000 && (rd(gcc + 0x3c0) & (1u << 31)); i++) {
		delay(1);
	}
	say("bt: the serial engine's clock: source CMD %#x CFG %#x, CBCR %#x (%s)\n", rd(rcg), rd(rcg + 4), rd(gcc + 0x3c0),
	    (rd(gcc + 0x3c0) & (1u << 31)) ? "STILL OFF" : "runs");
	return !(rd(gcc + 0x3c0) & (1u << 31));
}

static void
pins_to_uart(void)
{
	/* function 1 (qup03), 2 mA; Android: CTS and RX no pull, RTS pull-down, TX pull-up */
	static const uint32_t pull[4] = { 0, 1, 3, 0 };

	for (unsigned p = 0; p < 4; p++) {
		vm_offset_t page = ml_io_map(TLMM_SOUTH + 0x1000ul * (38 + p), 0x1000);

		if (page != 0) {
			*(volatile uint32_t *)page = 1u << 2 | pull[p];
		}
	}
}

/* ---- the UART (GENI FIFO mode) ---- */

static vm_offset_t uart;

static inline void
uw(uint32_t offset, uint32_t value)
{
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(uart + offset) = value;
	__asm__ volatile ("dsb osh" ::: "memory");
}

static inline uint32_t
ur(uint32_t offset)
{
	uint32_t v = *(volatile uint32_t *)(uart + offset);

	__asm__ volatile ("dmb oshld" ::: "memory");
	return v;
}

static void
uart_divider(uint32_t divider)
{
	uw(0x48, divider << 4 | 1);     /* GENI_SER_M_CLK_CFG: SER_CLK_EN, the divider */
	uw(0x4c, divider << 4 | 1);     /* ... S */
	uw(0x7c, 0);                    /* SE_GENI_CLK_SEL */
	delay(1000);
}

/* stopped, both FIFOs emptied, the interrupts cleared (thyme's bt_uart_quiesce) */
static void
uart_quiesce(void)
{
	for (unsigned attempt = 0; attempt < 2; attempt++) {
		uint32_t active = ur(0x40);

		if (!(active & 0x1001)) {
			break;
		}
		if (active & 1) {
			uw(0x604, attempt ? 2 : 4);
		}
		if (active & 0x1000) {
			uw(0x634, attempt ? 2 : 4);
		}
		for (unsigned t = 0; t < 10000 && (ur(0x40) & 0x1001); t++) {
			delay(1);
		}
	}
	for (unsigned words = ur(0x804) & 0x1ffffff; words != 0 && words < 64; words--) {
		(void)ur(0x780);
	}
	uw(0x618, 0xffffffff);
	uw(0x648, 0xffffffff);
}

static void
uart_setup(uint32_t divider)
{
	uart_quiesce();
	uart_divider(divider);
	uw(0x258, 0); uw(0x22c, 0);                     /* FIFO mode, no loopback */
	uw(0x25c, 2); uw(0x280, 0);                     /* TX: CTS ignored; RX: plain */
	uw(0x260, 0xf); uw(0x264, 0);                   /* TX packing: one byte a word */
	uw(0x284, 0x4380e); uw(0x288, 0xc3e0e);         /* RX packing: four bytes a word */
	uw(0x268, 8); uw(0x28c, 8); uw(0x26c, 0);       /* 8 bits, 1 stop bit */
	uw(0x294, 160); uw(0x2a4, 0); uw(0x2a8, 0);     /* RX stale count, no parity */
	uw(0x80c, 1); uw(0x810, 0); uw(0x814, 6);       /* watermarks */
	uw(0x618, 0xffffffff); uw(0x648, 0xffffffff);
	uw(0x630, 1u << 27 | 0x80);                     /* S_CMD0: START_READ, RFR open */
}

static uint8_t rx_ring[1024];
static unsigned rx_head, rx_tail;

static void
uart_rx_take(void)
{
	uw(0x648, ur(0x640));
	for (unsigned budget = 0; budget < 64; budget++) {
		uint32_t status = ur(0x804), words = status & 0x1ffffff, last = (status >> 28) & 7, bytes, word;

		if (words == 0) {
			return;
		}
		bytes = words == 1 && (status >> 31) && last ? last : 4;
		word = ur(0x780);
		for (unsigned b = 0; b < bytes && rx_head - rx_tail < sizeof(rx_ring); b++) {
			rx_ring[rx_head++ % sizeof(rx_ring)] = (uint8_t)(word >> (8 * b));
		}
	}
}

static bool
uart_tx(const uint8_t *bytes, unsigned length)
{
	unsigned sent = 0;

	uw(0x618, 0xffffffff);
	uw(0x270, length);              /* TX_TRANS_LEN */
	uw(0x600, 1u << 27);            /* M_CMD0: START_TX */
	for (unsigned waited = 0; waited < 200000; waited++) {
		unsigned used = ur(0x800) & 0x0fffffff;

		uart_rx_take();
		for (unsigned room = used < 16 ? 16 - used : 0; room != 0 && sent < length; room--) {
			uw(0x700, bytes[sent++]);
		}
		if (sent == length && (ur(0x610) & 1)) {
			uw(0x618, ur(0x610));
			return true;
		}
		delay(1);
	}
	say("bt: TX timed out (%u of %u sent, M_IRQ %#x, TX FIFO %#x)\n", sent, length, ur(0x610), ur(0x800));
	uw(0x604, 4);
	return false;
}

/* the next HCI event (H4: 0x04, code, length, data), waiting up to `ms'; its length, or 0 */
static unsigned
hci_event(uint8_t *event, unsigned capacity, unsigned ms)
{
	for (unsigned waited = 0; waited <= ms * 10; waited++) {
		uart_rx_take();
		while (rx_head != rx_tail && rx_ring[rx_tail % sizeof(rx_ring)] != 0x04) {
			uint8_t b = rx_ring[rx_tail++ % sizeof(rx_ring)];

			say("bt: a byte outside an event: %#04x\n", b);
		}
		if (rx_head - rx_tail >= 3) {
			unsigned length = 3u + rx_ring[(rx_tail + 2) % sizeof(rx_ring)];

			if (rx_head - rx_tail >= length) {
				unsigned n = MIN(length, capacity);

				for (unsigned i = 0; i < length; i++) {
					uint8_t b = rx_ring[rx_tail++ % sizeof(rx_ring)];

					if (i < n) {
						event[i] = b;
					}
				}
				return n;
			}
		}
		delay(100);
	}
	return 0;
}

static void
say_event(const char *what, const uint8_t *e, unsigned n)
{
	char line[200];
	int at = 0;

	for (unsigned i = 0; i < n && at < (int)sizeof(line) - 4; i++) {
		at += snprintf(line + at, sizeof(line) - (size_t)at, " %02x", e[i]);
	}
	say("bt: %s: %s\n", what, n ? line : " NOTHING");
}

static unsigned
hci_command(const char *what, uint16_t opcode, const uint8_t *params, unsigned plen, uint8_t *event, unsigned capacity)
{
	uint8_t packet[260] = { 1, (uint8_t)opcode, (uint8_t)(opcode >> 8), (uint8_t)plen };
	unsigned n;

	memcpy(packet + 4, params, plen);
	if (!uart_tx(packet, plen + 4)) {
		return 0;
	}
	/*
	 * The answer that names this opcode: a Command Complete or Command Status for it, or (the QCA commands 0xfcXX) a vendor
	 * event. Anything else in between -- the chip's NOP "ready" after the rampatch, a late answer -- is said and passed
	 * over (one NOP left unread put every answer after it one behind, logs/015126-run.log).
	 */
	for (unsigned tries = 0; tries < 8; tries++) {
		n = hci_event(event, capacity, 1000);
		if (n == 0 ||
		    (n >= 6 && event[1] == 0x0e && event[4] == (uint8_t)opcode && event[5] == (uint8_t)(opcode >> 8)) ||
		    (n >= 7 && event[1] == 0x0f && event[5] == (uint8_t)opcode && event[6] == (uint8_t)(opcode >> 8)) ||
		    ((opcode >> 8) == 0xfc && event[1] == 0xff)) {
			break;
		}
		if (event[1] != 0x3e && event[1] != 0x2f && event[1] != 0x22 && event[1] != 0x02) {
			say_event("(passed over)", event, n);     /* (advertising and inquiry reports carry others' addresses: never said) */
		}
	}
	say_event(what, event, n);
	return n;
}

/* ---- the firmware ---- */

static int
fw_load(const char *name, uint8_t **data, uint32_t *size)
{
	static const char *const dirs[] = { "/redwood/fw-adsp/", "/private/preboot/redwood/fw-adsp/" };
	vfs_context_t context = vfs_context_kernel();
	int error = ENOENT;

	for (unsigned d = 0; d < 2 && error != 0; d++) {
		char path[80];
		vnode_t vnode = NULLVP;
		struct vnode_attr attributes;
		int resid = 0;
		vm_offset_t memory = 0;

		snprintf(path, sizeof(path), "%s%s", dirs[d], name);
		if ((error = vnode_open(path, FREAD, 0, 0, &vnode, context)) != 0) {
			continue;
		}
		VATTR_INIT(&attributes);
		VATTR_WANTED(&attributes, va_data_size);
		error = vnode_getattr(vnode, &attributes, context);
		if (error == 0 && (attributes.va_data_size < 4 || attributes.va_data_size > 512 * 1024)) {
			error = EFBIG;
		}
		if (error == 0 && kmem_alloc(kernel_map, &memory, round_page(attributes.va_data_size), KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
		    KERN_SUCCESS) {
			error = ENOMEM;
		}
		if (error == 0) {
			error = vn_rdwr(UIO_READ, vnode, (caddr_t)memory, (int)attributes.va_data_size, 0, UIO_SYSSPACE, 0,
			    vfs_context_ucred(context), &resid, vfs_context_proc(context));
			if (error == 0 && resid != 0) {
				error = EIO;
			}
		}
		vnode_close(vnode, FREAD, context);
		if (error == 0) {
			*data = (uint8_t *)memory;
			*size = (uint32_t)attributes.va_data_size;
			say("bt: %s: %u bytes from %s\n", name, *size, dirs[d]);
		}
	}
	return error;
}

static bool
qca_tlv_ack(const uint8_t *e, unsigned n)
{
	/* a vendor event "EDL command response" for the TLV request (btqca: 0xff, length 3, 0x00 0x04 0x00) or a Command Complete */
	return (n == 6 && e[0] == 4 && e[1] == 0xff && e[2] == 3 && e[3] == 0 && e[4] == 4 && e[5] == 0) ||
	       (n >= 7 && e[0] == 4 && e[1] == 0x0e && e[4] == 0x00 && e[5] == 0xfc && e[6] == 0);
}

static bool
qca_download(const char *what, const uint8_t *tlv, uint32_t size, bool skip_events)
{
	uint32_t offset = 0, segments = 0;

	while (offset < size) {
		uint32_t part = MIN(size - offset, 243u);
		uint8_t packet[260] = { 1, 0x00, 0xfc, (uint8_t)(part + 2), 0x1e, (uint8_t)part }, event[32];
		bool last = offset + part == size;

		memcpy(packet + 6, tlv + offset, part);
		if (!uart_tx(packet, part + 6)) {
			return false;
		}
		offset += part;
		segments++;
		if (!skip_events || last) {
			unsigned n = hci_event(event, sizeof(event), 1000);

			/* a Command Complete for opcode 0 (NOP) is the chip saying it is ready (it came after the rampatch): not the answer */
			while (n >= 6 && event[1] == 0x0e && event[4] == 0 && event[5] == 0) {
				say("bt: the chip says it is ready (NOP) during %s\n", what);
				n = hci_event(event, sizeof(event), 1000);
			}

			if (!qca_tlv_ack(event, n)) {
				say_event("a segment NOT acknowledged", event, n);
				say("bt: %s: segment %u of %u refused\n", what, segments, (size + 242) / 243);
				return false;
			}
		}
	}
	say("bt: %s: %u bytes in %u segments, accepted\n", what, size, segments);
	return true;
}

/* the NVM's records (tag, length, 8 reserved bytes, data): tag 17 (HCI transport) gets the speed in byte 2, its IBS bit off */
static bool
nvm_tags(uint8_t *nvm, uint32_t at, uint32_t end, uint8_t baud_index)
{
	bool found = false;

	while (at + 12 <= end) {
		uint32_t tag = nvm[at] | (uint32_t)nvm[at + 1] << 8, length = nvm[at + 2] | (uint32_t)nvm[at + 3] << 8;

		if (at + 12 + length > end) {
			break;
		}
		if (tag == 17 && length >= 3) {
			say("bt: NVM tag 17 was %02x %02x %02x: IBS bit cleared, speed index %u in byte 2\n", nvm[at + 12], nvm[at + 13],
			    nvm[at + 14], baud_index);
			nvm[at + 12] &= (uint8_t)~0x80;
			nvm[at + 14] = baud_index;
			found = true;
		}
		at += 12 + length;
	}
	return found;
}

/*
 * A plain NVM is one TLV of type 2. This phone's crnv21.bin is a TLV of type 4 that holds TLVs: a type 2 (the NVM, 3639
 * bytes) and a type 3 (1071 bytes). It goes to the chip whole, as btqca sends a type it does not know; only the tags of
 * the type 2 inside are changed.
 */
static bool
nvm_prepare(uint8_t *nvm, uint32_t size, uint8_t baud_index)
{
	uint32_t length = (uint32_t)nvm[1] | (uint32_t)nvm[2] << 8 | (uint32_t)nvm[3] << 16;
	bool found = false;

	if (length != size - 4) {
		say("bt: the NVM's length (%u) is not its size less 4 (%u)\n", length, size - 4);
		return false;
	}
	if (nvm[0] == 2) {
		return nvm_tags(nvm, 4, size, baud_index);
	}
	for (uint32_t at = 4; nvm[0] == 4 && at + 4 <= size;) {
		uint32_t inner = (uint32_t)nvm[at + 1] | (uint32_t)nvm[at + 2] << 8 | (uint32_t)nvm[at + 3] << 16;

		if (at + 4 + inner > size) {
			break;
		}
		if (nvm[at] == 2) {
			found = nvm_tags(nvm, at + 4, at + 4 + inner, baud_index) || found;
		}
		at += 4 + inner;
	}
	if (!found) {
		say("bt: no tag 17 in the NVM (type %u)\n", nvm[0]);
	}
	return found;
}

static void
clock_rate(uint32_t m, uint32_t n)
{
	vm_offset_t gcc = ml_io_map(GCC_PA + 0x17000, 0x1000), rcg = gcc + (RCG_S3 & 0xfff);

	*(volatile uint32_t *)(rcg + 0x8) = m;
	*(volatile uint32_t *)(rcg + 0xc) = ~(n - m) & 0xffff;
	*(volatile uint32_t *)(rcg + 0x10) = ~n & 0xffff;
	*(volatile uint32_t *)(rcg + 0x4) = 6u << 8 | 1u | 2u << 12;
	*(volatile uint32_t *)rcg |= 1u;
	for (unsigned i = 0; i < 1000 && (rd(rcg) & 1u); i++) {
		delay(1);
	}
}

static void
firmware_and_scan(void)
{
	uint8_t *patch = NULL, *nvm = NULL, event[64];
	uint32_t patch_size = 0, nvm_size = 0, heard = 0;
	static const uint8_t to_3m[] = { 14 };          /* QCA_BAUDRATE_3000000 */
	static const uint8_t scan_params[] = { 0, 0x10, 0, 0x10, 0, 0, 0 };     /* passive, 10 ms, public, all */
	static const uint8_t scan_on[] = { 1, 0 }, scan_off[] = { 0, 0 };
	uint8_t seen[64][6];
	unsigned n;

	if (fw_load("crbtfw21.tlv", &patch, &patch_size) != 0 || fw_load("crnv21.bin", &nvm, &nvm_size) != 0) {
		say("bt: the firmware files are not in the image (FW_ADSP at build time)\n");
		return;
	}
	if (!nvm_prepare(nvm, nvm_size, 14)) {
		return;
	}
	/* 3,000,000 baud: the chip told, then the host (96 MHz / 32) */
	uint8_t packet[5] = { 1, 0x48, 0xfc, 1, to_3m[0] };
	if (!uart_tx(packet, sizeof(packet))) {
		return;
	}
	delay(10000);
	uart_quiesce();
	clock_rate(8, 25);
	uart_setup(1);
	delay(50000);
	n = hci_event(event, sizeof(event), 200);
	say_event("after the speed change", event, n);
	if (!hci_command("Read Local Version at 3 Mbaud", 0x1001, NULL, 0, event, sizeof(event))) {
		return;
	}
	if (!qca_download("rampatch crbtfw21.tlv", patch, patch_size, patch_size > 14 && patch[14] == 3)) {
		return;
	}
	n = hci_event(event, sizeof(event), 100);       /* the chip's NOP once the patch runs */
	say_event("after the rampatch", event, n);
	if (!qca_download("NVM crnv21.bin", nvm, nvm_size, false)) {
		return;
	}
	delay(10000);
	(void)hci_command("HCI Reset after the firmware", 0x0c03, NULL, 0, event, sizeof(event));
	(void)hci_command("Read Local Version after the firmware", 0x1001, NULL, 0, event, sizeof(event));
	n = hci_command("Read BD_ADDR", 0x1009, NULL, 0, event, 7);
	(void)hci_command("LE Set Scan Parameters", 0x200b, scan_params, sizeof(scan_params), event, sizeof(event));
	(void)hci_command("LE Set Scan Enable", 0x200c, scan_on, sizeof(scan_on), event, sizeof(event));
	for (unsigned ms = 0; ms < 10000; ms += 10) {
		uint8_t e[64];

		while ((n = hci_event(e, sizeof(e), 0)) != 0) {
			if (n >= 13 && e[1] == 0x3e && e[3] == 0x02) {      /* LE advertising report: the first one's address */
				bool known = false;

				for (unsigned i = 0; i < heard && !known; i++) {
					known = memcmp(seen[i], e + 7, 6) == 0;
				}
				if (!known && heard < 64) {
					memcpy(seen[heard++], e + 7, 6);
				}
			}
		}
		delay(10000);
	}
	(void)hci_command("LE Set Scan Enable (off)", 0x200c, scan_off, sizeof(scan_off), event, sizeof(event));
	say("bt: THE RADIO HEARS: %u different Bluetooth LE advertisers in 10 s\n", heard);
	/* back to 115200 for the power-off pulse that follows */
	clock_rate(384, 15625);
}

static void
talk(void)
{
	uint32_t divider = 2;
	uint8_t pulse, event[64];
	static const uint8_t version_request[] = { 0x19 };      /* EDL_PATCH_VER_REQ_CMD */

	PE_parse_boot_argn("bt_div", &divider, sizeof(divider));
	uart = ml_io_map(UART_PA, 0x4000);
	if (uart == 0) {
		return;
	}
	/* boot mode: 0xC0 at 2400 baud (divider x 48 of 115200's) */
	uart_setup(divider * 48);
	pulse = 0xc0;
	(void)uart_tx(&pulse, 1);
	delay(10000);
	/* on: 0xFC at 115200, then 100 ms */
	uart_setup(divider);
	pulse = 0xfc;
	(void)uart_tx(&pulse, 1);
	delay(100000);
	uart_rx_take();
	say("bt: after the pulses: %u bytes waiting, GENI_STATUS %#x, RX FIFO %#x\n", rx_head - rx_tail, ur(0x40), ur(0x804));
	(void)hci_command("HCI Reset", 0x0c03, NULL, 0, event, sizeof(event));
	(void)hci_command("Read Local Version", 0x1001, NULL, 0, event, sizeof(event));
	(void)hci_command("QCA version (0xfc00 0x19)", 0xfc00, version_request, sizeof(version_request), event, sizeof(event));
	if (level >= 4) {
		firmware_and_scan();
	}
	/* off again: 0xC0 at 2400 */
	uart_setup(divider * 48);
	pulse = 0xc0;
	(void)uart_tx(&pulse, 1);
	delay(10000);
	uart_quiesce();
	say("bt: the chip told to power off (0xC0 at 2400 baud); the UART stopped\n");
}

static void
bt_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("bt: level %u\n", level);
	look();
	if (level >= 2 && clock_on()) {
		vm_offset_t se = ml_io_map(UART_PA, 0x4000);

		pins_to_uart();
		if (se != 0) {
			uint32_t fw = rd(se + 0x68);

			say("bt: the serial engine: GENI_FW_REVISION %#x (protocol %u%s, version %#x), GENI_STATUS %#x, SE_HW_PARAM_0 %#x\n",
			    fw, (fw >> 8) & 0xff, ((fw >> 8) & 0xff) == 2 ? ": A UART" : ((fw >> 8) & 0xff) == 0 ? ": NONE LOADED" : "",
			    fw & 0xff, rd(se + 0x40), rd(se + 0xe24));
		}
		if (level >= 3) {
			talk();
		}
	}
	say("bt: done\n");
	thread_terminate(current_thread());
}

static thread_call_t bt_call;

static void
bt_go(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;

	if (kernel_thread_start(bt_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

__startup_func
static void
hakios_bt_sunfish_init(void)
{
	uint64_t deadline;

	if (!PE_parse_boot_argn("bt", &level, sizeof(level)) || level == 0) {
		return;
	}
	bt_call = thread_call_allocate(bt_go, NULL);
	clock_interval_to_deadline(30, NSEC_PER_SEC, &deadline);       /* after Wi-Fi has powered the shared supplies */
	thread_call_enter_delayed(bt_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_bt_sunfish_init);
