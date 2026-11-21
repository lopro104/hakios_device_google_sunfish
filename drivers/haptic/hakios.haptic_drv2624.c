/*
 * The vibration motor of the Pixel 4a (sunfish): a linear resonant actuator driven by a Texas Instruments DRV2624
 * (compatible "ti,drv2624"), Google's drivers/input/misc/drv2624.c. The numbers below are this phone's, from Android's
 * live device tree (/proc/device-tree, read only) and that driver's source:
 *
 *   I2C        the serial engine 9 (qupv3_se9_i2c at 0xa8c000, function qup13 on GPIO 46 data and 47 clock, 2 mA, no pull),
 *              address 0x5a; done by hand here as in hakios.haptic.c (POCO X5 Pro), the two pins taken for each transaction
 *              and given back as they were found. They are in the TLMM's east tile (0x3400000 + 0x100000).
 *   reset      GPIO 11, high = running (the pin's default is output low; Android's driver: low 1 ms, then high)
 *   the motor  an LRA: rated voltage 0x6d, overdrive clamp 0xa1, 172 Hz (drive time), open-loop period for 155 Hz, sine
 *              shape, "smart loop" (the open-loop bit set), BEMF factor 0x96, voltage compensation 0x12, blanking 2, idiss 2
 *
 * Nothing here is done unless a boot-arg asks for it (the same ladder as hakios.haptic.c):
 *
 *   haptic=1   a look, reading only: the pins
 *   haptic=2   ... and the reset released, the chip's ID and registers read. Nothing is written to the chip.
 *              (haptic_i2c=0: the reset released and nothing else, no I2C)
 *   haptic=3   ... and Android's configuration written (every write read back). Nothing moves.
 *   haptic=4   ... and one pulse played (haptic_ms=N, default 40: the motor runs N ms in real-time-playback mode)
 *   haptic=5   ... configured as for 3, and the motor for Darwin until the run ends: what Darwin plays on its "Actuator"
 *              device (the HAL plug-in's, userland/src/audio_plugin.c) is written to /dev/hakios-haptic as samples of one
 *              channel; a waveform of Darwin's becomes a tap (a short pulse), one that goes on a vibration (until it ends, or
 *              HAPTIC_MAX_MS), and the strength follows how loud Darwin's waveform is.
 *
 * Limits, whatever asks: no pulse longer than HAPTIC_MAX_MS, the RTP amplitude never above 0x7f, and any fault the chip
 * reports (over-current, over-temperature, under-voltage), a write that does not read back, or a byte nobody acknowledges:
 * the chip is put into reset and nothing more is done for the rest of the run. At the run's end the chip goes back into reset.
 * Status: WRITTEN FROM THE SOURCES, NOT YET RUN ON THE PHONE.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <miscfs/devfs/devfs.h>
#include <sys/conf.h>
#include <sys/systm.h>
#include <sys/uio.h>
#include <sys/hakios.audio.h>
#include <sys/hakios.input.h>
#include <kern/thread_call.h>
#include <kern/startup.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);

static vm_offset_t tlmm;
static uint32_t level;
static bool broken, released, leaving;

/* every line to the loader's log as well (and to the console when that may be written) */
static int say(const char *format, ...) __printflike(1, 2);

