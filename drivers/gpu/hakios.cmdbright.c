/*
 * The Pixel 4a panel's brightness: DCS 0x51 with a 10-bit level, high byte first, as a long write through DSI0's command
 * FIFO (no memory, no SMMU), as Linux's dsi_ctrl_hw_cmn_kickoff_fifo_command does. No PWM or WLED on this phone.
 *
 * Only with boot-arg panel_brightness=N (4..1023): N a few seconds after the kernel starts. Then the slider in iOS
 * (hakios.cmdbright.cpp) and kern.hakios.panel_brightness set it. Nothing is sent unless DSI0 is on in command mode.
 *
 * Writes to no PMIC: only DSI0's registers, and to the panel only this one command.
 *
 * Where it comes from: sweet_fb_set_brightness() in SweetFramebuffer.cpp of ios-sunfish, which ran on this phone from
 * ABL (2026-10-28), and docs/SUNFISH-BACKLIGHT.md there. Status: RUN ON THE PHONE through U-Boot, iOS 16.5 (2026-11-05): the user
 * saw it dim during boot.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/sysctl.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void HakiosBacklightStart(int level);
extern void delay(int usec);

#define DSI0_BASE       0x0AE94000UL
#define DSI_CTRL        0x004
#define DSI_DMA_CTRL    0x03C
#define DSI_DMA_LENGTH  0x04C
#define DSI_DMA_TRIGGER 0x090
#define DSI_INT_CTRL    0x110
#define DSI_INT_KEEP    0xA222AA02u     /* the enables and the error mask, no other status bit */
#define DSI_TPG_CTRL    0x15C
#define DSI_TPG_DMA_INIT 0x17C
#define DSI_TPG_FIFO_RST 0x1EC

#define LEVEL_MIN       4
#define LEVEL_MAX       1023
#define START_SECONDS   3

static LCK_GRP_DECLARE(cmdbright_group, "hakios.cmdbright");
static LCK_MTX_DECLARE(cmdbright_lock, &cmdbright_group);
static vm_offset_t dsi;
static int first_level, level_now = -1;
static thread_call_t start_call;
static bool panel_asleep;

int hakios_panel_brightness(int level);

/* One packet through DSI0's command FIFO: words[0] the header (data0, data1, type, flags: BIT 7 last, BIT 6 long), then the payload. */
static bool
dsi_kick(volatile uint32_t *r, const uint32_t *words, unsigned n)
{
	uint32_t dma;
	bool done = false;

	r[DSI_INT_CTRL / 4] = (r[DSI_INT_CTRL / 4] & DSI_INT_KEEP) | 1u;
	r[DSI_TPG_CTRL / 4] = 0x00030006u;
	for (unsigned i = 0; i < n; i++) {
		r[DSI_TPG_DMA_INIT / 4] = words[i];
	}
	dma = r[DSI_DMA_CTRL / 4];
	r[DSI_DMA_CTRL / 4] = (dma & ~(1u << 31 | 1u << 30 | 1u << 26)) | 1u << 28;
	r[DSI_DMA_LENGTH / 4] = 4 * n;
	__asm__ volatile("dsb sy" ::: "memory");
	r[DSI_DMA_TRIGGER / 4] = 1;
	for (unsigned i = 0; i < 500 && !done; i++) {
		done = (r[DSI_INT_CTRL / 4] & 1u) != 0;
		if (!done) {
			delay(100);
		}
	}
	r[DSI_INT_CTRL / 4] = (r[DSI_INT_CTRL / 4] & DSI_INT_KEEP) | 1u;
	r[DSI_TPG_CTRL / 4] = 0;
	r[DSI_TPG_FIFO_RST / 4] = 1;
	delay(1);
	r[DSI_TPG_FIFO_RST / 4] = 0;
	r[DSI_DMA_CTRL / 4] = dma;
	__asm__ volatile("dsb sy" ::: "memory");
	return done;
}

static int panel_send(int level);
static int level_ios = -1;              /* what iOS asked for last (the slider), whoever drives the panel */
static bool auto_drives(void);
static void auto_goal_now(void);

/* iOS's brightness: the panel's level, or with Auto-Brightness driving it, the slider that scales the curve. */
int
hakios_panel_brightness(int level)
{
	level = level < LEVEL_MIN ? LEVEL_MIN : level > LEVEL_MAX ? LEVEL_MAX : level;
	level_ios = level;
	if (auto_drives()) {
		auto_goal_now();
		return 0;
	}
	return panel_send(level);
}

