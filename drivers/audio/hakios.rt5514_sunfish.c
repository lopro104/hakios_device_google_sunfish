/*
 * The Pixel 4a's microphones: its digital mics go into a Realtek RT5514 (also Google's hotword chip), which hands
 * them to the audio DSP over the tertiary TDM port (AFE 0x9021, group 0x9121). From Android's device tree and its
 * kernel (msm-4.14 sound/soc/codecs/rt5514.c, techpack/audio/asoc/sm6150.c, mixer_paths_bolero_snd.xml):
 *
 *   the chip     I2C 0x57 on QUP0 SE2 (i2c@888000), GPIO 34 data / 35 clock (function qup02, the TLMM's south tile);
 *                reset GPIO 33 (north tile), active high: Android holds it high (GPIOD_OUT_HIGH)
 *   registers    32-bit addresses, 32-bit values, both big-endian; the codec's map is 0x18000000 + reg
 *                (VENDOR_ID2 0x2ff4 = 0x10ec5514; VENDOR_ID1 0x2ff0 = 0x80 on the RT5514P)
 *   TDM          GPIO 53..56 (west tile, function ter_mi2s): clock, frame, data in, data out; 8 slots of 32 bits
 *                at 48 kHz (12.288 MHz, the DSP the clock master); Android's handset-mic: one channel, DMIC1,
 *                "Sto1 ADC MIXL DMIC Switch", ADC1 capture volume 23
 *
 * The I2C lines are driven by hand (as the haptic driver does for its own), the pins given back as found.
 *
 *   mic=1      a look: the pins as found, then the chip's ID read over I2C (the reset line released first if it
 *              was held; nothing else written)
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

/* ---- pins ------------------------------------------------------------------------------------------ */

#define TLMM_NORTH              0x03900000UL    /* pinctrl-sdmmagpie.c: base 0x03400000 + the tile */
#define TLMM_SOUTH              0x03d00000UL
#define TLMM_WEST               0x03500000UL
#define CFG_PULL_UP             3
#define CFG_OUTPUT              (1u << 9)

#define PIN_RESET               33
#define PIN_SDA                 34
#define PIN_SCL                 35
#define CODEC_ADDRESS           0x57

static vm_offset_t pin_page[64];        /* each pin's own page: cfg at +0, in/out at +4 */

static vm_offset_t
tile_of(unsigned pin)
{
	return pin == PIN_RESET ? TLMM_NORTH : (pin == PIN_SDA || pin == PIN_SCL) ? TLMM_SOUTH : TLMM_WEST;
}

static bool
map_pin(unsigned pin)
{
	if (pin_page[pin] == 0) {
		pin_page[pin] = ml_io_map(tile_of(pin) + 0x1000UL * pin, 0x1000);
	}
	return pin_page[pin] != 0;
}

#define GPIO_CFG(n)             (pin_page[n])
#define GPIO_IO(n)              (pin_page[n] + 4)

static void
show_pin(unsigned pin, const char *what)
{
	uint32_t cfg = rd(GPIO_CFG(pin)), io = rd(GPIO_IO(pin));

	say("mic: gpio%u (%s): function %u, pull %u, %u mA, %s, reads %u\n", pin, what, (cfg >> 2) & 15, cfg & 3,
	    (((cfg >> 6) & 7) + 1) * 2, (cfg & CFG_OUTPUT) ? (io & 2 ? "drives high" : "drives low") : "input", io & 1);
}

/* ---- I2C by hand ------------------------------------------------------------------------------------ */

