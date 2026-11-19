/*
 * The clock of the SM7150's cores (Pixel 4a) for drivers/power/hakios.cpufreq.c, whose own registers ("EPSS") are the reference
 * phone's. This SoC has the older "OSM" (Linux qcom-cpufreq-hw, the sdm845-style register set, sm7150.dtsi cpufreq@18323000): a table of
 * frequencies per group, built by the firmware, and hardware that switches clock and voltage when told which row is wanted.
 *   group 0, the six Cortex-A55 (cpu0-5) at 0x18323000; group 1, the two Cortex-A76 (cpu6-7) at 0x18325800
 *   0x000 enable: bit 0, the firmware runs this group
 *   0x110 + 32 n: row n: [31:30] clock source (0: 300 MHz, else the PLL), [18:16] core count (1: a boost row), [7:0] x 19.2 MHz
 *   0x114 + 32 n: its voltage, [11:0] in mV
 *   0x704: what runs now, [9:0] x 19.2 MHz      0x920: the row that is wanted
 *
 * Every group with a core of ours goes to its top row, boost rows left out as Linux leaves them out. Only with cpufreq=1 (the caller).
 * Hot cores: see the governor at the end of this file; beyond it, the stop of
 * hakios.thermal.c at 105 stays. cpufreq=1 is not in this device's default arguments.
 *
 * Also the command database's place for the memory vote: the firmware says where it is (the same locator as Linux's cmd-db.c through
 * aop_cmd_db, and bsd/dev/sweet_rpmh.c); the reference phone's fixed address is another memory here. Checked before use, RAM is never
 * scanned.
 *
 * Status: the raise and the vote ran on the phone from sweet_cpufreq.c and sweet_rpmh.c of ios-sunfish (A55 1804, A76 2208 MHz,
 * booted by ABL). THIS FILE: compiled only, not yet run.
 */
#include <kern/clock.h>
#include <kern/thread_call.h>
#include <stdarg.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.thermal_sensors.h>
#include <sys/sysctl.h>
#include <sys/hakios.input.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void hakios_cpufreq_osm_raise(void);
extern uint64_t hakios_cpufreq_osm_cmd_db(void);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);

#define OSM_DOMAINS             2
#define OSM_DOMAIN_SIZE         0x1400UL
#define OSM_ENABLE              0x000
#define OSM_FREQ_LUT            0x110
#define OSM_VOLT_LUT            0x114
#define OSM_LUT_ROW             32
#define OSM_LUT_ROWS            40
#define OSM_CURRENT_VOTE        0x704
#define OSM_PERF_STATE          0x920
#define LUT_TURBO_IND           1
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000

#define CMD_DB_LOCATOR_PAGE     0x0c3f0000UL
#define CMD_DB_LOCATOR          0xc             /* two words in that page: the address and the size */
#define CMD_DB_MAX              0x40000u

static const uint64_t osm_phys[OSM_DOMAINS] = { 0x18323000ULL, 0x18325800ULL };
static const char *const osm_name[OSM_DOMAINS] = { "A55 group (cpu0-5)", "A76 group (cpu6-7)" };

extern void loader_log_putc(char c);

/* Into the loader's log as well as the kernel's: the kernel's early and middle lines may be dropped from a long run's log. */
static void
say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char line[200];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	printf("%s", line);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

static uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

static uint32_t row_khz[OSM_DOMAINS][OSM_LUT_ROWS];
static vm_offset_t osm_base[OSM_DOMAINS];
static int top_row_of[OSM_DOMAINS] = { -1, -1 }, cap[OSM_DOMAINS];
static void governor_start(void);