static int
panel_send(int level)
{
	volatile uint32_t *r = (volatile uint32_t *)dsi;
	uint32_t ctrl, words[2];
	bool done;

	if (dsi == 0) {
		return ENODEV;
	}
	level = level < LEVEL_MIN ? LEVEL_MIN : level > LEVEL_MAX ? LEVEL_MAX : level;
	lck_mtx_lock(&cmdbright_lock);
	if (panel_asleep) {             /* sent when it wakes */
		level_now = level;
		lck_mtx_unlock(&cmdbright_lock);
		return 0;
	}
	ctrl = r[DSI_CTRL / 4];
	if ((ctrl & 0x5u) != 0x5u) {
		lck_mtx_unlock(&cmdbright_lock);
		printf("cmdbright: DSI0 is not on in command mode (DSI_CTRL %#x): nothing sent\n", ctrl);
		return ENXIO;
	}
	words[0] = 0xC0390003u;         /* long write, DT 0x39, 3 bytes, last command */
	words[1] = 0xFF000051u | ((uint32_t)level & 0xffu) << 16 | ((uint32_t)level >> 8) << 8;
	done = dsi_kick(r, words, 2);
	if (done) {
		level_now = level;
	}
	lck_mtx_unlock(&cmdbright_lock);
	printf("cmdbright: level %d %s\n", level, done ? "sent" : "NOT acknowledged (50 ms)");
	return done ? 0 : ETIMEDOUT;
}

/*
 * The panel off at screen off and back at screen on (panel_power=1): iOS's display power request reaches
 * MiniIOMobileFramebuffer (hakios_panel_power_hook). Off: the pingpongs' autorefresh stopped, DCS 0x28 display off and
 * 0x10 enter sleep. On: 0x11 sleep out, 120 ms, 0x29 display on, autorefresh as it was, the brightness again. Measured
 * (2026-11-19, idle): 570 mA lit, 424 with the panel off. The display clocks stay on (10 mA more, and a risk on wake).
 * All as one-byte long writes: 4-byte short packets through this FIFO were acknowledged but the panel ignored them.
 */
extern int (*hakios_panel_power_hook)(uint64_t state);
bool hakios_panel_follows_power(void);

bool
hakios_panel_follows_power(void)
{
	return hakios_panel_power_hook != NULL;
}
static vm_offset_t pingpongs;
static uint32_t autorefresh_was[4];

static bool
dcs1(volatile uint32_t *r, uint8_t command)
{
	uint32_t words[2] = { 0xC0390001u, 0xFFFFFF00u | command };

	return dsi_kick(r, words, 2);
}

/* The panel to sleep when the kernel hands back to the loader (loader_return.c), so the last iOS frame does not burn
 * into the OLED while the phone sits at the loader. Best-effort; DSI must be on in command mode. */
void
hakios_panel_blank(void)
{
	volatile uint32_t *r = (volatile uint32_t *)dsi;

	if (r == NULL || (r[DSI_CTRL / 4] & 0x5u) != 0x5u || panel_asleep) {
		return;
	}
	lck_mtx_lock(&cmdbright_lock);
	(void)dcs1(r, 0x28);            /* display off */
	delay(20000);
	(void)dcs1(r, 0x10);            /* enter sleep */
	panel_asleep = true;
	lck_mtx_unlock(&cmdbright_lock);
}

static int
panel_power(uint64_t state)
{
	volatile uint32_t *r = (volatile uint32_t *)dsi, *pp;
	bool on = state != 0, a, b;

	if (r == NULL) {
		return ENODEV;
	}
	if (pingpongs == 0) {
		pingpongs = ml_io_map(0x0ae70000UL, 0x4000);    /* MDP 0xae01000 + 0x70000 + 0x800 n */
	}
	pp = (volatile uint32_t *)pingpongs;
	lck_mtx_lock(&cmdbright_lock);
	if (on == !panel_asleep || (r[DSI_CTRL / 4] & 0x5u) != 0x5u) {
		lck_mtx_unlock(&cmdbright_lock);
		return 0;
	}
	if (!on) {
		for (unsigned i = 0; pp != NULL && i < 4; i++) {
			autorefresh_was[i] = pp[(0x1000u + 0x800u * i + 0x030u) / 4];
			pp[(0x1000u + 0x800u * i + 0x030u) / 4] = 0;
		}
		__asm__ volatile("dsb sy" ::: "memory");
		delay(20000);   /* a frame in flight ends */
		a = dcs1(r, 0x28);
		delay(20000);
		b = dcs1(r, 0x10);
		panel_asleep = true;
		lck_mtx_unlock(&cmdbright_lock);
		printf("cmdbright: panel off (display off %s, sleep %s)\n", a ? "sent" : "NOT acked", b ? "sent" : "NOT acked");
		return 0;
	}
	a = dcs1(r, 0x11);
	delay(120000);  /* the panel's wake time */
	b = dcs1(r, 0x29);
	for (unsigned i = 0; pp != NULL && i < 4; i++) {
		if (autorefresh_was[i] & 0x80000000u) {
			pp[(0x1000u + 0x800u * i + 0x030u) / 4] = autorefresh_was[i];
		}
	}
	__asm__ volatile("dsb sy" ::: "memory");
	panel_asleep = false;
	lck_mtx_unlock(&cmdbright_lock);
	printf("cmdbright: panel on (sleep out %s, display on %s)\n", a ? "sent" : "NOT acked", b ? "sent" : "NOT acked");
	if (level_now > 0) {
		(void)panel_send(level_now);
		if (auto_drives()) {
			auto_goal_now();        /* the light may have changed while it slept */
		}
	}
	return 0;
}