static void
half_bit(void)
{
	spin_us(5);
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
		say("mic: the I2C bus is not at rest (data reads %u, clock %u)\n", level_of(PIN_SDA), level_of(PIN_SCL));
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

/* one 32-bit register at a 32-bit address, both big-endian (regmap's default for the chip) */
static bool
codec_read(uint32_t reg, uint32_t *value)
{
	uint8_t v[4] = { 0 };
	bool answered;

	if (!bus_take()) {
		bus_give_back();
		return false;
	}
	i2c_start();
	answered = i2c_write(CODEC_ADDRESS << 1) && i2c_write((uint8_t)(reg >> 24)) && i2c_write((uint8_t)(reg >> 16)) &&
	    i2c_write((uint8_t)(reg >> 8)) && i2c_write((uint8_t)reg);
	if (answered) {
		i2c_start();
		answered = i2c_write(CODEC_ADDRESS << 1 | 1);
	}
	for (unsigned i = 0; answered && i < 4; i++) {
		v[i] = i2c_read(i < 3);
	}
	i2c_stop();
	bus_give_back();
	*value = (uint32_t)v[0] << 24 | (uint32_t)v[1] << 16 | (uint32_t)v[2] << 8 | v[3];
	return answered;
}

static bool
codec_write(uint32_t reg, uint32_t value)
{
	bool answered;

	if (!bus_take()) {
		bus_give_back();
		return false;
	}
	i2c_start();
	answered = i2c_write(CODEC_ADDRESS << 1) && i2c_write((uint8_t)(reg >> 24)) && i2c_write((uint8_t)(reg >> 16)) &&
	    i2c_write((uint8_t)(reg >> 8)) && i2c_write((uint8_t)reg) && i2c_write((uint8_t)(value >> 24)) &&
	    i2c_write((uint8_t)(value >> 16)) && i2c_write((uint8_t)(value >> 8)) && i2c_write((uint8_t)value);
	i2c_stop();
	bus_give_back();
	return answered;
}

#define R(reg)                  (0x18000000u | (reg))   /* the codec's map, as rt5514_i2c_read/write reach it */

/* a register of the codec's map: only the bits of `mask' changed */
static bool
codec_bits(uint32_t reg, uint32_t mask, uint32_t value)
{
	uint32_t now = 0;

	return codec_read(R(reg), &now) && codec_write(R(reg), (now & ~mask) | (value & mask));
}

/* ---- the microphones' own supply: PM6150 (slave 0) GPIO 6, Android's "audio_ldo1" (Audio LDO Mic1, for DMIC1) ---- */

extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);

#define SP_CORE_PA              0x0c440000u
#define SP_OBS_PA               0x0e600000u
#define SP_CFG_PA               0x0c40a000u
#define SP_CHNL_PA              0x0c600000u
#define LDO_PPID(n)             (0x0c5u + (n))  /* slave 0, peripherals 0xc5 and 0xc6: GPIO 6 (audio_ldo1) and 7 (audio_ldo2) */

static vm_offset_t sp_core, sp_cfg, sp_obs, ldo_channels[2];
static uint32_t ldo_apids[2] = { ~0u, ~0u };
static bool ldo_owned[2], sp_poisoned;
static unsigned ldo;              /* which of the two the functions below speak to */
#define ldo_apid ldo_apids[ldo]
#define ldo_ours ldo_owned[ldo]
#define ldo_channel ldo_channels[ldo]

static bool
ldo_find(void)
{
	uint32_t count;

	if (sp_core == 0) {
		sp_core = ml_io_map(SP_CORE_PA, 0x2000);
		sp_cfg = ml_io_map(SP_CFG_PA, 0x1000);
		sp_obs = ml_io_map(SP_OBS_PA, 0x10000);
	}
	if (sp_core == 0 || sp_cfg == 0 || sp_obs == 0) {
		return false;
	}
	count = rd(sp_core + 4) & 0x7ffu;
	for (uint32_t i = 0; i < count && i < 512; i++) {
		uint32_t map = rd(sp_core + 0x900u + 4u * i);

		if (map != 0 && ((map >> 8) & 0xfffu) == LDO_PPID(ldo)) {
			bool owned = (rd(sp_cfg + 0x700u + 4u * i) & 7u) == 0;

			if (ldo_apid == ~0u || owned) {
				ldo_apid = i;
				ldo_ours = owned;
			}
		}
	}
	return ldo_apid != ~0u;
}