/* The highest row that is not a boost row, or -1 when the firmware does not run this group. */
static int
top_row(vm_offset_t base, uint32_t domain, uint32_t *top_khz)
{
	uint32_t prev = 0;
	int top = -1;

	if (!(rd(base, OSM_ENABLE) & 1u)) {
		printf("cpufreq: %s: not enabled by the firmware (%#x); left alone\n", osm_name[domain], rd(base, OSM_ENABLE));
		return -1;
	}
	for (uint32_t row = 0; row < OSM_LUT_ROWS; row++) {
		uint32_t entry = rd(base, OSM_FREQ_LUT + OSM_LUT_ROW * row);
		uint32_t mv = rd(base, OSM_VOLT_LUT + OSM_LUT_ROW * row) & 0xfff;
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;
		uint32_t cores = (entry >> 16) & 7;

		if (row > 0 && khz == prev) {
			break;                  /* the table ends by repeating its last row */
		}
		say("cpufreq: %s row %u: %u MHz, %u mV%s\n", osm_name[domain], row, khz / 1000, mv,
		    cores == LUT_TURBO_IND ? " (boost: not used)" : "");
		row_khz[domain][row] = khz;
		if (cores != LUT_TURBO_IND && khz > *top_khz) {
			top = (int)row;
			*top_khz = khz;
		}
		prev = khz;
	}
	return top;
}

void
hakios_cpufreq_osm_raise(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t cores[OSM_DOMAINS] = { 0 };

	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		cores[((topology->cpus[i].phys_id >> 8) & 0xff) < 6 ? 0 : 1]++;     /* MPIDR Aff1 is the core's number */
	}
	for (uint32_t d = 0; d < OSM_DOMAINS; d++) {
		vm_offset_t base = ml_io_map(osm_phys[d], OSM_DOMAIN_SIZE);
		uint32_t was, top_khz = 0;
		int top;

		if (base == 0) {
			printf("cpufreq: could not map %s\n", osm_name[d]);
			continue;
		}
		top = top_row(base, d, &top_khz);
		was = rd(base, OSM_PERF_STATE);
		say("cpufreq: %s, as the boot loader left it: row %u, running %u MHz\n", osm_name[d], was,
		    XO_KHZ * (rd(base, OSM_CURRENT_VOTE) & 0x3ff) / 1000);
		if (top < 0 || cores[d] == 0) {
			continue;
		}
		*(volatile uint32_t *)(base + OSM_PERF_STATE) = (uint32_t)top;
		delay_for_interval(20, NSEC_PER_MSEC);
		say("cpufreq: %s, %u of our cores: row %u -> %d (%u MHz), now running %u MHz\n", osm_name[d], cores[d], was, top,
		    top_khz / 1000, XO_KHZ * (rd(base, OSM_CURRENT_VOTE) & 0x3ff) / 1000);
		osm_base[d] = base;
		top_row_of[d] = top;
		cap[d] = top;
	}
	governor_start();
}

/* Where the firmware left the command database, 0 if what it says cannot be right. */
uint64_t
hakios_cpufreq_osm_cmd_db(void)
{
	vm_offset_t page = ml_io_map(CMD_DB_LOCATOR_PAGE, 0x1000);
	uint32_t pa, size;

	if (page == 0) {
		return 0;
	}
	pa = rd(page, CMD_DB_LOCATOR);
	size = rd(page, CMD_DB_LOCATOR + 4);
	if (pa < 0x80000000u || (pa & 3u) || size < 0x1000u || size > CMD_DB_MAX || (uint64_t)pa + size > 0x100000000ULL) {
		printf("ddr: the firmware's locator of the command database makes no sense (%#x, %#x)\n", pa, size);
		return 0;
	}
	return pa;
}

/*
 * The governor: slows a group of cores when its sensors are hot, and speeds it up again. The rules are the old tree's
 * (bsd/sys/sweet_thermal.h of ios-sunfish, from HackiOS's hk_thermal.h), which ran on this phone. Every half second:
 *   at or past the limit      a row down; 5 degrees or more past it, three rows down
 *   5 degrees under the limit for 4 looks in a row    a row up, to the top row at the most
 * It reads the sensors itself (one load each, as hakios.thermal.c does), and writes only the row a group is to run at.
 * A55: cpu-0-*, cpuss-*. A76: cpu-1-*, and gpuss-*: nothing here sets the GPU's clock, so a hot GPU slows the big cores, which
 * lessens what it is asked to draw and what is near it; it does not slow the GPU itself. So a GPU heating alone is not governed:
 * the stop of hakios.thermal.c at 105 stays for that.
 * With no reading for 5 s a group goes to half its rows, and says so.
 * Boot-arg thermal_slow=<C> (40 to 110, default 95): the limit; thermal_slow=0: no governor.
 * Status: RUN ON THE PHONE (2026-11-05, logs/034326-run.log, thermal_slow=60): both groups stepped down as far as row 0 and 1 as
 * they passed 60 C, temperatures fell, and they stepped up again under 55 C. At the real limit, 95, not yet seen firing. The
 * "now running" number comes from OSM_CURRENT_VOTE, which read 2995 MHz whatever the row, so it says nothing here.
 */
