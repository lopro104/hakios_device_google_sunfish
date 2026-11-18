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
#include <pexpert/device_tree.h>
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

static uint8_t rx_ring[16384];
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

static void
fw_free(uint8_t *data, uint32_t size)
{
	if (data != NULL) {
		kmem_free(kernel_map, (vm_offset_t)data, round_page(size));
	}
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

/*
 * The address. The NVM's own (tag 2) is the same on every Pixel 4a, and Android writes the phone's in at boot. Here it is
 * made from the SoC's serial number: the same on every boot of one phone (pairings last), different between phones, and
 * never written to a log. LSB first, as HCI and the NVM have it; bit 1 of the top byte set (locally administered).
 */
static uint8_t bd_address_lsb[6];

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);

#define SMEM_PA                 0x86000000ul
#define SMEM_SIZE               0x200000ul
#define WIMG_WCOMB              0x6u

/*
 * The SoC's serial number, from SMEM item 137 (socinfo, serial_num at byte 96) in the global partition (hosts 0xfffe),
 * through mappings made for the moment and taken back, as the modem driver reads SMEM. (HakiOS's socinfo driver maps the
 * whole 2 MB and keeps it: with it on, this phone met a kmem_alloc of 384 KiB that found no space and panicked.)
 */
static uint32_t
soc_serial(void)
{
	vm_offset_t page = ml_io_map_unmappable(SMEM_PA + SMEM_SIZE - 0x4000, 0x4000, WIMG_WCOMB), part;
	uint32_t serial = 0, offset = 0, size = 0;
	const uint8_t *toc;

	if (page == 0) {
		return 0;
	}
	toc = (const uint8_t *)page + 0x4000 - 0x1000;
	if (memcmp(toc, "$TOC", 4) == 0) {
		uint32_t entries = *(const volatile uint32_t *)(toc + 8);

		for (uint32_t i = 0; i < entries && i < 32; i++) {
			const uint8_t *e = toc + 32 + 48 * i;          /* (Linux smem_ptable: a 32-byte header, 48-byte entries) offset, size, flags, host0, host1 */

			if (*(const volatile uint16_t *)(e + 12) == 0xfffe && *(const volatile uint16_t *)(e + 14) == 0xfffe) {
				offset = *(const volatile uint32_t *)e;
				size = *(const volatile uint32_t *)(e + 4);
			}
		}
	}
	ml_io_unmap(page, 0x4000);
	if (size == 0 || offset + size > SMEM_SIZE || size > 0x100000) {
		return 0;
	}
	/* (mapped from the page it begins in: a mapping of an address not on a page panicked in the pmap) */
	vm_offset_t base = trunc_page(SMEM_PA + offset), map_size = round_page(SMEM_PA + offset + size) - base, window;

	window = ml_io_map_unmappable(base, map_size, WIMG_WCOMB);
	if (window == 0) {
		return 0;
	}
	part = window + (SMEM_PA + offset - base);
	if (memcmp((const void *)part, "$PRT", 4) == 0) {
		uint32_t end = MIN(*(const volatile uint32_t *)(part + 12), size);       /* offset_free_uncached */

		for (uint32_t at = 32; at + 16 <= end;) {
			uint16_t canary = *(const volatile uint16_t *)(part + at), item = *(const volatile uint16_t *)(part + at + 2);
			uint32_t length = *(const volatile uint32_t *)(part + at + 4);
			uint16_t pad_hdr = *(const volatile uint16_t *)(part + at + 10);
			uint32_t data = at + 16 + pad_hdr;

			if (canary != 0xa5a5 || data + length > end) {
				break;
			}
			if (item == 137 && length >= 100) {
				serial = *(const volatile uint32_t *)(part + data + 96);
				break;
			}
			at = data + length;
		}
	}
	ml_io_unmap(window, map_size);
	return serial;
}