static int
say(const char *format, ...)
{
	char line[300];
	va_list list;
	int length;

	va_start(list, format);
	length = vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (!leaving && ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
	return length;
}

static inline uint32_t
rd(vm_offset_t address)
{
	return *(volatile uint32_t *)address;
}

static inline void
wr(vm_offset_t address, uint32_t value)
{
	*(volatile uint32_t *)address = value;
}

static void
spin_us(uint32_t us)
{
	uint64_t until;

	nanoseconds_to_absolutetime((uint64_t)us * NSEC_PER_USEC, &until);
	until += mach_absolute_time();
	while (mach_absolute_time() < until) {
	}
}

static void
wait_ms(uint32_t ms)
{
	if (leaving) {
		spin_us(ms * 1000);
	} else {
		delay_for_interval(ms, NSEC_PER_MSEC);
	}
}

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_PHYS               0x03900000UL    /* SM7150 TLMM North tile (sm7150.dtsi): holds GPIO 11, 46, 47 */
#define TLMM_SIZE               0x300000UL
#define GPIO_CFG(n)             (tlmm + 0x1000UL * (n))         /* [1:0] pull, [5:2] function, [8:6] drive, [9] output */
#define GPIO_IO(n)              (tlmm + 0x1000UL * (n) + 4)     /* bit 0 in, bit 1 out */
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

#define PIN_SDA                 46      /* the serial engine's first pin is the data line, the second the clock */
#define PIN_SCL                 47
#define PIN_RESET               11

static void
show_pins(void)
{
	static const struct {
		unsigned pin;
		const char *what;
	} pins[] = { { PIN_SDA, "I2C data" }, { PIN_SCL, "I2C clock" }, { PIN_RESET, "reset" } };

	for (unsigned i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
		uint32_t cfg = rd(GPIO_CFG(pins[i].pin)), io = rd(GPIO_IO(pins[i].pin));

		say("haptic: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pins[i].pin, pins[i].what, (cfg >> 2) & 15,
		    cfg & 3, (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
	}
}

/* the reset pin as an output of the GPIO function, driving `high' (2 mA, no pull, as Android has it) */
static void
reset_pin(bool high)
{
	wr(GPIO_IO(PIN_RESET), high ? 2 : 0);
	wr(GPIO_CFG(PIN_RESET), CFG_OUTPUT);
	released = high;
}

/* ---- I2C by hand (as in hakios.speaker.c) ---------------------------------------------------------- */

static void
half_bit(void)
{
	spin_us(5);                     /* 100 kHz at most */
}

static void
line(unsigned pin, bool high)
{
	wr(GPIO_IO(pin), 0);
	wr(GPIO_CFG(pin), CFG_PULL_UP | (high ? 0 : CFG_OUTPUT));
}

static bool
level_of(unsigned pin)
{
	return rd(GPIO_IO(pin)) & 1;
}

static void
i2c_start(void)
{
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, false);
	half_bit();
}

static void
i2c_stop(void)
{
	line(PIN_SDA, false);
	half_bit();
	line(PIN_SCL, true);
	half_bit();
	line(PIN_SDA, true);
	half_bit();
}

static bool
i2c_bit(bool out)
{
	bool in;

	line(PIN_SDA, out);
	half_bit();
	line(PIN_SCL, true);
	for (unsigned stretched = 0; !level_of(PIN_SCL) && stretched < 100; stretched++) {
		half_bit();
	}
	half_bit();
	in = level_of(PIN_SDA);
	line(PIN_SCL, false);
	return in;
}

static bool
i2c_write(uint8_t byte)
{
	for (int bit = 7; bit >= 0; bit--) {
		i2c_bit((byte >> bit) & 1);
	}
	return !i2c_bit(true);
}

static uint8_t
i2c_read(bool more)
{
	uint8_t byte = 0;

	for (int bit = 7; bit >= 0; bit--) {
		byte = (uint8_t)(byte << 1 | i2c_bit(true));
	}
	i2c_bit(!more);
	return byte;
}

/* The bus is shared with the speaker amplifiers (hakios.cs35l41_sunfish.c, through hakios_sunfish_i2c()): one transfer at a time. */
static LCK_GRP_DECLARE(bus_lock_group, "hakios-sunfish-i2c");
static LCK_MTX_DECLARE(bus_lock, &bus_lock_group);

/* the bus is shared with the camera's PMIC (SLG51000, 0x75; drivers/camera/hakios.camera_sunfish.c): it takes this too */
void hakios_se9_bus_lock(void);
void hakios_se9_bus_unlock(void);
void
hakios_se9_bus_lock(void)
{
	lck_mtx_lock(&bus_lock);
}
void
hakios_se9_bus_unlock(void)
{
	lck_mtx_unlock(&bus_lock);
}

static uint32_t taken_cfg[2], taken_io[2];

static bool
bus_take(void)
{
	taken_cfg[0] = rd(GPIO_CFG(PIN_SDA));
	taken_cfg[1] = rd(GPIO_CFG(PIN_SCL));
	taken_io[0] = rd(GPIO_IO(PIN_SDA));
	taken_io[1] = rd(GPIO_IO(PIN_SCL));
	line(PIN_SDA, true);
	line(PIN_SCL, true);
	half_bit();
	if (!level_of(PIN_SDA) || !level_of(PIN_SCL)) {
		wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
		wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
		wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
		wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
		say("haptic: the I2C bus is not at rest (gpio%u reads %u, gpio%u reads %u): not touched\n", PIN_SDA,
		    level_of(PIN_SDA), PIN_SCL, level_of(PIN_SCL));
		return false;
	}
	return true;
}

static void
bus_give_back(void)
{
	wr(GPIO_IO(PIN_SDA), taken_io[0] & 2);
	wr(GPIO_IO(PIN_SCL), taken_io[1] & 2);
	wr(GPIO_CFG(PIN_SDA), taken_cfg[0]);
	wr(GPIO_CFG(PIN_SCL), taken_cfg[1]);
}

#define DRV_ADDRESS             0x5a

/* `count' 8-bit registers from `reg' on (the chip's address counts up, but for its RAM data port) */
static bool
regs_read(uint8_t reg, uint8_t *values, unsigned count)
{
	bool answered;

	lck_mtx_lock(&bus_lock);
	if (!bus_take()) {
		lck_mtx_unlock(&bus_lock);
		return false;
	}
	i2c_start();
	answered = i2c_write(DRV_ADDRESS << 1) && i2c_write(reg);
	if (answered) {
		i2c_start();
		answered = i2c_write(DRV_ADDRESS << 1 | 1);
	}
	for (unsigned i = 0; answered && i < count; i++) {
		values[i] = i2c_read(i + 1 < count);
	}
	i2c_stop();
	bus_give_back();
	lck_mtx_unlock(&bus_lock);
	return answered;
}

static bool
regs_write(uint8_t reg, const uint8_t *values, unsigned count)
{
	bool answered;

	lck_mtx_lock(&bus_lock);
	if (!bus_take()) {
		lck_mtx_unlock(&bus_lock);
		return false;
	}
	i2c_start();
	answered = i2c_write(DRV_ADDRESS << 1) && i2c_write(reg);
	for (unsigned i = 0; answered && i < count; i++) {
		answered = i2c_write(values[i]);
	}
	i2c_stop();
	bus_give_back();
	lck_mtx_unlock(&bus_lock);
	return answered;
}

/*
 * For the other chips on these two lines (the speaker amplifiers at 0x40 and 0x41): `nt' bytes written to `address', then,
 * if nr != 0, a repeated start and `nr' bytes read. False when the bus is not at rest or a byte is not acknowledged.
 */
bool hakios_sunfish_i2c(uint8_t address, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr);
bool
hakios_sunfish_i2c(uint8_t address, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr)
{
	bool answered;

	lck_mtx_lock(&bus_lock);
	if (tlmm == 0) {
		tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	}
	if (tlmm == 0 || !bus_take()) {
		lck_mtx_unlock(&bus_lock);
		return false;
	}
	i2c_start();
	answered = i2c_write((uint8_t)(address << 1));
	for (unsigned i = 0; answered && i < nt; i++) {
		answered = i2c_write(tx[i]);
	}
	if (answered && nr != 0) {
		i2c_start();
		answered = i2c_write((uint8_t)(address << 1 | 1));
		for (unsigned i = 0; answered && i < nr; i++) {
			rx[i] = i2c_read(i + 1 < nr);
		}
	}
	i2c_stop();
	bus_give_back();
	lck_mtx_unlock(&bus_lock);
	return answered;
}

/* ---- the chip -------------------------------------------------------------------------------------- */

#define R_ID            0x00
#define R_STATUS        0x01            /* [0] over-current, [1] over-temperature, [2] under-voltage, [3] done, [4] program error */
#define R_INT_ENABLE    0x02
#define R_MODE          0x07            /* [3:2] pin function (2: interrupt), [1:0] the mode (0: real-time playback) */
#define R_CONTROL1      0x08            /* [7] 1 = LRA, [6] 1 = open loop, [4] automatic braking */
#define R_GO            0x0c
#define R_CONTROL2      0x0d            /* [7] library (0: LRA), [5] interval (1: 1 ms) */
#define R_RTP_INPUT     0x0e            /* the amplitude of real-time playback */
#define R_RATED_VOLTAGE 0x1f
#define R_OD_CLAMP      0x20
#define R_CAL_COMP      0x21
#define R_CAL_BEMF      0x22
#define R_LOOP_CONTROL  0x23
#define R_DRIVE_TIME    0x27
#define R_BLK_IDISS     0x28
#define R_ZC_OD_TIME    0x29
#define R_LRA_OL_CTRL   0x2c
#define R_OL_PERIOD_H   0x2e
#define R_OL_PERIOD_L   0x2f

#define STATUS_FAULTS   0x07
#define HAPTIC_MAX_MS   1000
#define RTP_MAX         0x7f

static uint32_t haptic_gain = 45;       /* percentage (1..100, default 45% for subtle crisp tactile clicks) */
static uint32_t haptic_tap_ms = 16;     /* tap pulse duration in ms (default 16 ms) */

static void
give_up(const char *why)
{
	broken = true;
	say("haptic: %s: THE CHIP IS PUT INTO RESET (gpio%u low) and nothing more is done\n", why, PIN_RESET);
	if (tlmm != 0) {
		reset_pin(false);
	}
}

static bool
get(uint8_t reg, uint8_t *value)
{
	if (!regs_read(reg, value, 1)) {
		say("haptic: %#04x NOT READ\n", reg);
		return false;
	}
	return true;
}

/* One register written and read back (`mask': the bits that are to read back as written). */
static bool
put_masked(uint8_t reg, uint8_t value, uint8_t mask)
{
	uint8_t after = 0;

	if (broken) {
		return false;
	}
	if (!regs_write(reg, &value, 1)) {
		say("haptic: %#04x <- %#04x NOT ACKNOWLEDGED\n", reg, value);
		give_up("a write was not acknowledged");
		return false;
	}
	if (!regs_read(reg, &after, 1)) {
		give_up("a register could not be read back");
		return false;
	}
	say("haptic: %#04x <- %#04x, reads %#04x%s\n", reg, value, after, (after & mask) == (value & mask) ? "" : ": NOT AS WRITTEN");
	if ((after & mask) != (value & mask)) {
		give_up("a register did not read back as written");
		return false;
	}
	return true;
}

static bool
put(uint8_t reg, uint8_t value)
{
	return put_masked(reg, value, 0xff);
}

/* read-modify-write of the bits in `mask' */
static bool
put_bits(uint8_t reg, uint8_t mask, uint8_t value)
{
	uint8_t now = 0;

	if (!get(reg, &now)) {
		give_up("a register could not be read");
		return false;
	}
	return put_masked(reg, (uint8_t)((now & ~mask) | (value & mask)), mask);
}

/* The status flags (reading clears them): false, and the chip given up on, if one is a fault. */
static bool
healthy(const char *when)
{
	uint8_t flags = 0;

	if (!get(R_STATUS, &flags)) {
		give_up("the status could not be read");
		return false;
	}
	if (flags & STATUS_FAULTS) {
		say("haptic: %s: the chip reports %#04x:%s%s%s\n", when, flags, flags & 0x01 ? " OVER-CURRENT" : "",
		    flags & 0x02 ? " OVER-TEMPERATURE" : "", flags & 0x04 ? " UNDER-VOLTAGE" : "");
		give_up("a fault");
		return false;
	}
	return true;
}

static void
show_registers(void)
{
	for (unsigned reg = 0; reg <= 0x30; reg++) {
		uint8_t value = 0;

		if (!regs_read((uint8_t)reg, &value, 1)) {
			say("haptic: %#04x NOT READ\n", reg);
			return;
		}
		say("haptic: %#04x = %#04x\n", reg, value);
	}
}

static bool
chip_answers(void)
{
	uint8_t id = 0xff;

	if (!regs_read(R_ID, &id, 1)) {
		return false;
	}
	say("haptic: the chip answers at %#04x: ID register %#04x\n", DRV_ADDRESS, id);
	return true;
}

/*
 * Google's dev_init_platform_data() with this phone's numbers: LRA, open loop ("smart loop"), automatic braking, rated voltage
 * 0x6d, overdrive clamp 0xa1, drive time for 172 Hz (5 * (1000 - 172) / 172 = 24), open-loop period for 155 Hz
 * (1e9 / (24615 * 155) = 262 = 0x106), sine shape, voltage compensation 0x12, BEMF factor 0x96, BEMF gain 0, brake factor 0,
 * blanking 2, idiss 2, zero-crossing time 0, a 1 ms interval.
 */
static bool
configure(void)
{
	return put_bits(R_MODE, 0x0f, 0x08) &&                  /* real-time playback, the pin as the interrupt */
	       put_bits(R_CONTROL1, 0xd0, 0xd0) &&              /* LRA, open loop, automatic braking */
	       put_bits(R_CONTROL2, 0xa0, 0x20) &&              /* the LRA library, a 1 ms interval */
	       put(R_RATED_VOLTAGE, 0x6d) && put(R_OD_CLAMP, 0xa1) &&
	       put_bits(R_DRIVE_TIME, 0x9f, 24) &&
	       put_bits(R_OL_PERIOD_H, 0x03, 0x01) && put(R_OL_PERIOD_L, 0x06) &&
	       put_bits(R_LRA_OL_CTRL, 0x01, 0x01) &&
	       put(R_CAL_COMP, 0x12) && put(R_CAL_BEMF, 0x96) &&
	       put_bits(R_LOOP_CONTROL, 0x73, 0x00) &&
	       put_bits(R_BLK_IDISS, 0xff, 0x22) &&
	       put_bits(R_ZC_OD_TIME, 0x03, 0x00) &&
	       put(R_INT_ENABLE, 0x00);
}

/* ---- playing: real-time playback ------------------------------------------------------------------- */

static bool
motor_on(uint8_t amplitude)
{
	uint8_t go = 0x01;

	if (amplitude > RTP_MAX) {
		amplitude = RTP_MAX;
	}
	if (!regs_write(R_RTP_INPUT, &amplitude, 1) || !regs_write(R_GO, &go, 1)) {
		give_up("the start was not acknowledged");
		return false;
	}
	return true;
}

static bool
motor_off(void)
{
	uint8_t zero = 0;

	/* the amplitude to zero first (a stop with automatic braking can ring on), then GO */
	if (!regs_write(R_RTP_INPUT, &zero, 1) || !regs_write(R_GO, &zero, 1)) {
		give_up("it does not stop");
		return false;
	}
	return true;
}

static bool
play_pulse(unsigned ms)
{
	uint8_t amp = (uint8_t)((RTP_MAX * haptic_gain) / 100);
	if (amp == 0) {
		amp = 1;
	}
	if (ms < 5 || ms > HAPTIC_MAX_MS) {
		ms = 40;
	}
	say("haptic: a pulse of %u ms at amplitude %#04x\n", ms, amp);
	if (!motor_on(amp)) {
		return false;
	}
	wait_ms(ms);
	if (!motor_off()) {
		return false;
	}
	wait_ms(20);
	return healthy("after the pulse");
}

/* ---- haptic=5: the motor for Darwin ------------------------------------------------------------------- */

#define ACTIVE_US               30000           /* a write that was not silence counts for this long */
#define SILENT_BELOW            256             /* of 32768: samples no larger are silence */
#define TAP_US                  60000           /* longer than this, and it is a vibration, not a tap */
#define SETTLE_US               8000            /* how long Darwin's waveform is listened to before a tap: its loudness */
#define FOLLOW_US               20000           /* how often a vibration's strength follows Darwin's */

#define TOLD_EVENTS             24              /* the first so many taps and vibrations are each said */

static volatile uint64_t heard_us;              /* when the last write that was not silence came */
static volatile uint64_t quiet_us;              /* when the last write that was silence came */
static volatile uint64_t onset_us;              /* when Darwin's last waveform began: the first write not silence after a pause */
static volatile uint32_t heard_peak;            /* the largest sample of that waveform, or since the service last looked */
static uint64_t written_frames;
static unsigned taps, loops, capped;            /* what has been played, for the run's end */

static uint64_t
now_us(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_USEC;
}

/* From the audio server's I/O thread: never waits. */
static int
haptic_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	int16_t samples[256];
	int error = 0;

	while (uio_resid(uio) >= 2) {
		user_ssize_t bytes = uio_resid(uio);
		uint32_t peak = 0;

		if (bytes > (user_ssize_t)sizeof(samples)) {
			bytes = sizeof(samples);
		}
		bytes &= ~(user_ssize_t)1;
		if ((error = uiomove((caddr_t)samples, (int)bytes, uio)) != 0) {
			break;
		}
		for (unsigned i = 0; i < bytes / 2; i++) {
			uint32_t v = samples[i] < 0 ? (uint32_t)-(int32_t)samples[i] : (uint32_t)samples[i];

			peak = v > peak ? v : peak;
		}
		written_frames += (uint64_t)bytes / 2;
		if (peak > SILENT_BELOW) {
			uint64_t now = now_us();

			if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
				onset_us = now;
				heard_peak = 0;
			}
			heard_us = now;
			if (peak > heard_peak) {
				heard_peak = peak;
			}
		} else {
			quiet_us = now_us();
		}
	}
	return error;
}