#define TM_STATUS(n)            (0xa0 + 4 * (n))
#define TM_VALID                (1u << 21)
#define GOV_MS                  100             /* the load's look; the temperature's is every fifth of them (500 ms) */
#define GOV_THERMAL_EVERY       5
#define GOV_COOL_LOOKS          4
#define GOV_BLIND_LOOKS         10
#define GOV_BUSY_UP             60              /* percent busy (the busiest core of a group): straight to the top row */
#define GOV_BUSY_DOWN           25              /* under this: a row down each look */
#define GOV_MAX_CPUS            8

extern const struct hakios_thermal_sensor hakios_thermal_sm7150[HAKIOS_THERMAL_SM7150_SENSORS];

static vm_offset_t gov_tm[2];
static thread_call_t gov_call;
static int gov_limit_tenths = 950, gov_cool[OSM_DOMAINS], gov_blind[OSM_DOMAINS];
static int gov_throttles, gov_limit_c = 95;
SYSCTL_DECL(_kern_hakios);
SYSCTL_INT(_kern_hakios, OID_AUTO, thermal_throttles, CTLFLAG_RD | CTLFLAG_LOCKED, &gov_throttles, 0,
    "how many times a group of cores was slowed");

/* The hottest of a group's sensors, tenths of a degree; false if none has a reading. */
static bool
gov_hottest(uint32_t d, int *hottest, const char **name)
{
	bool any = false;

	for (unsigned i = 0; i < HAKIOS_THERMAL_SM7150_SENSORS; i++) {
		const struct hakios_thermal_sensor *z = &hakios_thermal_sm7150[i];
		bool mine = d == 0 ? (strncmp(z->name, "cpu-0-", 6) == 0 || strncmp(z->name, "cpuss", 5) == 0) :
		    (strncmp(z->name, "cpu-1-", 6) == 0 || strncmp(z->name, "gpuss", 5) == 0);
		uint32_t status;
		int value;

		if (!mine) {
			continue;
		}
		status = *(volatile uint32_t *)(gov_tm[z->tm == 0xc263000u ? 0 : 1] + TM_STATUS(z->number));
		value = (int32_t)(status << 20) >> 20;
		if ((status & TM_VALID) && value > -400 && value < 1600 && (!any || value > *hottest)) {
			*hottest = value;
			*name = z->name;
			any = true;
		}
	}
	return any;
}

/*
 * The load (cpu_gov, on unless cpu_gov=0). The firmware left every group at its top row and it stayed there: an idle phone ran
 * its big cores at 2.2 GHz and their voltage and sat at 76-81 C with 1-3 % of the processors busy (logs/115955-run.log). Every
 * 100 ms each group's busiest core is looked at (the scheduler's idle time, ml_cpu_idle_time): 60 % or more, the group goes to
 * its top row at once (a touch is not kept waiting); under 25 %, one row down a look. The temperature's limit above stays the
 * ceiling: the row written is the lower of the two.
 */
static uint32_t gov_load_on = 2, gov_ticks;
static int load_row[OSM_DOMAINS], written_row[OSM_DOMAINS];

/*
 * cpu_gov=2 (the default since 2026-11-20), the way Nik300/ios-viva's governor does it on its MediaTek phone: every 20 ms
 * (100 ms while a group idles at its lowest row) the busiest core of each group is looked at, and the group is put at the
 * slowest row at which that core would be 50 % busy or less (its busy share scaled by the clock now / the row's clock);
 * twice 80 % or more: the top row at once; going down only after 3 looks in a row that want lower. And a touch (the touch
 * screen's driver, hakios_cpufreq_boost) or the first frame after a pause (the framebuffer) put both groups at their top
 * row for a while, so the work a touch starts is not first measured at a low clock. cpu_gov=1: the rule before (60 % top,
 * under 25 % a row down a look); cpu_gov=0: the load not looked at.
 */