/*
 * For the battery: the display really off at disp_off_s seconds (default 110), and nothing touches DSI0 after. Only for a
 * measurement: the screen does not come back until the next boot.
 *   disp_off=1  the pingpongs' autorefresh off, then DCS 0x28 (display off) and 0x10 (enter sleep) to the panel
 *   disp_off=2  and then the display clocks gated: MDP, rotator, LUT, vsync, pclk0, byte0, byte0_intf, esc0 (dispcc)
 *   disp_off=3  and then the MDSS power domain collapsed (its GDSCR, SW_COLLAPSE)
 */
#define DISPCC_BASE     0x0AF00000UL
#define GCC_DISP_AHB    0x0010B00CUL
static uint32_t disp_off, disp_off_s = 110;
static thread_call_t disp_off_call;

static void
disp_off_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	static const uint32_t gated[] = { 0x200c, 0x2024, 0x201c, 0x2014, 0x2004, 0x2028, 0x202c, 0x2038 };
	volatile uint32_t *r = (volatile uint32_t *)dsi, *mdp, *cc, *gcc;
	uint32_t words[2];
	bool off, sleep;

	if (r == 0 || (r[DSI_CTRL / 4] & 0x5u) != 0x5u) {
		printf("disp_off: DSI0 is not on in command mode: nothing done\n");
		return;
	}
	lck_mtx_lock(&cmdbright_lock);
	mdp = (volatile uint32_t *)ml_io_map(0x0ae70000UL, 0x4000);  /* the pingpongs: MDP 0xae01000 + 0x70000 + 0x800 n */
	for (unsigned pp = 0; mdp != NULL && pp < 4; pp++) {
		mdp[(0x1000u + 0x800u * pp + 0x030u) / 4] = 0;
	}
	__asm__ volatile("dsb sy" ::: "memory");
	delay(50000);   /* a frame in flight ends */
	/* as long writes (DT 0x39, one byte), the shape the brightness command has: a 4-byte short packet through this FIFO
	 * was acknowledged by the host but the panel stayed lit (user, 2026-11-19) */
	words[0] = 0xC0390001u;
	words[1] = 0xFFFFFF28u;         /* 0x28 display off */
	off = dsi_kick(r, words, 2);
	delay(20000);
	words[1] = 0xFFFFFF10u;         /* 0x10 enter sleep */
	sleep = dsi_kick(r, words, 2);
	delay(120000);
	dsi = 0;        /* no more brightness: hakios_panel_brightness says ENODEV */
	printf("disp_off: autorefresh off; display off %s, sleep %s\n", off ? "sent" : "NOT acknowledged",
	    sleep ? "sent" : "NOT acknowledged");
	if (disp_off >= 2) {
		gcc = (volatile uint32_t *)ml_io_map(GCC_DISP_AHB & ~0x3fffUL, 0x4000);
		cc = (volatile uint32_t *)ml_io_map(DISPCC_BASE, 0x10000);
		if (gcc == NULL || cc == NULL || !(gcc[(GCC_DISP_AHB & 0x3fffUL) / 4] & 0x3u)) {
			printf("disp_off: dispcc not reachable (gcc_disp_ahb %#x): no clock gated\n",
			    gcc != NULL ? gcc[(GCC_DISP_AHB & 0x3fffUL) / 4] : 0);
		} else {
			for (unsigned i = 0; i < sizeof(gated) / sizeof(gated[0]); i++) {
				uint32_t was = cc[gated[i] / 4];

				cc[gated[i] / 4] = was & ~1u;
				__asm__ volatile("dsb sy" ::: "memory");
				delay(50);
				printf("disp_off: dispcc %#x %#x -> %#x\n", gated[i], was, cc[gated[i] / 4]);
			}
			if (disp_off >= 3) {
				uint32_t was = cc[0x3000 / 4];

				cc[0x3000 / 4] = was | 1u;
				__asm__ volatile("dsb sy" ::: "memory");
				delay(1000);
				printf("disp_off: MDSS GDSCR %#x -> %#x (bit 31: powered)\n", was, cc[0x3000 / 4]);
			}
		}
	}
	lck_mtx_unlock(&cmdbright_lock);
}