static bool
sp_transfer(bool write, uint32_t reg, uint8_t *value)
{
	vm_offset_t ch;
	uint32_t status = 0;
	bool completed = false;

	if (sp_poisoned || (write && !ldo_ours) || reg < 0x40u || reg > 0x46u) {
		return false;
	}
	if (write && ldo_channel == 0) {
		ldo_channel = ml_io_map(SP_CHNL_PA + 0x10000u * ldo_apid, 0x1000);
	}
	ch = write ? ldo_channel : sp_obs + 0x80u * ldo_apid;
	if (ch == 0) {
		return false;
	}
	hakios_spmi_lock();
	__asm__ volatile ("dmb oshst" ::: "memory");
	if (write) {
		wr(ch + 0x10u, *value);
		wr(ch, reg << 4);
	} else {
		wr(ch, (1u << 27) | (reg << 4));
	}
	__asm__ volatile ("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000 && !completed; i++) {
		status = rd(ch + 0x08u);
		if (status & 1u) {
			completed = true;
		} else {
			delay(1);
		}
	}
	if (completed && !write && !(status & 0x0eu)) {
		*value = (uint8_t)rd(ch + 0x18u);
	}
	hakios_spmi_unlock();
	if (!completed) {
		sp_poisoned = true;
	}
	return completed && !(status & 0x0eu);
}

/* the mic's supply on (an output driven high: mode 0x01, inverted constant-low source 0x80, enabled) or off */
static bool mic_supply_one(bool on);

/* both mics' supplies (Android switches each with its DMIC's path; both here) */
static bool
mic_supply(bool on)
{
	bool ok = true;

	for (ldo = 0; ldo < 2; ldo++) {
		ok = mic_supply_one(on) && ok;
	}
	ldo = 0;
	return ok;
}

static bool
mic_supply_one(bool on)
{
	uint8_t mode = 0x01, out = on ? 0x80 : 0x00, en = 0x80, now = 0;

	if (ldo_apid == ~0u && !ldo_find()) {
		say("mic: no arbiter channel for the PM6150's GPIO %u\n", 6 + ldo);
		return false;
	}
	if (!(sp_transfer(true, 0x40, &mode) && sp_transfer(true, 0x44, &out) && sp_transfer(true, 0x46, &en))) {
		say("mic: the PM6150's GPIO %u could not be written (channel %u, %s)\n", 6 + ldo, ldo_apid, ldo_ours ? "ours" : "NOT ours");
		return false;
	}
	(void)sp_transfer(false, 0x44, &now);
	say("mic: a microphone's supply (PM6150 GPIO %u) %s (output source reads %#x)\n", 6 + ldo, on ? "ON" : "off", now);
	return true;
}

/* ---- the codec set up as Android's drivers leave it for handset-mic ---- */

/* the RT5514P's I2C patch (const_rt5514p_i2c_patch) and the codec's register patch (rt5514_patch), both from the probe */
static const uint32_t probe_patch[][2] = {
	{ 0xfafafafa, 0x00000001 }, { 0x18002000, 0x000010ec }, { 0x18002004, 0x00808f81 }, { 0x18002008, 0x00770000 },
	{ 0x18002f08, 0x00000006 }, { 0x18002f10, 0x00000000 }, { 0x18002f10, 0x00000001 }, { 0xfafafafa, 0x00000000 },
	{ 0x18001104, 0x00000007 }, { 0x18001108, 0x00000000 }, { 0x1800110c, 0x00000000 }, { 0x18001100, 0x0000031f },
	{ 0x18002000, 0x000010ec },
	{ R(0x2070), 0x00000040 }, { R(0x2104), 0x380200c1 }, { R(0x20ac), 0x44000eee }, { R(0x2200), 0x00028704 },
	{ R(0x2224), 0x00000800 }, { R(0x2180), 0x00000003 }, { R(0x2190), 0x0002042f }, { R(0x2194), 0x0002042f },
	{ R(0x2198), 0x10000342 }, { R(0x21a0), 0x0002042f }, { R(0x21a4), 0x0002042f }, { R(0x21a8), 0x10000342 },
};

static bool codec_running;