#define GOV_FAST_MS             20
#define GOV_TARGET              50
#define GOV_JUMP                80
#define GOV_DOWN_LOOKS          3
static int gov_high[OSM_DOMAINS], gov_lower[OSM_DOMAINS];
static uint64_t gov_boost_until;        /* mach time; 0: none */
static uint32_t gov_boosts;
SYSCTL_UINT(_kern_hakios, OID_AUTO, cpu_boosts, CTLFLAG_RD | CTLFLAG_LOCKED, &gov_boosts, 0, "touch and frame boosts of the cores");

void hakios_cpufreq_boost(uint32_t ms);

/* Both groups at their top row for `ms' (a touch: 250; a frame after a pause: less). Any context; cheap when one runs. */
void
hakios_cpufreq_boost(uint32_t ms)
{
	uint64_t until;

	if (gov_load_on != 2 || gov_call == NULL) {
		return;
	}
	clock_interval_to_deadline(ms, NSEC_PER_MSEC, &until);
	if (until <= gov_boost_until) {
		return;
	}
	if (gov_boost_until < mach_absolute_time()) {
		gov_boosts++;
		gov_boost_until = until;
		thread_call_cancel(gov_call);           /* a 100 ms idle look may be pending: look now */
		thread_call_enter(gov_call);
	} else {
		gov_boost_until = until;
	}
}

/* A frame presented (MiniIOMobileFramebuffer): the first after 200 ms without one starts something (a scroll, an
 * animation): 100 ms at the top. Frames in a row do nothing more (their load keeps the clock up by itself). */
extern void (*hakios_frame_hook)(void);
extern void (*hakios_touch_hook)(uint32_t type);
static uint64_t gov_last_frame;

/* a touch going down or moving (drivers/input/hakios.input.c): 250 ms at the top */
static void
gov_touch(uint32_t type)
{
	if (type != HAKIOS_TOUCH_UP) {
		hakios_cpufreq_boost(250);
	}
}

static void
gov_frame(void)
{
	uint64_t now = mach_absolute_time(), pause;

	clock_interval_to_absolutetime_interval(200, NSEC_PER_MSEC, &pause);
	if (now - gov_last_frame > pause) {
		hakios_cpufreq_boost(100);
	}
	gov_last_frame = now;
}

/*
 * The L3 cache's clock (on by default, l3_gov=0 to leave it; its table is said either way): the OSM's third domain at 0x18321000 (sdmmagpie.dtsi
 * qcom,cpucc osm_l3_base; the same registers as the cores' groups, as Linux's sdm845 OSM L3 driver reads them). The firmware
 * leaves it where it was; Linux moves it with the cores' memory latency. Here: it follows whichever group runs at the larger
 * share of its own range (row / top row), rounded up, so the cache is never slower than the cores need.
 */
#define OSM_L3_PHYS             0x18321000ULL
static vm_offset_t l3_base;
static uint32_t l3_on = 1, l3_khz[OSM_LUT_ROWS];      /* l3_gov=0: left where the firmware put it */
static int l3_top = -1, l3_written = -1;

static void
l3_start(void)
{
	uint32_t prev = 0;

	PE_parse_boot_argn("l3_gov", &l3_on, sizeof(l3_on));
	l3_base = ml_io_map(OSM_L3_PHYS, OSM_DOMAIN_SIZE);
	if (l3_base == 0 || !(rd(l3_base, OSM_ENABLE) & 1u)) {
		say("cpufreq: L3: not there or not enabled by the firmware: left alone\n");
		l3_on = 0;
		return;
	}
	for (uint32_t row = 0; row < OSM_LUT_ROWS; row++) {
		uint32_t entry = rd(l3_base, OSM_FREQ_LUT + OSM_LUT_ROW * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz == prev) {
			break;
		}
		l3_khz[row] = khz;
		l3_top = (int)row;
		prev = khz;
	}
	say("cpufreq: L3: %d rows, %u to %u MHz; it runs at row %u now; %s\n", l3_top + 1, l3_khz[0] / 1000,
	    l3_top >= 0 ? l3_khz[l3_top] / 1000 : 0, rd(l3_base, OSM_PERF_STATE), l3_on ? "it follows the cores (l3_gov=1)" : "left alone");
}