/*
 * Auto-Brightness (the way Evsio0n/instantnoodle-xnu does it on the OnePlus 8, not through CoreBrightness, which
 * crashes on a light sensor of ours): hakios-sensors writes the light (kern.hakios.auto_lux, milli-lux, ~10 a second),
 * SpringBoard's libhakios writes the stock switch (Settings > Accessibility > Display & Text Size > Auto-Brightness,
 * kern.hakios.auto_enabled). While both are there the panel follows a curve of the light: smoothed over 250 ms, a new
 * goal only after it has held for 1.5 s (or at once when the slider moves), reached in steps of at most 8 every 20 ms.
 * The slider scales the curve (middle, level 226: as is; ends: about x2 and /2). Not while the panel sleeps.
 */
static const struct { uint32_t lux, level; } auto_curve[] = {
	{ 0, 8 }, { 5, 30 }, { 20, 80 }, { 50, 150 }, { 100, 220 }, { 300, 380 }, { 1000, 600 }, { 3000, 850 }, { 10000, 1023 },
};
#define AUTO_SLIDER_MID         226
static int auto_enabled;                /* the stock switch, from SpringBoard */
static uint64_t auto_lux_at;            /* when the last light came (mach time): stale after 5 s */
static uint32_t auto_lux_f;             /* smoothed, milli-lux */
static int auto_goal = -1, auto_cand = -1;
static uint64_t auto_cand_at;
static thread_call_t auto_call;
static bool auto_ramping;

static uint64_t
auto_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / 1000000;
}

static bool
auto_drives(void)
{
	return auto_enabled && auto_lux_at != 0 && auto_ms() - auto_lux_at < 5000;
}

static uint32_t
isqrt(uint32_t v)
{
	uint32_t r = 0;

	for (uint32_t b = 1u << 30; b != 0; b >>= 2) {
		if (v >= r + b) {
			v -= r + b;
			r = (r >> 1) + b;
		} else {
			r >>= 1;
		}
	}
	return r;
}

static int
adiff(int a, int b)
{
	return a > b ? a - b : b - a;
}

static int
auto_level_for(uint32_t mlux)
{
	uint32_t lux = mlux / 1000, n = sizeof(auto_curve) / sizeof(auto_curve[0]), base = auto_curve[n - 1].level;
	int slider = level_ios > 0 ? level_ios : AUTO_SLIDER_MID, level;

	for (uint32_t i = 1; i < n; i++) {
		if (lux < auto_curve[i].lux) {
			base = auto_curve[i - 1].level + (auto_curve[i].level - auto_curve[i - 1].level) * (lux - auto_curve[i - 1].lux) /
			    (auto_curve[i].lux - auto_curve[i - 1].lux);
			break;
		}
	}
	/* x sqrt(slider / middle): the slider's level is itself already a curve of its position */
	level = (int)(base * isqrt((uint32_t)slider * 1024 / AUTO_SLIDER_MID) / 32);
	return level < LEVEL_MIN ? LEVEL_MIN : level > LEVEL_MAX ? LEVEL_MAX : level;
}

