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