static void
l3_follow(void)
{
	int share = 0, row;

	if (!l3_on || l3_top <= 0) {
		return;
	}
	for (uint32_t d = 0; d < OSM_DOMAINS; d++) {
		if (top_row_of[d] > 0) {
			share = MAX(share, written_row[d] * 1000 / top_row_of[d]);
		}
	}
	row = (share * l3_top + 999) / 1000;
	row = row > l3_top ? l3_top : row;
	if (row != l3_written) {
		*(volatile uint32_t *)(l3_base + OSM_PERF_STATE) = (uint32_t)row;
		l3_written = row;
	}
}

/* The row the load wants (cpu_gov=2), from the busiest core's share at the row it ran at. */
static int
gov_row_for(uint32_t d, int busy)
{
	uint64_t now_khz = row_khz[d][written_row[d]];
	int top = top_row_of[d], want = load_row[d], need = top;

	if (busy >= GOV_JUMP) {
		if (++gov_high[d] >= 2) {
			gov_lower[d] = 0;
			return top;
		}
	} else {
		gov_high[d] = 0;
	}
	for (int r = 0; r <= top; r++) {
		if ((uint64_t)busy * now_khz <= (uint64_t)GOV_TARGET * row_khz[d][r]) {
			need = r;
			break;
		}
	}
	if (need > want) {
		gov_lower[d] = 0;
		return need;
	}
	if (need < want) {
		if (++gov_lower[d] >= GOV_DOWN_LOOKS) {
			gov_lower[d] = 0;
			return need;
		}
		return want;
	}
	gov_lower[d] = 0;
	return want;
}
static uint64_t gov_idle_before[GOV_MAX_CPUS], gov_then;

static void
gov_load(int busiest[OSM_DOMAINS])
{
	ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t now = mach_absolute_time(), span = now - gov_then;

	busiest[0] = busiest[1] = 0;
	for (unsigned i = 0; topology != NULL && i < topology->num_cpus && i < GOV_MAX_CPUS; i++) {
		uint64_t idle = 0, busy;
		uint32_t core = (topology->cpus[i].phys_id >> 8) & 0xff, d = core >= 6 ? 1 : 0;

		if (!ml_cpu_idle_time((int)topology->cpus[i].cpu_id, &idle)) {
			continue;
		}
		if (gov_then != 0 && span != 0) {
			busy = span > idle - gov_idle_before[i] ? span - (idle - gov_idle_before[i]) : 0;
			busiest[d] = MAX(busiest[d], (int)(busy * 100 / span));
		}
		gov_idle_before[i] = idle;
	}
	gov_then = now;
}