static void
auto_ramp(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;
	int now, step;

	lck_mtx_lock(&cmdbright_lock);
	now = level_now;
	lck_mtx_unlock(&cmdbright_lock);
	if (!auto_drives() || panel_asleep || auto_goal < 0 || now < 0 || now == auto_goal) {
		auto_ramping = false;
		return;
	}
	step = auto_goal - now;
	step = step > 8 ? 8 : step < -8 ? -8 : step;
	(void)panel_send(now + step);
	clock_interval_to_deadline(20, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(auto_call, deadline);
}

static void
auto_start_ramp(void)
{
	if (!auto_ramping && auto_call != NULL) {
		auto_ramping = true;
		thread_call_enter(auto_call);
	}
}

/* The slider moved (or the switch came on): the goal at once. */
static void
auto_goal_now(void)
{
	if (auto_lux_at == 0) {
		return;
	}
	auto_goal = auto_cand = auto_level_for(auto_lux_f);
	auto_start_ramp();
}

static void
auto_light(uint32_t mlux)
{
	uint64_t now = auto_ms();
	int cand;

	/* 250 ms smoothing: each new reading moves the average by dt / 250 of the way (dt at most 250 ms) */
	if (auto_lux_at == 0 || now - auto_lux_at > 5000) {
		auto_lux_f = mlux;
	} else {
		uint64_t dt = now - auto_lux_at > 250 ? 250 : now - auto_lux_at;

		auto_lux_f = (uint32_t)(((uint64_t)auto_lux_f * (250 - dt) + (uint64_t)mlux * dt) / 250);
	}
	auto_lux_at = now;
	if (!auto_enabled) {
		return;
	}
	cand = auto_level_for(auto_lux_f);
	if (auto_goal < 0) {
		auto_goal = auto_cand = cand;
		auto_start_ramp();
		return;
	}
	/* a new goal only when it differs by more than 10 % (8 at least) and has held for 1.5 s */
	if (adiff(cand, auto_goal) <= (auto_goal / 10 > 8 ? auto_goal / 10 : 8)) {
		auto_cand = auto_goal;
		return;
	}
	if (adiff(cand, auto_cand) > (auto_cand / 10 > 8 ? auto_cand / 10 : 8)) {
		auto_cand = cand;
		auto_cand_at = now;
	} else if (now - auto_cand_at >= 1500) {
		auto_goal = cand;
		auto_start_ramp();
	}
}

static int
sysctl_auto_lux(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = (int)auto_lux_f, error = sysctl_handle_int(oidp, &value, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	auto_light(value < 0 ? 0 : (uint32_t)value);
	return 0;
}

static int
sysctl_auto_enabled(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = auto_enabled, error = sysctl_handle_int(oidp, &value, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	if ((value != 0) != (auto_enabled != 0)) {
		auto_enabled = value != 0;
		printf("cmdbright: Auto-Brightness %s\n", auto_enabled ? "on" : "off");
		if (auto_enabled) {
			auto_goal_now();
		} else if (level_ios > 0) {
			(void)panel_send(level_ios);    /* back to the slider */
		}
	}
	return 0;
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, auto_lux, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    sysctl_auto_lux, "I", "the light, milli-lux (hakios-sensors writes it)");
SYSCTL_PROC(_kern_hakios, OID_AUTO, auto_enabled, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_ANYBODY | CTLFLAG_LOCKED, NULL, 0,
    sysctl_auto_enabled, "I", "the stock Auto-Brightness switch (SpringBoard's libhakios writes it)");

static int
sysctl_panel_brightness(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value = level_now, error = sysctl_handle_int(oidp, &value, 0, req);

	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	return hakios_panel_brightness(value);
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, panel_brightness, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0,
    sysctl_panel_brightness, "I", "the panel's level, 4..1023; -1: none sent yet");

static void
start_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	auto_call = thread_call_allocate(auto_ramp, NULL);
	dsi = ml_io_map(DSI0_BASE, 0x1000);
	if (dsi == 0) {
		printf("cmdbright: could not map DSI0\n");
		return;
	}
	(void)hakios_panel_brightness(first_level);
	HakiosBacklightStart(first_level);
	uint32_t power = 0;

	if (PE_parse_boot_argn("panel_power", &power, sizeof(power)) && power != 0) {
		hakios_panel_power_hook = panel_power;
		printf("cmdbright: the panel follows iOS's display power\n");
	}
}

__startup_func
static void
cmdbright_init(void)
{
	uint64_t deadline;

	if (!PE_parse_boot_argn("panel_brightness", &first_level, sizeof(first_level)) || first_level <= 0) {
		return;
	}
	if (PE_parse_boot_argn("disp_off", &disp_off, sizeof(disp_off)) && disp_off != 0) {
		(void)PE_parse_boot_argn("disp_off_s", &disp_off_s, sizeof(disp_off_s));
		disp_off_call = thread_call_allocate(disp_off_run, NULL);
		clock_interval_to_deadline(disp_off_s, NSEC_PER_SEC, &deadline);
		thread_call_enter_delayed(disp_off_call, deadline);
	}
	start_call = thread_call_allocate(start_run, NULL);
	clock_interval_to_deadline(START_SECONDS, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(start_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, cmdbright_init);
