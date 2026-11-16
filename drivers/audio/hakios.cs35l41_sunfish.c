/*
 * The Pixel 4a's two speaker amplifiers (Cirrus CS35L41, I2C 0x40 and 0x41): STEP ONE, are they there.
 *
 * Status: COMPILED ONLY (2026-11-16). Not run on the phone.
 *
 * Facts (the phone's Android device tree, as Yofuness/ios-sunfish's docs/features/audio.md reads it and its patch 0217
 * uses it; none of its code is here):
 *   - the amplifiers are on the serial engine 9's two pins, GPIO 46 (data) and 47 (clock): the same two lines as the
 *     vibration motor's chip (0x5a), which hakios.haptic_drv2624.c drives by hand. This file asks that driver for every
 *     transfer (hakios_sunfish_i2c: one lock for the bus).
 *   - their reset lines are PM6150L's GPIO 8 (the amplifier at 0x40) and GPIO 11 (0x41): SPMI slave 4, peripherals 0xc7
 *     and 0xca, active high, so released = output high. Registers as Linux's pinctrl-spmi-gpio.c names them (LV/MV
 *     subtypes 0x10..0x13): 0x40 MODE_CTL (1 = digital output), 0x44 DIG_OUT_SOURCE_CTL (bit 7 the level), 0x46 EN_CTL
 *     (bit 7 enabled).
 *   - a CS35L41 has 32-bit registers at 32-bit addresses, both sent most significant byte first; DEVID (0x000000) reads
 *     0x35a40, REVID (0x000004) the revision (0xb2 for the B2 this phone's firmware files are named after).
 *
 * Boot-arg amp=N, amp_delay=S seconds after the kernel's start (default 30):
 *   1  reads   the two GPIOs' type and their six registers, through the PMIC arbiter's observer channel. Nothing written.
 *   2  writes  the two reset lines released (the three registers above), then reads DEVID and REVID of each amplifier;
 *              then puts the six registers of each GPIO back as they were (unless amp_keep=1): the amplifiers go back
 *              into reset, as the bootloader left them.
 * No amplifier register is written here: power-up, the boost converter and the volume are the next step, and they
 * start 20 dB down with the DSP's clock running first.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern kern_return_t thread_terminate(thread_t thread);
extern void loader_log_putc(char c);
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);
extern bool hakios_sunfish_i2c(uint8_t address, const uint8_t *tx, unsigned nt, uint8_t *rx, unsigned nr);

#define SP_CORE_PA       0x0c440000u
#define SP_OBS_PA        0x0e600000u
#define SP_CFG_PA        0x0c40a000u
#define SP_CHNL_PA       0x0c600000u    /* this execution environment's write channels, 0x10000 each */
#define PM6150L_SID      4u

static void say(const char *format, ...) __printflike(1, 2);

