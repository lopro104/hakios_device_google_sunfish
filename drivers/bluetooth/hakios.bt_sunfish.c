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
	vm_offset_t uart = ml_io_map(UART_PA, 0x4000);

	if (uart != 0) {
		say("bt: the serial engine: GENI_FW_REVISION %#x, GENI_STATUS %#x, SE_HW_PARAM_0 %#x, protocol %u\n", rd(uart + 0x68),
		    rd(uart + 0x40), rd(uart + 0xe24), (rd(uart + 0x68) >> 8) & 0xff);
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

static void
bt_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("bt: level %u\n", level);
	look();
	if (level >= 2 && clock_on()) {
		vm_offset_t uart = ml_io_map(UART_PA, 0x4000);

		pins_to_uart();
		if (uart != 0) {
			uint32_t fw = rd(uart + 0x68);

			say("bt: the serial engine: GENI_FW_REVISION %#x (protocol %u%s, version %#x), GENI_STATUS %#x, SE_HW_PARAM_0 %#x\n",
			    fw, (fw >> 8) & 0xff, ((fw >> 8) & 0xff) == 2 ? ": A UART" : ((fw >> 8) & 0xff) == 0 ? ": NONE LOADED" : "",
			    fw & 0xff, rd(uart + 0x40), rd(uart + 0xe24));
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
