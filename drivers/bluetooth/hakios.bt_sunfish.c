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
	n = hci_event(event, capacity, 1000);
	say_event(what, event, n);
	return n;
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