/*
 * The writes the machine driver's hw_params (sm6150.c, tertiary TDM TX) and rt5514's hw_params and DAPM make for
 * DMIC1 into "Stereo1": the PLL2 (the RT5514P's) fed by the TDM bit clock, 12.288 MHz in and out (rl6231_pll_calc:
 * k 6, n 6, M bypassed); the system clock from the PLL; DSP-A frames of 8 slots of 32 bits, 16-bit samples; the DMIC
 * clock 12.288 MHz / 4; the ADC filter clock (AD0); the PLL2 and its LDO powered. Stereo1's source DMIC1 (DIG_SOURCE
 * bit 0 clear), its DMIC mix unmuted and its gain 23 are the defaults (DOWNFILTER0 0x0002042f).
 */
bool hakios_rt5514_start(void);
bool
hakios_rt5514_start(void)
{
	uint32_t i2s = 0, clk1 = 0, clk2 = 0, pwr2 = 0, pll = 0, src = 0, id2 = 0, format = 3;
	bool ok = true;

	/*
	 * The frame's format: DSP-A, as Android names it, put every sample one bit late for the DSP's port (a quiet room read as
	 * runs of 0x0000 and 0x7fff: the sign bit lost, logs/193935-run.log); DSP-B (no delay) is the default. mic_fmt=2: DSP-A.
	 */
	PE_parse_boot_argn("mic_fmt", &format, sizeof(format));
	format = format == 2 ? 2 : 3;

	if (!map_pin(PIN_RESET) || !map_pin(PIN_SDA) || !map_pin(PIN_SCL)) {
		return false;
	}
	if (!((rd(GPIO_CFG(PIN_RESET)) & CFG_OUTPUT) && (rd(GPIO_IO(PIN_RESET)) & 2))) {
		wr(GPIO_IO(PIN_RESET), 2);
		wr(GPIO_CFG(PIN_RESET), CFG_OUTPUT);
		delay_for_interval(10, NSEC_PER_MSEC);
	}
	if (!codec_read(R(0x2ff4), &id2) || id2 != 0x10ec5514) {
		(void)codec_read(R(0x2ff4), &id2);
	}
	if (id2 != 0x10ec5514) {
		say("mic: the codec does not answer as an RT5514 (%#x): not set up\n", id2);
		return false;
	}
	for (unsigned i = 0; i < sizeof(probe_patch) / sizeof(probe_patch[0]); i++) {
		ok = codec_write(probe_patch[i][0], probe_patch[i][1]) && ok;
	}
	ok = ok && codec_bits(0x2100, 0x7u << 8, 0x3u << 8);                    /* PLL2's source: the bit clock */
	ok = ok && codec_write(R(0x2250), 6u << 16 | 6u << 4);                  /* PLL2: k 6, n 6, m 0 */
	ok = ok && codec_bits(0x2254, 1u << 2, 1u << 2);                        /* ... M bypassed */
	ok = ok && codec_bits(0x2108, 0x777u, 0x3u);                            /* system clock: the PLL, no divider, OSR 0 */
	ok = ok && codec_bits(0x2010, 1u << 28 | 1u << 30 | 0x7u << 16 | 1u << 25 | 1u << 26 | 0xfffu,
	    1u << 28 | format << 16 | 0x3u << 10 | 0x3u << 8 | 0x3u << 6 | 0x3u << 4);  /* TDM, DSP-A, 8 x 32, 16-bit data */
	ok = ok && codec_bits(0x2104, 0xfu | 0x7u << 8 | 1u << 23, 0x1u | 0x2u << 8 | 1u << 23);    /* ANA1 sel 1, DMIC /4, AD0 */
	ok = ok && codec_bits(0x20a4, 0x1u, 0x0u);                              /* Stereo1 from DMIC1 */
	ok = ok && codec_bits(0x2254, 0x1u, 0x1u);                              /* the PLL2's LDO enabled */
	ok = ok && codec_bits(0x2008, 1u << 20, 1u << 20);                      /* ... powered */
	delay_for_interval(1, NSEC_PER_MSEC);
	ok = ok && codec_bits(0x2008, 1u << 22, 1u << 22);                      /* the PLL2 powered */
	ok = ok && codec_bits(0x2190, 1u << 7, 0);                              /* Stereo1 unmuted */
	ok = ok && codec_bits(0x2194, 1u << 7, 0);
	ok = mic_supply(true) && ok;
	(void)codec_read(R(0x2010), &i2s);
	(void)codec_read(R(0x2104), &clk1);
	(void)codec_read(R(0x2108), &clk2);
	(void)codec_read(R(0x2008), &pwr2);
	(void)codec_read(R(0x2250), &pll);
	(void)codec_read(R(0x2100), &src);
	say("mic: the codec set up %s: I2S_CTRL1 %#x, CLK_CTRL1 %#x, CLK_CTRL2 %#x, PWR_ANA2 %#x, PLL2 %#x, PLL source %#x\n",
	    ok ? "(every write acknowledged)" : "(A WRITE WAS NOT ACKNOWLEDGED)", i2s, clk1, clk2, pwr2, pll, src);
	codec_running = ok;
	return ok;
}