static void
gov_look(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	uint64_t deadline;
	int busiest[OSM_DOMAINS] = { 100, 100 };
	bool boosting = gov_load_on == 2 && mach_absolute_time() < gov_boost_until, idle_floor = true;
	uint64_t look_ms = GOV_MS, now_abs = mach_absolute_time();
	static uint64_t thermal_next;
	bool thermal;

	/* the temperature every 500 ms whatever the look's pace */
	if (now_abs >= thermal_next) {
		uint64_t step;

		clock_interval_to_absolutetime_interval(GOV_MS * GOV_THERMAL_EVERY, NSEC_PER_MSEC, &step);
		thermal_next = now_abs + step;
		thermal = true;
	} else {
		thermal = false;
	}
	gov_ticks++;

	if (gov_load_on) {
		gov_load(busiest);
	}
	for (uint32_t d = 0; d < OSM_DOMAINS; d++) {
		int hot = 0, want, row;
		const char *name = "?";

		if (top_row_of[d] < 0) {
			continue;
		}
		if (!gov_load_on) {
			load_row[d] = top_row_of[d];
		} else if (gov_load_on == 2) {
			load_row[d] = gov_row_for(d, busiest[d]);
		} else if (busiest[d] >= GOV_BUSY_UP) {
			load_row[d] = top_row_of[d];
		} else if (busiest[d] < GOV_BUSY_DOWN && load_row[d] > 0) {
			load_row[d]--;
		}
		row = MIN(boosting ? top_row_of[d] : load_row[d], cap[d]);
		if (load_row[d] > 0 || boosting) {
			idle_floor = false;
		}
		if (row != written_row[d]) {
			*(volatile uint32_t *)(osm_base[d] + OSM_PERF_STATE) = (uint32_t)row;
			written_row[d] = row;
		}
		if (!thermal) {
			continue;
		}
		want = cap[d];
		if (!gov_hottest(d, &hot, &name)) {
			if (++gov_blind[d] == GOV_BLIND_LOOKS) {
				want = top_row_of[d] / 2;
				say("cpufreq: %s: no temperature for 5 s: to row %d\n", osm_name[d], want);
			}
		} else {
			gov_blind[d] = 0;
			if (hot >= gov_limit_tenths) {
				want = cap[d] - (hot >= gov_limit_tenths + 50 ? 3 : 1);
				gov_cool[d] = 0;
			} else if (hot < gov_limit_tenths - 50) {
				if (++gov_cool[d] >= GOV_COOL_LOOKS) {
					want = cap[d] + 1;
					gov_cool[d] = 0;
				}
			} else {
				gov_cool[d] = 0;
			}
		}
		want = want < 0 ? 0 : want > top_row_of[d] ? top_row_of[d] : want;
		if (want != cap[d]) {
			if (want < cap[d]) {
				gov_throttles++;
			}
			say("cpufreq: %s: %s at %d.%d C (limit %d): its ceiling row %d -> %d, %u MHz\n", osm_name[d], name, hot / 10,
			    hot % 10, gov_limit_c, cap[d], want, row_khz[d][want] / 1000);
			cap[d] = want;
			row = MIN(load_row[d], cap[d]);
			if (row != written_row[d]) {
				*(volatile uint32_t *)(osm_base[d] + OSM_PERF_STATE) = (uint32_t)row;
				written_row[d] = row;
			}
		}
	}
	l3_follow();
	if (gov_load_on == 2 && !idle_floor) {
		look_ms = GOV_FAST_MS;
	}
	clock_interval_to_deadline(look_ms, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(gov_call, deadline);
}

static void
governor_start(void)
{
	uint32_t limit = 95;
	uint64_t deadline;

	if (PE_parse_boot_argn("thermal_slow", &limit, sizeof(limit)) && (limit == 0 || limit < 40 || limit > 110)) {
		say("cpufreq: no governor (thermal_slow=%u): hot cores are not slowed, only the stop at 105\n", limit);
		return;
	}
	gov_limit_c = (int)limit;
	gov_limit_tenths = (int)limit * 10;
	gov_tm[0] = ml_io_map(0xc263000, 0x1000);
	gov_tm[1] = ml_io_map(0xc265000, 0x1000);
	if (gov_tm[0] == 0 || gov_tm[1] == 0) {
		say("cpufreq: no governor: could not map the sensors\n");
		return;
	}
	PE_parse_boot_argn("cpu_gov", &gov_load_on, sizeof(gov_load_on));
	for (uint32_t d = 0; d < OSM_DOMAINS; d++) {
		load_row[d] = written_row[d] = top_row_of[d] < 0 ? 0 : top_row_of[d];
	}
	say("cpufreq: governor: a group is slowed at %u C, and sped up again under %u C; %s\n", limit, limit - 5,
	    gov_load_on == 2 ? "and by its load: the slowest row at which the busiest core is 50 % busy, every 20 ms; touch and frame boosts" :
	    gov_load_on ? "and by its load: the top row at 60 % busy, a row down a tenth of a second under 25 %" : "load not looked at (cpu_gov=0)");
	l3_start();
	gov_call = thread_call_allocate(gov_look, NULL);
	if (gov_load_on == 2) {
		hakios_frame_hook = gov_frame;
		hakios_touch_hook = gov_touch;
	}
	clock_interval_to_deadline(GOV_MS, NSEC_PER_MSEC, &deadline);
	thread_call_enter_delayed(gov_call, deadline);
}