/*
 * A test's stand-in for Darwin's waveform (hakios.mic.c, mic_tap=1: the microphone hears the motor): as if
 * a write of this loudness (of 32767) had just come, or silence for 0. The service plays it with all its
 * own limits, as it plays Darwin's.
 */
void
hakios_haptic_pretend(uint32_t peak)
{
	uint64_t now = now_us();

	if (peak <= SILENT_BELOW) {
		quiet_us = now;
		return;
	}
	if (heard_us == 0 || quiet_us > heard_us || now - heard_us >= ACTIVE_US) {
		onset_us = now;
		heard_peak = 0;
	}
	heard_us = now;
	if (peak > heard_peak) {
		heard_peak = peak;
	}
}

static int
haptic_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static const struct cdevsw haptic_cdevsw = {
	.d_open = haptic_open, .d_close = haptic_open, .d_read = eno_rdwrt, .d_write = haptic_write,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/* Start a waveform without waiting for it: once, or (`forever') until stopped. */
/* How strong to play what Darwin plays: scaled by haptic_gain */
static uint8_t
amplitude_for(uint32_t peak)
{
	uint32_t top = (RTP_MAX * haptic_gain) / 100;
	if (top == 0) {
		top = 1;
	}
	uint32_t low = top * 3 / 8;
	if (low == 0) {
		low = 1;
	}
	uint32_t third = 32767 / 3;

	return (uint8_t)(peak >= third ? top : low + (top - low) * peak / third);
}

#define TAP_PLAY_US     ((uint64_t)haptic_tap_ms * 1000)        /* a tap: the motor runs this long */

static void
serve(void)
{
	enum { IDLE, TAP, LOOP, REST } state = IDLE;
	uint64_t since = 0, rest_until = 0, last_look = 0, tap_onset = 0, followed = 0;
	uint8_t playing = 0;
	int major = cdevsw_add(-1, &haptic_cdevsw);

	if (major < 0 || devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-haptic") == NULL) {
		say("haptic: /dev/hakios-haptic cannot be made\n");
		return;
	}
	say("haptic: THE MOTOR PLAYS WHAT DARWIN PLAYS ON ITS ACTUATOR (/dev/hakios-haptic) from here on, until the run ends\n");
	while (!broken) {
		uint64_t now = now_us();
		bool active = heard_us != 0 && now - heard_us < ACTIVE_US;
		uint32_t peak = heard_peak;
		uint8_t amp = amplitude_for(peak);

		switch (state) {
		case IDLE:
			if (active && onset_us != tap_onset && now - onset_us >= SETTLE_US) {
				if (!motor_on(amp)) {
					break;
				}
				state = TAP;
				since = now;
				tap_onset = onset_us;
				playing = amp;
				taps++;
				if (taps + loops <= TOLD_EVENTS) {
					say("haptic: tap %u: Darwin's waveform peaks at %u of 32768: amplitude %#04x\n", taps, peak, amp);
				}
			}
			break;
		case TAP:
			if (active && quiet_us < tap_onset && now - since > TAP_US) {
				/* Darwin goes on playing: a vibration */
				state = LOOP;
				since = followed = now;
				heard_peak = 0;
				loops++;
				if (taps + loops <= TOLD_EVENTS) {
					say("haptic: vibration %u: Darwin goes on playing (peak %u): the motor goes on, %u ms at most\n", loops,
					    peak, HAPTIC_MAX_MS);
				}
			} else if (now - since >= TAP_PLAY_US && !active) {
				if (motor_off()) {
					state = IDLE;
				}
			} else if (now - since >= TAP_PLAY_US && !(active && quiet_us < tap_onset)) {
				if (motor_off()) {
					state = IDLE;
				}
			}
			break;
		case LOOP:
			if (active && now - followed >= FOLLOW_US) {
				followed = now;
				heard_peak = 0;
				if (peak != 0 && (amp > playing + 8 || amp + 8 < playing)) {
					if (!motor_on(amp)) {
						break;
					}
					playing = amp;
				}
			}
			if (!active || now - since > HAPTIC_MAX_MS * 1000ull) {
				bool cut = now - since > HAPTIC_MAX_MS * 1000ull;

				if (cut) {
					capped++;
					rest_until = now + HAPTIC_MAX_MS * 1000ull;
				}
				if (motor_off()) {
					state = rest_until > now ? REST : IDLE;
					if (taps + loops <= TOLD_EVENTS + 1) {
						say("haptic: vibration %u: %s after %llu ms\n", loops,
						    cut ? "CUT at the limit" : "Darwin stopped, and so does the motor", (now - since) / 1000);
					}
				}
			}
			break;
		case REST:
			if (now >= rest_until) {
				state = IDLE;
			}
			break;
		}
		if (state != IDLE && state != REST && now - last_look > 50000) {
			last_look = now;
			if (!healthy("while playing")) {
				break;
			}
		}
		wait_ms(4);
	}
	say("haptic: the motor is given up on (%u taps and %u vibrations played, %u cut at %u ms)\n", taps, loops, capped,
	    HAPTIC_MAX_MS);
}

static void
haptic_thread(__unused void *parameter, __unused wait_result_t result)
{
	bool configured = false;

	tlmm = ml_io_map(TLMM_PHYS, TLMM_SIZE);
	say("haptic: level %u: %s\n", level, level == 1 ? "a look, reading only" : level == 2 ? "the reset released, registers read" :
	    level == 3 ? "the chip configured, nothing played" : level == 4 ? "one pulse played" : "the motor for Darwin");
	show_pins();
	if (level >= 2 && !((rd(GPIO_CFG(PIN_RESET)) & CFG_OUTPUT) && (rd(GPIO_IO(PIN_RESET)) & 2))) {
		/* (Android's driver: the pin low 1 ms, then high, and the chip ready for I2C 1 ms later) */
		say("haptic: the reset is released: gpio%u low for 2 ms, then high\n", PIN_RESET);
		reset_pin(false);
		wait_ms(2);
		reset_pin(true);
		wait_ms(5);
	}
	{
		uint32_t i2c = 1;

		(void)PE_parse_boot_argn("haptic_i2c", &i2c, sizeof(i2c));
		if (level >= 2 && i2c == 0) {
			/* haptic_i2c=0: the reset pin only, no I2C (to tell which of the two a phone does not survive) */
			show_pins();
			say("haptic: haptic_i2c=0: no I2C transaction made; the chip stays out of reset until the run ends\n");
			say("haptic: done\n");
			thread_terminate(current_thread());
			return;
		}
	}
	if (level >= 2 && !chip_answers()) {
		say("haptic: (the reset pin %s)\n", (rd(GPIO_CFG(PIN_RESET)) & CFG_OUTPUT) && (rd(GPIO_IO(PIN_RESET)) & 2) ?
		    "drives high: the chip should answer" : "does not drive high: the chip is held in reset or not powered");
		if (released) {
			give_up("the chip does not answer");
		}
	} else if (level >= 2) {
		show_registers();
		configured = level >= 3 && configure();
		if (configured && level == 4) {
			uint32_t ms = 40;

			PE_parse_boot_argn("haptic_ms", &ms, sizeof(ms));
			(void)play_pulse(ms);
		} else if (level == 5 && !broken && configured) {
			serve();
		} else if (level == 5 && !broken) {
			give_up("the chip is not configured");
		}
	}
	say("haptic: done\n");
	thread_terminate(current_thread());
}

static bool started;

void
hakios_haptic_init(void)
{
	thread_t thread;

	if (started || !PE_parse_boot_argn("haptic", &level, sizeof(level)) || level == 0) {
		return;
	}
	started = true;
	if (level > 5) {
		level = 5;
	}
	(void)PE_parse_boot_argn("haptic_gain", &haptic_gain, sizeof(haptic_gain));
	if (haptic_gain == 0) {
		haptic_gain = 1;
	} else if (haptic_gain > 100) {
		haptic_gain = 100;
	}
	(void)PE_parse_boot_argn("haptic_tap_ms", &haptic_tap_ms, sizeof(haptic_tap_ms));
	if (haptic_tap_ms < 5) {
		haptic_tap_ms = 5;
	} else if (haptic_tap_ms > 50) {
		haptic_tap_ms = 50;
	}
	if (kernel_thread_start(haptic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/* No audio driver on this phone to start the motor: a thread call, 3 s into the boot (as the modem's driver does). */
static thread_call_t haptic_call;

static void
haptic_go(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	hakios_haptic_init();
}

static void
haptic_startup(void)
{
	uint64_t deadline;

	haptic_call = thread_call_allocate(haptic_go, NULL);
	clock_interval_to_deadline(3, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(haptic_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, haptic_startup);

/* At the run's end and on a panic's way out: the chip back into reset, if it was let out. */
void
hakios_haptic_quiesce(void)
{
	if (released && tlmm != 0) {
		leaving = true;
		reset_pin(false);
		if (level == 5) {
			say("haptic: at the run's end: %u taps and %u vibrations played (%u cut at %u ms), from %llu frames Darwin "
			    "wrote; the chip is held in reset\n", taps, loops, capped, HAPTIC_MAX_MS, written_frames);
		}
	}
}