/* the mics' supply off, the ADC filter clock and the PLL2 powered down */
void hakios_rt5514_stop(void);
void
hakios_rt5514_stop(void)
{
	if (!codec_running) {
		return;
	}
	codec_running = false;
	(void)mic_supply(false);
	(void)codec_bits(0x2104, 1u << 23, 0);
	(void)codec_bits(0x2008, 1u << 22 | 1u << 20, 0);
	(void)codec_bits(0x2254, 0x1u, 0);
	say("mic: the codec powered down\n");
}

/* ---- the steps -------------------------------------------------------------------------------------- */

static void
look(void)
{
	static const unsigned tdm[] = { 53, 54, 55, 56 };
	static const char *const tdm_name[] = { "TDM clock", "TDM frame", "TDM data in", "TDM data out" };
	uint32_t id1 = 0, id2 = 0;
	bool released = false;

	if (!map_pin(PIN_RESET) || !map_pin(PIN_SDA) || !map_pin(PIN_SCL)) {
		say("mic: the TLMM cannot be mapped\n");
		return;
	}
	show_pin(PIN_RESET, "the codec's reset, high = running");
	show_pin(PIN_SDA, "I2C data");
	show_pin(PIN_SCL, "I2C clock");
	for (unsigned i = 0; i < 4; i++) {
		if (map_pin(tdm[i])) {
			show_pin(tdm[i], tdm_name[i]);
		}
	}
	if (!((rd(GPIO_CFG(PIN_RESET)) & CFG_OUTPUT) && (rd(GPIO_IO(PIN_RESET)) & 2))) {
		/* held in reset (or not driven): released as Android's probe does, high, and given 10 ms */
		wr(GPIO_IO(PIN_RESET), 2);
		wr(GPIO_CFG(PIN_RESET), CFG_OUTPUT);
		released = true;
		delay_for_interval(10, NSEC_PER_MSEC);
		say("mic: the reset line was not high: released (driven high, 2 mA)\n");
	}
	/* (Android reads the ID twice: a chip whose lines glitched at power-up misses the first read) */
	if (!codec_read(0x18002ff4, &id2) || id2 != 0x10ec5514) {
		(void)codec_read(0x18002ff4, &id2);
	}
	if (codec_read(0x18002ff4, &id2) && codec_read(0x18002ff0, &id1)) {
		say("mic: the codec at 0x%02x answers: ID %#010x%s, version register %#x%s\n", CODEC_ADDRESS, id2,
		    id2 == 0x10ec5514 ? " (an RT5514)" : " (NOT an RT5514)", id1, id1 == 0x80 ? " (the RT5514P)" : "");
	} else {
		say("mic: no answer from 0x%02x on gpio%u/%u\n", CODEC_ADDRESS, PIN_SDA, PIN_SCL);
	}
	(void)released;
}

static void
mic_thread(__unused void *parameter, __unused wait_result_t result)
{
	say("mic: level %u\n", level);
	look();
	say("mic: done\n");
	thread_terminate(current_thread());
}

static thread_call_t mic_call;

static void
mic_go(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;

	if (kernel_thread_start(mic_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

__startup_func
static void
hakios_rt5514_sunfish_init(void)
{
	uint64_t deadline;

	if (!PE_parse_boot_argn("mic", &level, sizeof(level)) || level == 0) {
		return;
	}
	mic_call = thread_call_allocate(mic_go, NULL);
	clock_interval_to_deadline(4, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(mic_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_rt5514_sunfish_init);