static void
bd_address(void)
{
	uint64_t h = 0xcbf29ce484222325ull;
	uint32_t serial;
	DTEntry chosen;
	const void *real = NULL;
	unsigned int real_size = 0;

	/* the phone's own, when the loader found it on the bootloader's command line (androidboot.btmacaddr, in display order) */
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) == kSuccess &&
	    SecureDTGetProperty(chosen, "hakios-bluetooth-address", &real, &real_size) == kSuccess && real_size == 6) {
		for (unsigned i = 0; i < 6; i++) {
			bd_address_lsb[i] = ((const uint8_t *)real)[5 - i];
		}
		say("bt: the address is the phone's own, from its bootloader\n");
		return;
	}
	serial = soc_serial();

	for (unsigned i = 0; i < 4; i++) {
		h = (h ^ ((serial >> (8 * i)) & 0xff)) * 0x100000001b3ull;      /* FNV-1a */
	}
	h = (h ^ 0x42) * 0x100000001b3ull;
	for (unsigned i = 0; i < 6; i++) {
		bd_address_lsb[i] = (uint8_t)(h >> (8 * i));
	}
	bd_address_lsb[5] = (uint8_t)((bd_address_lsb[5] & 0xfc) | 0x02);
	say("bt: the address made from the SoC's serial number%s\n", serial ? "" : " (NOT KNOWN: the same on every phone)");
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
		if (tag == 2 && length == 6) {
			memcpy(nvm + at + 12, bd_address_lsb, 6);      /* this phone's address (see bd_address()); never logged */
			say("bt: NVM tag 2 (the address) given this phone's own\n");
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

static uint32_t divider = 2;

/* the power pulses, then the ROM's answers: true when the chip said its QCA version */
static bool
chip_up(void)
{
	uint8_t pulse, event[64];
	static const uint8_t version_request[] = { 0x19 };      /* EDL_PATCH_VER_REQ_CMD */
	unsigned n;

	PE_parse_boot_argn("bt_div", &divider, sizeof(divider));
	uart = ml_io_map(UART_PA, 0x4000);
	if (uart == 0) {
		return false;
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
	n = hci_command("QCA version (0xfc00 0x19)", 0xfc00, version_request, sizeof(version_request), event, sizeof(event));
	return n >= 17 && event[1] == 0xff;
}

static void
chip_off(void)
{
	uint8_t pulse = 0xc0;

	clock_rate(384, 15625);
	uart_setup(divider * 48);
	(void)uart_tx(&pulse, 1);
	delay(10000);
	uart_quiesce();
	say("bt: the chip told to power off (0xC0 at 2400 baud); the UART stopped\n");
}

/* 3 Mbaud, the rampatch and the NVM, a reset: true when the patched firmware answers */
static bool
chip_firmware(void)
{
	uint8_t *patch = NULL, *nvm = NULL, event[64];
	uint32_t patch_size = 0, nvm_size = 0;
	uint8_t packet[5] = { 1, 0x48, 0xfc, 1, 14 };   /* QCA_BAUDRATE_3000000 */
	unsigned n;

	if (fw_load("crbtfw21.tlv", &patch, &patch_size) != 0 || fw_load("crnv21.bin", &nvm, &nvm_size) != 0) {
		say("bt: the firmware files are not in the image (FW_ADSP at build time)\n");
		return false;
	}
	bd_address();
	if (!nvm_prepare(nvm, nvm_size, 14)) {
		fw_free(patch, patch_size);
		fw_free(nvm, nvm_size);
		return false;
	}
	/* 3,000,000 baud: the chip told, then the host (96 MHz / 32) */
	if (!uart_tx(packet, sizeof(packet))) {
		return false;
	}
	delay(10000);
	uart_quiesce();
	clock_rate(8, 25);
	uart_setup(1);
	delay(50000);
	n = hci_event(event, sizeof(event), 200);
	say_event("after the speed change", event, n);
	if (!hci_command("Read Local Version at 3 Mbaud", 0x1001, NULL, 0, event, sizeof(event))) {
		return false;
	}
	if (!qca_download("rampatch crbtfw21.tlv", patch, patch_size, patch_size > 14 && patch[14] == 3)) {
		return false;
	}
	n = hci_event(event, sizeof(event), 100);       /* the chip's NOP once the patch runs */
	say_event("after the rampatch", event, n);
	if (!qca_download("NVM crnv21.bin", nvm, nvm_size, false)) {
		return false;
	}
	delay(10000);
	fw_free(patch, patch_size);
	fw_free(nvm, nvm_size);
	(void)hci_command("HCI Reset after the firmware", 0x0c03, NULL, 0, event, sizeof(event));
	n = hci_command("Read Local Version after the firmware", 0x1001, NULL, 0, event, sizeof(event));
	return n >= 15 && event[6] == 0;
}

static void
chip_scan(void)
{
	uint8_t event[64], seen[64][6];
	uint32_t heard = 0;
	static const uint8_t scan_params[] = { 0, 0x10, 0, 0x10, 0, 0, 0 };     /* passive, 10 ms, public, all */
	static const uint8_t scan_on[] = { 1, 0 }, scan_off[] = { 0, 0 };
	unsigned n;

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
}

/* ---- bt=5: Darwin's. The kernel control com.hakios.bluetooth carries H4 between bluetoothd and the chip ---- */
/*
 * As the Mi 10S port does it (hakios-thyme: kernel control, termios answers, the IOService of hakios.bt_sunfish.cpp,
 * userland/src/inject_bluetooth.c in bluetoothd, umi_bluetool for BlueTool), without its in-band sleep: the NVM has IBS
 * off here, so packets go both ways as they are.
 */
#include <sys/kern_control.h>
#include <sys/kpi_mbuf.h>
#include <sys/termios.h>
#include <sys/ttycom.h>
#include <kern/locks.h>

extern int hakios_bt_sunfish_publish(const uint8_t address[6], const uint8_t commands[64], uint32_t speed);
extern void hakios_bt_sunfish_set_ready(bool ready);

#define BT_SPEED                3000000u
#define TXQ_SIZE                65536u
#define H4_MAX                  4096u

static LCK_GRP_DECLARE(ctl_lock_group, "hakios-bt");
static LCK_MTX_DECLARE(ctl_lock, &ctl_lock_group);
static kern_ctl_ref ctl_ref;
static uint32_t ctl_unit, ctl_generation;
static bool ctl_fault;
static uint8_t txq[TXQ_SIZE];
static uint32_t txq_head, txq_tail;
static struct termios ctl_termios;

struct h4 {
	uint8_t bytes[H4_MAX];
	uint32_t used, wanted, header;
	bool complete;
};

/* 0: incomplete, 1: a packet, -1: a bad type, -2: too long (hakios-thyme's hakios_bt_h4_push, without IBS) */
static int
h4_push(struct h4 *p, uint8_t byte)
{
	if (p->complete) {
		p->used = p->wanted = p->header = 0;
		p->complete = false;
	}
	if (p->used == 0) {
		switch (byte) {
		case 1: case 3: p->header = 4; break;
		case 2: case 5: p->header = 5; break;
		case 4: p->header = 3; break;
		default: return -1;
		}
		p->wanted = p->header;
	}
	if (p->used >= H4_MAX) {
		return -2;
	}
	p->bytes[p->used++] = byte;
	if (p->used == p->header) {
		uint32_t length = p->bytes[0] == 1 || p->bytes[0] == 3 ? p->bytes[3] : p->bytes[0] == 4 ? p->bytes[2] :
		    (p->bytes[3] | (uint32_t)p->bytes[4] << 8) & (p->bytes[0] == 5 ? 0x3fff : 0xffff);

		if (length > H4_MAX - p->header) {
			p->used = p->wanted = p->header = 0;
			return -2;
		}
		p->wanted = p->header + length;
	}
	if (p->used == p->wanted) {
		p->complete = true;
		return 1;
	}
	return 0;
}

static errno_t
ctl_connect(__unused kern_ctl_ref ref, struct sockaddr_ctl *address, void **info)
{
	uid_t uid = kauth_cred_getuid(kauth_cred_get());
	int error;

	if (uid != 0 && uid != 501) {
		return EPERM;
	}
	lck_mtx_lock(&ctl_lock);
	error = ctl_fault ? EIO : ctl_unit ? EBUSY : 0;
	if (error == 0) {
		ctl_unit = address->sc_unit;
		ctl_generation++;
		txq_head = txq_tail = 0;
		*info = NULL;
	}
	lck_mtx_unlock(&ctl_lock);
	if (error == 0) {
		say("bt: bluetoothd connected (uid %u)\n", uid);
	}
	return error;
}

static errno_t
ctl_disconnect(__unused kern_ctl_ref ref, uint32_t unit, __unused void *info)
{
	lck_mtx_lock(&ctl_lock);
	if (ctl_unit == unit) {
		ctl_unit = 0;
		ctl_generation++;
		txq_head = txq_tail = 0;
	}
	lck_mtx_unlock(&ctl_lock);
	say("bt: bluetoothd disconnected\n");
	return 0;
}

static errno_t
ctl_send(__unused kern_ctl_ref ref, uint32_t unit, __unused void *info, mbuf_t message, __unused int flags)
{
	size_t length = mbuf_pkthdr_len(message);
	int error = 0;

	lck_mtx_lock(&ctl_lock);
	if (ctl_fault) {
		error = EIO;
	} else if (unit != ctl_unit) {
		error = ENOTCONN;
	} else if (length > TXQ_SIZE - (txq_head - txq_tail)) {
		error = ENOBUFS;
	}
	for (size_t offset = 0; error == 0 && offset < length;) {
		uint8_t bytes[256];
		size_t count = MIN(length - offset, sizeof(bytes));

		error = mbuf_copydata(message, offset, count, bytes);
		for (size_t i = 0; error == 0 && i < count; i++) {
			txq[txq_head++ % TXQ_SIZE] = bytes[i];
		}
		offset += count;
	}
	lck_mtx_unlock(&ctl_lock);
	mbuf_freem(message);
	return error;
}

static errno_t
ctl_getopt(__unused kern_ctl_ref ref, __unused uint32_t unit, __unused void *info, int option, void *data, size_t *length)
{
	int modem = TIOCM_CTS | TIOCM_DSR | TIOCM_CAR;
	size_t wanted;

	if (option == (int)TIOCGETA) {
		wanted = sizeof(ctl_termios);
	} else if (option == (int)TIOCMGET) {
		wanted = sizeof(modem);
	} else {
		say("bt: bluetoothd asks for option %#x: not known\n", option);
		return ENOPROTOOPT;
	}
	if (data == NULL) {
		*length = wanted;
		return 0;
	}
	if (*length < wanted) {
		return EMSGSIZE;
	}
	lck_mtx_lock(&ctl_lock);
	memcpy(data, option == (int)TIOCGETA ? (void *)&ctl_termios : (void *)&modem, wanted);
	lck_mtx_unlock(&ctl_lock);
	*length = wanted;
	return 0;
}

static errno_t
ctl_setopt(__unused kern_ctl_ref ref, __unused uint32_t unit, __unused void *info, int option, void *data, size_t length)
{
	uint32_t o = (uint32_t)option;

	if (o == (uint32_t)TIOCSETA || o == (uint32_t)TIOCSETAW || o == (uint32_t)TIOCSETAF) {
		const struct termios *settings = data;

		if (length != sizeof(struct termios) || data == NULL) {
			return EINVAL;
		}
		if ((settings->c_ispeed && settings->c_ispeed != BT_SPEED) || (settings->c_ospeed && settings->c_ospeed != BT_SPEED)) {
			say("bt: bluetoothd asks for %lu baud: the link runs at %u\n", (unsigned long)settings->c_ospeed, BT_SPEED);
			return EINVAL;
		}
		lck_mtx_lock(&ctl_lock);
		ctl_termios = *settings;
		lck_mtx_unlock(&ctl_lock);
		return 0;
	}
	if (o == (uint32_t)TIOCSDTR || o == (uint32_t)TIOCCDTR || o == (uint32_t)TIOCMSET || o == (uint32_t)TIOCMBIS ||
	    o == (uint32_t)TIOCMBIC || o == (uint32_t)TIOCFLUSH) {
		return 0;
	}
	say("bt: bluetoothd sets option %#x: not known\n", option);
	return ENOPROTOOPT;
}

static void
ctl_worker(__unused void *parameter, __unused wait_result_t result)
{
	static struct h4 tx, rx;
	uint32_t generation = 0, commands = 0, events = 0;
	bool have_rx = false;
	/* what went by, said every 10 s while it changes (no address, no data): ACL bytes out and in, connections */
	uint64_t acl_out = 0, acl_in = 0, said_out = 0, said_in = 0, next_report = 0;
	uint32_t connections = 0, disconnections = 0, said_conn = 0, said_disc = 0;
	/* per connection handle (the 12 bits of an ACL header): bytes each way, for the first eight handles seen */
	uint16_t handles[8];
	uint64_t h_out[8] = { 0 }, h_in[8] = { 0 };
	unsigned nh = 0;

	for (;;) {
		uint32_t unit;
		bool fault;

		lck_mtx_lock(&ctl_lock);
		unit = ctl_unit;
		if (generation != ctl_generation) {
			generation = ctl_generation;
			memset(&tx, 0, sizeof(tx));
		}
		while (unit && !ctl_fault && !tx.complete && txq_head != txq_tail) {
			int state = h4_push(&tx, txq[txq_tail++ % TXQ_SIZE]);

			if (state < 0 || (tx.used && tx.bytes[0] == 4)) {
				ctl_fault = true;
				say("bt: bluetoothd sent what is not H4: the link is given up\n");
			}
		}
		fault = ctl_fault;
		lck_mtx_unlock(&ctl_lock);
		if (unit && !fault && tx.complete) {
			if (tx.bytes[0] == 2 && tx.used >= 3) {
				uint16_t h = (uint16_t)((tx.bytes[1] | tx.bytes[2] << 8) & 0xfff);
				unsigned k;

				acl_out += tx.used;
				for (k = 0; k < nh && handles[k] != h; k++) {
				}
				if (k == nh && nh < 8) {
					handles[nh++] = h;
				}
				if (k < 8) {
					h_out[k] += tx.used;
				}
			}
			if (tx.bytes[0] == 1 && commands++ < 64) {
				say("bt: bluetoothd sends HCI command %#06x (%u bytes)\n", tx.bytes[1] | tx.bytes[2] << 8, tx.used);
			}
			if (!uart_tx(tx.bytes, tx.used)) {
				lck_mtx_lock(&ctl_lock);
				ctl_fault = true;
				lck_mtx_unlock(&ctl_lock);
				hakios_bt_sunfish_set_ready(false);
				say("bt: the UART stopped taking bytes: the link is given up\n");
			}
			memset(&tx, 0, sizeof(tx));
		}
		/* what the chip sent: whole H4 packets to bluetoothd (never logged: events carry others' addresses) */
		uart_rx_take();
		while (!have_rx && rx_head != rx_tail) {
			int state = h4_push(&rx, rx_ring[rx_tail++ % sizeof(rx_ring)]);

			if (state < 0) {
				say("bt: the chip sent what is not H4 (%d): the packet dropped\n", state);
				memset(&rx, 0, sizeof(rx));
			} else if (state == 1) {
				have_rx = true;
			}
		}
		/*
		 * The chip's own diagnostic log comes as ACL data on handle 0xedc (QCA's debug handle 0x2edc): Linux's hci_qca
		 * takes it aside; here it went to bluetoothd, which said "connection handle ... is invalid" for each. Dropped.
		 */
		if (have_rx && rx.bytes[0] == 2 && rx.used >= 3 && ((rx.bytes[1] | rx.bytes[2] << 8) & 0xfff) == 0xedc) {
			have_rx = false;
			acl_in += rx.used;
		}
		if (have_rx) {
			int queued = unit ? ctl_enqueuedata(ctl_ref, unit, rx.bytes, rx.used, 0) : 0;

			if (queued != ENOBUFS) {
				have_rx = false;
				if (rx.bytes[0] == 2 && rx.used >= 3) {
					uint16_t h = (uint16_t)((rx.bytes[1] | rx.bytes[2] << 8) & 0xfff);
					unsigned k;

					acl_in += rx.used;
					for (k = 0; k < nh && handles[k] != h; k++) {
					}
					if (k == nh && nh < 8) {
						handles[nh++] = h;
					}
					if (k < 8) {
						h_in[k] += rx.used;
					}
				} else if (rx.bytes[0] == 4 && rx.bytes[1] == 0x03 && rx.used >= 6) {
					connections++;
					say("bt: link: a classic connection: status %#x, handle %#x, link type %u\n", rx.bytes[3],
					    (rx.bytes[4] | rx.bytes[5] << 8) & 0xfff, rx.used >= 14 ? rx.bytes[13] : 99);
				} else if (rx.bytes[0] == 4 && rx.bytes[1] == 0x3e && rx.used >= 7 && (rx.bytes[3] == 0x01 || rx.bytes[3] == 0x0a)) {
					connections++;
					say("bt: link: an LE connection: status %#x, handle %#x\n", rx.bytes[4], (rx.bytes[5] | rx.bytes[6] << 8) & 0xfff);
				} else if (rx.bytes[0] == 4 && rx.bytes[1] == 0x05 && rx.used >= 7) {
					disconnections++;
					say("bt: link: a disconnection: status %#x, handle %#x, reason %#x\n", rx.bytes[3],
					    (rx.bytes[4] | rx.bytes[5] << 8) & 0xfff, rx.bytes[6]);
				}
				if (rx.bytes[0] == 4 && rx.bytes[1] == 0x0e && rx.used >= 7 && rx.bytes[6] != 0 && events++ < 32) {
					say("bt: the chip refused command %#06x (status %#x)\n", rx.bytes[4] | rx.bytes[5] << 8, rx.bytes[6]);
				}
			}
		}
		if (mach_absolute_time() >= next_report) {
			if (acl_out != said_out || acl_in != said_in || connections != said_conn || disconnections != said_disc) {
				say("bt: link: %u connections, %u disconnections; ACL out %llu KB (%llu KB/s), in %llu KB\n", connections,
				    disconnections, acl_out / 1024, (acl_out - said_out) / 10240, acl_in / 1024);
				for (unsigned k = 0; k < nh; k++) {
					say("bt: link:   handle %#x: out %llu bytes, in %llu bytes\n", handles[k], h_out[k], h_in[k]);
				}
				said_out = acl_out;
				said_in = acl_in;
				said_conn = connections;
				said_disc = disconnections;
			}
			clock_interval_to_deadline(10, NSEC_PER_SEC, &next_report);
		}
		if (!(unit && (tx.complete || txq_head != txq_tail)) && !have_rx && rx_head == rx_tail) {
			delay(500);
		}
	}
}

static bool
runtime_start(void)
{
	struct kern_ctl_reg registration;
	uint8_t event[80], commands[64] = { 0 }, address[6];
	thread_t thread;
	unsigned n;
	int error;

	n = hci_command("Read Local Supported Commands", 0x1002, NULL, 0, event, sizeof(event));
	if (n >= 71) {
		memcpy(commands, event + 7, 64);
	}
	for (unsigned i = 0; i < 6; i++) {
		address[i] = bd_address_lsb[5 - i];     /* display order, as bluetoothd reads local-mac-address */
	}
	memset(&registration, 0, sizeof(registration));
	strlcpy(registration.ctl_name, "com.hakios.bluetooth", sizeof(registration.ctl_name));
	registration.ctl_flags = CTL_FLAG_REG_SOCK_STREAM;
	registration.ctl_sendsize = TXQ_SIZE;
	registration.ctl_recvsize = TXQ_SIZE;
	registration.ctl_connect = ctl_connect;
	registration.ctl_disconnect = ctl_disconnect;
	registration.ctl_send = ctl_send;
	registration.ctl_getopt = ctl_getopt;
	registration.ctl_setopt = ctl_setopt;
	ctl_termios.c_cflag = CS8 | CREAD | CLOCAL;
	ctl_termios.c_ispeed = ctl_termios.c_ospeed = BT_SPEED;
	if ((error = ctl_register(&registration, &ctl_ref)) != 0) {
		say("bt: the kernel control could not be registered: %d\n", error);
		return false;
	}
	if ((error = hakios_bt_sunfish_publish(address, commands, BT_SPEED)) != 0) {
		say("bt: the IOService could not be published: %d\n", error);
		ctl_deregister(ctl_ref);
		return false;
	}
	if (kernel_thread_start(ctl_worker, NULL, &thread) != KERN_SUCCESS) {
		ctl_deregister(ctl_ref);
		return false;
	}
	thread_deallocate(thread);
	hakios_bt_sunfish_set_ready(true);
	say("bt: BLUETOOTH IS DARWIN'S: com.hakios.bluetooth ready at %u baud, the \"bluetooth\" service published\n", BT_SPEED);
	return true;
}

static void
talk(void)
{
	/*
	 * The chip answers only once its supplies are on, which happens some time into the boot and not from the bootloader
	 * (at 3 s it said nothing, logs/161520-run.log): with bt=5 it is asked again every 3 s, for 90 s at most.
	 */
	for (unsigned tries = 1; !chip_up(); tries++) {
		chip_off();
		if (level < 5 || tries >= 30) {
			say("bt: the chip did not say its version\n");
			return;
		}
		say("bt: the chip does not answer yet (try %u): again in 3 s\n", tries);
		delay_for_interval(3, NSEC_PER_SEC);
	}
	if (level == 4) {
		if (chip_firmware()) {
			chip_scan();
		}
		chip_off();
	} else if (level >= 5) {
		if (!chip_firmware() || !runtime_start()) {
			chip_off();
		}
	} else {
		chip_off();
	}
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

static thread_call_t bt_call, bt_publish_call;

/*
 * The "bluetooth" service published long before the chip is up, not ready, with the address but no commands (the
 * chip's own are put in by runtime_start): bluetoothd, started by launchd at about 25 s, looks for it and, not finding
 * it, said "No bluetooth on this device" and quit, twice more, while SpringBoard tried to attach in between ("error 9")
 * and never tried again: no Bluetooth in Control Center (logs/002224-run.log). With the service there it stays, and
 * BlueTool's stand-in holds it until HakiosBluetoothReady. (inject_bluetooth.c reads the commands as bluetoothd starts,
 * for one patch, and makes the same choice with none as with this chip's.)
 */
static void
bt_publish(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint8_t commands[64] = { 0 }, address[6];
	int error;

	bd_address();
	for (unsigned i = 0; i < 6; i++) {
		address[i] = bd_address_lsb[5 - i];
	}
	if ((error = hakios_bt_sunfish_publish(address, commands, BT_SPEED)) != 0) {
		say("bt: the IOService could not be published early: %d\n", error);
	}
}

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
	/*
	 * 30 s: started at 3 s the chip did not answer and the driver stalled in its first exchange (logs/162013-run.log);
	 * from 30 s on it has always worked. BlueTool's stand-in waits for the controller meanwhile (userland/src/umi_bluetool.c),
	 * so bluetoothd does not come and go and SpringBoard can attach. bt_delay=S changes it.
	 */
	uint32_t delay_s = 30;

	PE_parse_boot_argn("bt_delay", &delay_s, sizeof(delay_s));
	if (level >= 5) {
		bt_publish_call = thread_call_allocate(bt_publish, NULL);
		clock_interval_to_deadline(5, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(bt_publish_call, deadline);
	}
	bt_call = thread_call_allocate(bt_go, NULL);
	clock_interval_to_deadline(MIN(delay_s, 120u), NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(bt_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_bt_sunfish_init);