static void
say(const char *format, ...)
{
	char text[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	printf("%s", text);
	for (const char *c = text; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

static uint32_t
rd(vm_offset_t base, uint32_t offset)
{
	return *(volatile uint32_t *)(base + offset);
}

/* ---- the PMIC arbiter (version 5), as hakios.ftm5.c speaks to it ---- */

static vm_offset_t sp_core, sp_cfg, sp_obs;
static bool sp_poisoned;

struct pmic_gpio {
	const char *name;
	uint32_t peripheral;            /* 0xc7, 0xca */
	uint32_t apid;                  /* the arbiter's channel for it; ~0: none */
	bool ours;                      /* this execution environment may write it */
	vm_offset_t channel;
	uint8_t before[6];
	bool read;
};

static const uint8_t gpio_regs[6] = { 0x40, 0x41, 0x42, 0x44, 0x45, 0x46 };

static bool
sp_find(struct pmic_gpio *g)
{
	uint32_t count = rd(sp_core, 4) & 0x7ffu, ppid = PM6150L_SID << 8 | g->peripheral;

	g->apid = ~0u;
	for (uint32_t i = 0; i < count && i < 512; i++) {
		uint32_t map = rd(sp_core, 0x900u + 4u * i);

		if (map != 0 && ((map >> 8) & 0xfffu) == ppid) {
			bool owned = (rd(sp_cfg, 0x700u + 4u * i) & 7u) == 0;

			if (g->apid == ~0u || owned) {
				g->apid = i;
				g->ours = owned;
			}
		}
	}
	return g->apid != ~0u;
}

static bool
sp_read(const struct pmic_gpio *g, uint32_t reg, uint8_t *out)
{
	uint32_t at = 0x80u * g->apid;
	bool ok = false, completed = false;

	if (sp_poisoned) {
		return false;
	}
	hakios_spmi_lock();
	__asm__ volatile ("dmb oshst" ::: "memory");
	*(volatile uint32_t *)(sp_obs + at) = (1u << 27) | (reg << 4);          /* EXT_READL, one byte */
	__asm__ volatile ("dsb osh" ::: "memory");
	for (unsigned i = 0; i < 1000 && !completed; i++) {
		uint32_t status = rd(sp_obs, at + 0x08u);

		if (status & 1u) {
			completed = true;
			if (!(status & 0x0eu)) {
				*out = (uint8_t)rd(sp_obs, at + 0x18u);
				ok = true;
			}
		} else {
			delay(1);
		}
	}
	hakios_spmi_unlock();
	if (!completed) {
		sp_poisoned = true;
		say("amp: PMIC read of %s register %#x never completed; the arbiter is not used again\n", g->name, reg);
	}
	return ok;
}

/* Only the three registers this file is about (and, putting back, the six that were read), on a channel of ours. */
static bool
sp_write(struct pmic_gpio *g, uint32_t reg, uint8_t value)
{
	if (sp_poisoned || !g->ours || reg < 0x40u || reg > 0x46u) {
		say("amp: write of %s register %#x refused\n", g->name, reg);
		return false;
	}
	if (g->channel == 0) {
		g->channel = ml_io_map(SP_CHNL_PA + 0x10000u * g->apid, 0x1000);
		if (g->channel == 0) {
			return false;
		}
	}
	for (unsigned attempt = 0; attempt < 6; attempt++) {
		uint32_t status = 0;
		bool completed = false;

		hakios_spmi_lock();
		__asm__ volatile ("dmb oshst" ::: "memory");
		*(volatile uint32_t *)(g->channel + 0x10u) = value;             /* WDATA0 */
		*(volatile uint32_t *)(g->channel + 0x00u) = reg << 4;          /* EXT_WRITEL, one byte */
		__asm__ volatile ("dsb osh" ::: "memory");
		for (unsigned i = 0; i < 1000 && !completed; i++) {
			status = rd(g->channel, 0x08u);
			if (status & 1u) {
				completed = true;
			} else {
				delay(1);
			}
		}
		hakios_spmi_unlock();
		if (!completed) {
			sp_poisoned = true;
			say("amp: PMIC write of %s register %#x never completed; the arbiter is not used again\n", g->name, reg);
			return false;
		}
		if (!(status & 0x0eu)) {
			return true;
		}
		delay(300);             /* a failure, denied or dropped flag: tried again (seen on this phone with the touch screen's switch) */
	}
	say("amp: PMIC write of %s register %#x refused by the arbiter six times\n", g->name, reg);
	return false;
}

static bool
gpio_look(struct pmic_gpio *g)
{
	uint8_t type = 0, subtype = 0;

	if (!sp_find(g)) {
		say("amp: the arbiter has no channel for %s (slave %u, peripheral %#x)\n", g->name, PM6150L_SID, g->peripheral);
		return false;
	}
	if (!sp_read(g, 0x04, &type) || !sp_read(g, 0x05, &subtype)) {
		say("amp: %s: its type could not be read\n", g->name);
		return false;
	}
	g->read = true;
	for (unsigned i = 0; i < 6; i++) {
		g->read = g->read && sp_read(g, gpio_regs[i], &g->before[i]);
	}
	say("amp: %s: channel %u (%s), type %#x subtype %#x; mode %#x, 0x41 %#x, pull %#x, out source %#x, 0x45 %#x, enable %#x%s\n",
	    g->name, g->apid, g->ours ? "ours to write" : "NOT ours to write", type, subtype, g->before[0], g->before[1],
	    g->before[2], g->before[3], g->before[4], g->before[5], g->read ? "" : " (not all read)");
	return g->read && type == 0x10 && subtype >= 0x10 && subtype <= 0x13;
}

/* ---- the amplifiers ---- */

static bool
amp_read(uint8_t address, uint32_t reg, uint32_t *value)
{
	uint8_t tx[4] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg }, rx[4] = { 0 };

	if (!hakios_sunfish_i2c(address, tx, 4, rx, 4)) {
		return false;
	}
	*value = (uint32_t)rx[0] << 24 | (uint32_t)rx[1] << 16 | (uint32_t)rx[2] << 8 | rx[3];
	return true;
}

static void
amp_thread(__unused void *parameter, __unused wait_result_t result)
{
	static struct pmic_gpio gpios[2] = { { .name = "PM6150L GPIO 8", .peripheral = 0xc7 }, { .name = "PM6150L GPIO 11", .peripheral = 0xca } };
	static const uint8_t addresses[2] = { 0x40, 0x41 };
	uint32_t level = 0, wait = 30, keep = 0;
	bool usable[2] = { false, false };

	PE_parse_boot_argn("amp", &level, sizeof(level));
	PE_parse_boot_argn("amp_delay", &wait, sizeof(wait));
	PE_parse_boot_argn("amp_keep", &keep, sizeof(keep));
	delay_for_interval(wait * 1000, 1000 * 1000);
	say("amp: the speaker amplifiers, level %u: %s\n", level, level == 1 ? "a look at their reset lines, reading only" :
	    "their reset lines released, their identity read");
	sp_core = ml_io_map(SP_CORE_PA, 0x2000);
	sp_cfg = ml_io_map(SP_CFG_PA, 0x1000);
	sp_obs = ml_io_map(SP_OBS_PA, 0x10000);
	if (sp_core == 0 || sp_cfg == 0 || sp_obs == 0 || rd(sp_core, 0) < 0x50000000u || rd(sp_core, 0) >= 0x70000000u) {
		say("amp: the PMIC arbiter is not one this driver knows\n");
		goto out;
	}
	for (unsigned i = 0; i < 2; i++) {
		usable[i] = gpio_look(&gpios[i]);
	}
	if (level < 2) {
		goto out;
	}
	for (unsigned i = 0; i < 2; i++) {
		uint32_t id = 0, revision = 0;
		bool released;

		if (!usable[i] || !gpios[i].ours) {
			say("amp: %s is not a GPIO of the kind expected, or not ours: left alone, the amplifier at %#x not asked\n",
			    gpios[i].name, addresses[i]);
			continue;
		}
		say("amp: about to release %s (mode 1, out source 0x80, enable 0x80)\n", gpios[i].name);
		released = sp_write(&gpios[i], 0x40, 0x01) && sp_write(&gpios[i], 0x44, 0x80) && sp_write(&gpios[i], 0x46, 0x80);
		delay_for_interval(5, 1000 * 1000);
		if (!released) {
			say("amp: %s could not be released\n", gpios[i].name);
		} else if (amp_read(addresses[i], 0x000000, &id) && amp_read(addresses[i], 0x000004, &revision)) {
			say("amp: THE AMPLIFIER AT %#x ANSWERS: device %#x%s, revision %#x\n", addresses[i], id,
			    id == 0x35a40 ? " (a CS35L41)" : " (NOT the 0x35a40 expected)", revision);
		} else {
			say("amp: the amplifier at %#x does not answer on the bus\n", addresses[i]);
		}
		if (!keep) {
			bool back = true;

			for (int r = 5; r >= 0; r--) {          /* enable first, mode last: the way back */
				back = sp_write(&gpios[i], gpio_regs[r], gpios[i].before[r]) && back;
			}
			say("amp: %s put back as it was: %s\n", gpios[i].name, back ? "done" : "NOT wholly");
		}
	}
out:
	say("amp: done\n");
	thread_terminate(current_thread());
}

static thread_call_t amp_call;

static void
amp_go(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;
	uint32_t level = 0;

	if (PE_parse_boot_argn("amp", &level, sizeof(level)) && level != 0 &&
	    kernel_thread_start(amp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

__startup_func
static void
hakios_cs35l41_sunfish_init(void)
{
	uint64_t deadline;

	amp_call = thread_call_allocate(amp_go, NULL);
	clock_interval_to_deadline(3, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(amp_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_cs35l41_sunfish_init);
