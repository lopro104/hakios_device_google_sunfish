/*
 * The Pixel 4a's two speaker amplifiers (Cirrus CS35L41, I2C 0x40 and 0x41): STEP ONE, are they there.
 *
 * Status: amp=2 RUN ON THE PHONE 2026-11-16 (logs/143600-run.log): both answer, 0x35a40 revision 0xb2. The bring-up
 * further down (hakios_amp_prepare/start/stop, for adsp=7) is compiled only.
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
mmio(vm_offset_t base, uint32_t offset)
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
	uint32_t count = mmio(sp_core, 4) & 0x7ffu, ppid = PM6150L_SID << 8 | g->peripheral;

	g->apid = ~0u;
	for (uint32_t i = 0; i < count && i < 512; i++) {
		uint32_t map = mmio(sp_core, 0x900u + 4u * i);

		if (map != 0 && ((map >> 8) & 0xfffu) == ppid) {
			bool owned = (mmio(sp_cfg, 0x700u + 4u * i) & 7u) == 0;

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
		uint32_t status = mmio(sp_obs, at + 0x08u);

		if (status & 1u) {
			completed = true;
			if (!(status & 0x0eu)) {
				*out = (uint8_t)mmio(sp_obs, at + 0x18u);
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
			status = mmio(g->channel, 0x08u);
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


/* ==== the amplifier itself =========================================================================================
 * Ported from Yofuness/ios-sunfish phone/wifi/cs35l41.c and cs35l41.h (MIT License, Copyright (c) 2026 dipa-studio and
 * the ios-sunfish authors; its sequence follows Linux's cs35l41.c / cs35l41-lib.c and ios-redfin's patch 0953), which
 * played the first tone on this phone. What is changed: the bus (the haptic driver's lines), the log, the waits. Its
 * limits are kept: digital volume never above -20 dB, the boost converter bypassed unless asked, GLOBAL_EN only after
 * every configured field has read back, any error bit powers the amplifier down.
 */
static int quiet = 1;           /* no line per register access: the loader's log is small */

static void cs_say(const char *format, ...) __printflike(1, 2);

static void
cs_say(const char *format, ...)
{
	char text[256];
	va_list arguments;

	va_start(arguments, format);
	vsnprintf(text, sizeof(text), format, arguments);
	va_end(arguments);
	say("%s", text);
}

static void
cs_wait_us(uint32_t us)
{
	delay_for_interval(us < 1000 ? 1 : us / 1000, 1000 * 1000);
}

#define DEVID		0x00000000	/* 0x35a40 */
#define REVID		0x00000004	/* [7:0] 0xb2 = B2 */
#define OTPID		0x00000010	/* sp6: selects the OTP map for the trim unpack */
#define OTP_MEM0	0x00000400	/* sp6: 32 words of packed trims */
#define OTP_WORDS	32
#define TEST_KEY_CTL	0x00000040	/* 0x55, 0xaa unlock; 0xcc, 0x33 lock */
#define PWR_CTRL1	0x00002014	/* bit0 GLOBAL_EN */
#define PWR_CTRL2	0x00002018	/* bit0 AMP_EN, [5:4] BST_EN 0 off / 1 bypass (FET on, VBST = VP) / 2 boost */
#define PWR_CTRL3	0x0000201c	/* sp6: was 0x2028 (wrong) */
#define AMP_OUT_MUTE	0x00002024	/* sp8: bit4 OUT_MUTE */
#define PROTECT_REL	0x00002034	/* error release bits */
#define PUP_PDN_PATCH	0x00002084	/* 0x002f1aa0 before GLOBAL_EN, 0x002f1aa3 after power-down (unlocked) */
#define PLL_CLK_CTRL	0x00002c04	/* [2:0] REFCLK_SEL 0 = BCLK, bit4 REFCLK_EN, [10:5] FREQ, bit11 OPEN_LOOP */
#define GLOBAL_CLK_CTRL	0x00002c0c	/* [4:0] GLOBAL_FS, 48 kHz = 0x03 */
#define TST_FS_MON0	0x00002d10	/* sp8: FS1 window [10:0], FS2 [23:12]; test register (unlocked) */
#define BST_PEAK_CUR	0x00003808	/* sp8: [6:0] IPK */
#define BST_COEFF	0x00003810	/* sp8: K1 [7:0], K2 [15:8] */
#define BST_SLOPE_LBST	0x00003814	/* sp8: slope [15:8], LBST [1:0] */
#define SP_RATE_CTRL	0x00004804	/* sp8: [5:0] ASP_BCLK_FREQ */
#define SP_ENABLES	0x00004800	/* bit16 ASP_RX1_EN */
#define SP_FORMAT	0x00004808	/* RX width [31:24], TX width [23:16], FMT [10:8] 2 = I2S, masters bits 0/4, inv 2/6 */
#define SP_HIZ_CTRL	0x0000480c	/* [1:0] = DT cirrus,asp-sdout-hiz */
#define SP_RX_SLOT	0x00004820	/* RX1 slot [5:0] */
#define SP_RX_WL	0x00004840	/* [5:0] word length */
#define DAC_PCM1_SRC	0x00004c00	/* [6:0] 0x08 = ASPRX1 (DSP bypass) */
#define AMP_DIG_VOL	0x00006000	/* [13:3] signed, 0.125 dB, 0 = 0 dB; bit15 kept (reset value) */
#define AMP_GAIN_CTRL	0x00006c04	/* PCM gain [9:5]; 0 = 0.5 dB (B2 errata), reset value restored at the end */
#define AMP_GAIN_MAX	10		/* sp3: 10.5 dB ceiling for --amp-gain */
#define IRQ1_STATUS1	0x00010010	/* W1C */
#define IRQ1_STATUS3	0x00010018	/* sp2: bit1 PLL_LOCK */
#define IRQ1_STATUS4	0x0001001c
#define IRQ3_PLL_LOCK	(1u << 1)
#define IRQ_PUP_DONE	(1u << 24)
#define IRQ_PDN_DONE	(1u << 23)
#define IRQ_AMP_SHORT	(1u << 31)
#define IRQ_TEMP_ERR	(1u << 17)
#define IRQ_TEMP_WARN	(1u << 15)
#define IRQ_BST_SHORT	(1u << 8)
#define IRQ_BST_UVP	(1u << 7)
#define IRQ_BST_OVP	(1u << 6)
#define IRQ_ERRORS	(IRQ_AMP_SHORT | IRQ_TEMP_ERR | IRQ_TEMP_WARN | IRQ_BST_SHORT | IRQ_BST_UVP | IRQ_BST_OVP)

#define BCLK_CODE	0x1b		/* 1.536 MHz (cs35l41.c:66 PLL table) */
#define BCLK_CODE_32	0x21		/* pcm32: 3.072 MHz, Linux cs35l41-lib.c cs35l41_pll_sysclk { 3072000, 0x21 } */	/* verified: Linux cs35l41.c cs35l41_pll_sysclk { 3072000, 0x21 } (fs_mon: 1536000 -> 52/84, 3072000 -> 28/44) */
#define BCLK_CODE_OF(a)	((a)->slot32 ? BCLK_CODE_32 : BCLK_CODE)
#define VOL_CEIL_DB	(0)		/* the caller's to hold lower (the tone: -20). At 0 dB with the analog gain at 0.5 dB and the boost
					 * bypassed this is still some 19 dB under what the stock system drives the speaker with (its analog
					 * gain is 19.5 dB, with the amplifier's own protection firmware, which is not loaded here) */
#define VOL_FLOOR_DB	(-102)		/* tlv minimum 0x4cf */
#define AMP_OK(x)	((x) == 0x40 || (x) == 0x41)	/* ep1: 0x40 = top (earpiece, DT reset_pin_top), 0x41 = bottom loudspeaker */

#define CS_MAX_EXP	32

struct cs_exp {
	uint32_t reg, mask, val;
};

struct cs_amp {
	uint8_t addr;
	int right;		/* takes ASP RX slot 1 (unused: 0x41 is never written) */
	int amp_gain;		/* sp3: analog PCM gain steps after the errata (0.5 dB + 1 dB/step), clamped 0..10 */
	uint32_t gain_saved;	/* AMP_GAIN_CTRL before us, restored by cs_power_down */
	uint32_t err_bits;	/* IRQ1 error bits seen */
	int probed, configured, verified, up;
	int out_mute;		/* sp8: AMP_OUT_MUTE set at config, cleared 2 ms after GLOBAL_EN (ios-redfin 0953) */
	int clk;		/* sp8: SP_RATE_CTRL 0x28 + TST_FS_MON0 16 | 36 << 12 (ios-redfin 0953, Linux set_sysclk) */
	int slot32;		/* pcm32: --i2s-32: 32-bit ASP slots, BCLK 3.072 MHz (code 0x21) instead of 16 / 1.536 MHz */
	int nexp;
	struct cs_exp exp[CS_MAX_EXP];	/* read-back list */
};


static int
rd(struct cs_amp *a, uint32_t reg, uint32_t *v)
{
	if (!amp_read(a->addr, reg, v)) {
		cs_say("cs35l41 %#x: read %08x: no answer\n", a->addr, reg);
		return -1;
	}
	return 0;
}

static int
wr(struct cs_amp *a, uint32_t reg, uint32_t v)
{
	uint8_t p[8] = { (uint8_t)(reg >> 24), (uint8_t)(reg >> 16), (uint8_t)(reg >> 8), (uint8_t)reg,
		         (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };

	if (!AMP_OK(a->addr)) {
		return -1;
	}
	if (!hakios_sunfish_i2c(a->addr, p, 8, NULL, 0)) {
		cs_say("cs35l41 %#x: write %08x: no answer\n", a->addr, reg);
		return -1;
	}
	return 0;
}

/* sp1: what a register should read back as (fields merged over all writes to it) */
static void expect(struct cs_amp *a, uint32_t reg, uint32_t mask, uint32_t val)
{
	int i;
	for (i = 0; i < a->nexp && a->exp[i].reg != reg; i++)
		;
	if (i == a->nexp) {
		if (a->nexp == CS_MAX_EXP)
			return;	/* cannot happen: CS_MAX_EXP covers the whole sequence */
		a->exp[a->nexp++] = (struct cs_exp){ reg, 0, 0 };
	}
	a->exp[i].mask |= mask;
	a->exp[i].val = (a->exp[i].val & ~mask) | (val & mask);
}

static int set(struct cs_amp *a, uint32_t reg, uint32_t v)
{
	expect(a, reg, 0xffffffffu, v);
	return wr(a, reg, v);
}

static int upd(struct cs_amp *a, uint32_t reg, uint32_t mask, uint32_t val)
{
	uint32_t v;
	expect(a, reg, mask, val);
	int err = rd(a, reg, &v);
	return err ? err : wr(a, reg, (v & ~mask) | (val & mask));
}

static int unlock(struct cs_amp *a)
{
	return wr(a, TEST_KEY_CTL, 0x55) | wr(a, TEST_KEY_CTL, 0xaa);
}

static int lock(struct cs_amp *a)
{
	return wr(a, TEST_KEY_CTL, 0xcc) | wr(a, TEST_KEY_CTL, 0x33);
}

/* pup (0x002f1aa0) / pdn (0x002f1aa3) patch, test keys around it */
static int patch(struct cs_amp *a, uint32_t v)
{
	int err = unlock(a);
	err |= wr(a, PUP_PDN_PATCH, v);
	return err | lock(a);
}

static int cs_probe(struct cs_amp *a)
{
	static const struct { const char *n; uint32_t r; } regs[] = {
		{ "devid", DEVID }, { "revid", REVID }, { "otpid", OTPID }, { "pwr_ctrl1", PWR_CTRL1 },
		{ "pwr_ctrl2", PWR_CTRL2 }, { "pwr_ctrl3", PWR_CTRL3 }, { "amp_dig_vol", AMP_DIG_VOL },
		{ "amp_gain_ctrl", AMP_GAIN_CTRL }, { "irq1_status1", IRQ1_STATUS1 }, { "irq1_status2", IRQ1_STATUS1 + 4 },
		{ "irq1_status3", IRQ1_STATUS1 + 8 }, { "irq1_status4", IRQ1_STATUS4 },
	};
	a->probed = 0;
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
		uint32_t v;
		int err = rd(a, regs[i].r, &v);
		if (err) {
			if (!i)
				cs_say("cs35l41 %#x: no answer (held in reset? PM6150L GPIO %d)\n", a->addr,
				       a->addr == 0x40 ? 8 : 11);
			return err;
		}
		if (!quiet)
			cs_say("cs35l41 %#x: %s %08x = %08x\n", a->addr, regs[i].n, regs[i].r, v);
		/* sp1: abort unless devid 0x35a40, revid B2 (the errata below are B2's) and GLOBAL_EN still 0 */
		if (((regs[i].r == DEVID && (v & 0xffffff) != 0x35a40) ||
			     (regs[i].r == REVID && (v & 0xff) != 0xb2) || (regs[i].r == PWR_CTRL1 && (v & 1)))) {
			cs_say("cs35l41 %#x: unexpected %s %08x: stop\n", a->addr, regs[i].n, v);
			return -1;
		}
		if (regs[i].r == AMP_GAIN_CTRL)
			a->gain_saved = v;
	}
	a->probed = 1;
	return 0;
}

/*
 * sp6: OTP trim unpack (facts from Linux cs35l41-lib.c otp_map_1/otp_map_2 and cs35l41_otp_unpack): the fuses at
 * OTP_MEM0 hold bit fields packed LSB-first from word 2 bit 16; each field goes into {reg, shift, size}; reg 0 =
 * padding (skipped). Map 2 = map 1 with VMON_POL (0x4000 bit 11) in place of the padding bit. OTPID 1/8 -> map 1,
 * 2/3/6 -> map 2; other ids: skipped. Runs unlocked, right after the B2 errata. CS35L41_NO_OTP=1 turns it off.
 */
struct otp_field {
	uint32_t reg;
	uint8_t shift, size;
};

static const struct otp_field otp_fields[] = {
	{ 0x2030, 0, 4 }, { 0x2030, 7, 1 }, { 0x208c, 24, 6 }, { 0x2090, 14, 4 }, { 0x2090, 10, 4 },
	{ 0x300c, 11, 4 }, { 0x394c, 23, 2 },
	{ 0x3950, 0, 7 }, { 0x3950, 8, 7 }, { 0x3950, 16, 8 }, { 0x3950, 24, 8 },
	{ 0x3954, 0, 7 }, { 0x3954, 8, 7 }, { 0x3954, 16, 8 }, { 0x3954, 24, 8 },
	{ 0x3958, 0, 7 }, { 0x3958, 8, 7 }, { 0x3958, 16, 8 }, { 0x3958, 24, 8 },
	{ 0x395c, 0, 7 }, { 0x395c, 8, 7 }, { 0x395c, 16, 8 }, { 0x395c, 24, 8 },
	{ 0x416c, 0, 8 }, { 0x4160, 0, 7 }, { 0x416c, 8, 8 }, { 0x4160, 16, 10 }, { 0x416c, 16, 12 },
	{ 0x416c, 28, 1 },
	{ 0x4170, 0, 6 }, { 0x4170, 6, 1 }, { 0x4170, 8, 6 }, { 0x4170, 14, 1 }, { 0x4170, 16, 9 },
	{ 0x4360, 0, 5 }, { 0x4360, 6, 9 },
	{ 0x4448, 0, 8 }, { 0x4448, 8, 8 }, { 0x4448, 16, 8 }, { 0x4448, 24, 8 }, { 0x444c, 0, 3 },
	{ 0x6e30, 0, 5 }, { 0x6e30, 8, 5 }, { 0x6e30, 16, 5 }, { 0x6e30, 24, 5 },
	{ 0x6e34, 0, 5 }, { 0x6e34, 8, 5 }, { 0x6e34, 16, 5 }, { 0x6e34, 24, 5 },
	{ 0x6e38, 0, 5 }, { 0x6e38, 8, 5 }, { 0x6e38, 16, 5 }, { 0x6e38, 24, 5 },
	{ 0x6e3c, 0, 5 }, { 0x6e3c, 8, 5 }, { 0x6e3c, 16, 5 }, { 0x6e3c, 24, 5 },
	{ 0x6e40, 0, 5 }, { 0x6e40, 8, 5 }, { 0x6e40, 16, 5 }, { 0x6e40, 24, 5 }, { 0x6e44, 0, 5 },
	{ 0x6e48, 0, 10 }, { 0x6e48, 10, 10 }, { 0x6e48, 20, 10 }, { 0x6e4c, 0, 10 }, { 0x6e4c, 10, 10 },
	{ 0x6e4c, 20, 10 }, { 0x6e50, 0, 10 }, { 0x6e50, 10, 10 }, { 0x6e50, 20, 10 }, { 0x6e54, 0, 10 },
	{ 0x6e54, 10, 10 }, { 0x6e54, 20, 10 }, { 0x6e58, 0, 10 }, { 0x6e58, 10, 10 }, { 0x6e58, 20, 10 },
	{ 0x6e5c, 0, 10 }, { 0x6e5c, 10, 10 }, { 0x6e5c, 20, 10 }, { 0x6e60, 0, 10 }, { 0x6e60, 10, 10 },
	{ 0x6e60, 20, 10 }, { 0x6e64, 0, 10 },
	{ 0x7418, 7, 5 }, { 0x741c, 0, 5 }, { 0x741c, 11, 4 }, { 0x741c, 19, 4 }, { 0x7434, 17, 1 },
	{ 0x7434, 18, 7 }, { 0x7068, 0, 9 }, { 0x410c, 7, 1 }, { 0x400c, 0, 7 },
	{ 0x0000, 0, 1 },	/* OTP_PAD: map 1 padding bit, map 2 VMON_POL */
	{ 0x17040, 0, 8 }, { 0x17040, 8, 8 }, { 0x17040, 16, 8 }, { 0x17040, 24, 8 }, { 0x17044, 0, 24 },
};
#define OTP_PAD		93	/* index of the map 1 / map 2 difference */

/* call unlocked; 0 = done or skipped (unknown id, CS35L41_NO_OTP), -1 = I2C error */
static int otp_unpack(struct cs_amp *a)
{
	uint32_t id, mem[OTP_WORDS];
	if (rd(a, OTPID, &id))
		return -1;
	int map = id == 1 || id == 8 ? 1 : id == 2 || id == 3 || id == 6 ? 2 : 0;
	if (!map) {
		cs_say("cs35l41 %#x: otp: no map for OTPID %#x: skipped (trims stay default)\n", a->addr, id);
		return 0;
	}
	for (int i = 0; i < OTP_WORDS; i++)
		if (rd(a, OTP_MEM0 + 4 * i, &mem[i]))
			return -1;
	int n = sizeof(otp_fields) / sizeof(otp_fields[0]), nw = 0, err = 0;
	cs_say("cs35l41 %#x: otp: OTPID %#x -> map %d (Linux otp_map_%d), %d fields\n", a->addr, id, map, map, n);
	unsigned pos = 2 * 32 + 16;	/* word 2, bit 16 */
	for (int i = 0; i < n; i++) {
		struct otp_field f = otp_fields[i];
		if (i == OTP_PAD && map == 2)
			f = (struct otp_field){ 0x4000, 11, 1 };	/* VMON_POL */
		unsigned w = pos / 32, b = pos % 32;
		pos += f.size;
		if (w >= OTP_WORDS || (b + f.size > 32 && w + 1 >= OTP_WORDS)) {
			cs_say("cs35l41 %#x: otp: field %d past the end: stop\n", a->addr, i);
			return -1;
		}
		uint64_t win = mem[w] | (b + f.size > 32 ? (uint64_t)mem[w + 1] << 32 : 0);
		uint32_t fm = (uint32_t)((1ull << f.size) - 1);
		uint32_t val = (uint32_t)(win >> b) & fm, mask = fm << f.shift, v;
		if (!f.reg)
			continue;	/* padding */
		if (rd(a, f.reg, &v))
			return -1;
		if (!quiet)
			cs_say("cs35l41 %#x: otp: trim %08x mask %08x val %08x\n", a->addr, f.reg, mask, val << f.shift);
		err |= wr(a, f.reg, (v & ~mask) | (val << f.shift));
		nw++;
	}
	cs_say("cs35l41 %#x: otp: %d trim fields written%s\n", a->addr, nw, err ? " (write errors)" : "");
	return err ? -1 : 0;
}

/* sp8: AMP_OUT_MUTE read and logged */
static void cs_out_mute(struct cs_amp *a, const char *tag)
{
	uint32_t v;
	if (!rd(a, AMP_OUT_MUTE, &v))
		cs_say("cs35l41 %#x: AMP_OUT_MUTE %s %08x (bit4 %s)\n", a->addr, tag, v, v & 0x10 ? "muted" : "clear");
}

static int cs_config(struct cs_amp *a, int vol_db, int boost)
{
	if (!a->probed || !AMP_OK(a->addr)) {
		cs_say("cs35l41 %#x: config refused (%s)\n", a->addr, a->probed ? "only 0x40/0x41" : "not probed");
		return -1;
	}
	if (vol_db > VOL_CEIL_DB)
		vol_db = VOL_CEIL_DB;	/* sp1: hard ceiling */
	if (vol_db < VOL_FLOOR_DB)
		vol_db = VOL_FLOOR_DB;
	a->nexp = a->verified = 0;
	a->configured = 1;	/* from here on cs_power_down restores */
	int err = unlock(a);
	/* B2 errata (cs35l41-lib.c:667): amp + boost off, analog gain 0 (0.5 dB) */
	err |= set(a, 0x00004100, 0);
	err |= set(a, 0x00004310, 0);
	err |= set(a, 0x00004400, 0);
	err |= set(a, 0x0000381c, 0x51);
	err |= set(a, 0x02bc20e0, 0);
	err |= set(a, 0x02bc2020, 0);
	err |= set(a, PWR_CTRL2, 0);
	err |= set(a, AMP_GAIN_CTRL, 0);
	err |= set(a, 0x00004c28, 0);
	err |= set(a, 0x00004c2c, 0);
	err |= otp_unpack(a);	/* sp6: still unlocked, as Linux */
	err |= lock(a);
	/* sp3: analog PCM gain AMP_GAIN_CTRL[9:5] (Linux cs35l41.h AMP_GAIN_PCM_MASK 0x3e0), 0.5 dB + 1 dB per step,
	 * hard clamp 0..10 (<= 10.5 dB; stock 19 = 19.5 dB) */
	int g = a->amp_gain < 0 ? 0 : a->amp_gain > AMP_GAIN_MAX ? AMP_GAIN_MAX : a->amp_gain;
	cs_say("cs35l41 %#x: analog gain %d (%d.5 dB)\n", a->addr, g, g);
	err |= upd(a, AMP_GAIN_CTRL, 0x3e0, (uint32_t)g << 5);
	if (boost) {
		/*
		 * sp8 (--amp-boost): internal boost, Linux cs35l41-lib.c cs35l41_boost_config() for the stock DT
		 * (live.dts: boost-ind-nanohenry 1000 -> LBST 0, boost-cap-microfarad 15 -> range 0, boost-peak-milliamp
		 * 3700): k1_table[0][0] 0x24, k2_table[0][0] 0x24, slope_table[0] 0x75, IPK (3700 - 1600) / 50 + 0x10 =
		 * 0x3a; then BST_EN 2. Same values as ios-redfin 0953 configure().
		 */
		cs_say("cs35l41 %#x: boost: K1 0x24 K2 0x24 slope 0x75 LBST 0 IPK 0x3a (3700 mA), BST_EN 2\n", a->addr);
		err |= upd(a, BST_COEFF, 0xffff, 0x24 | 0x24 << 8);
		err |= upd(a, BST_SLOPE_LBST, 0xff03, 0x75 << 8 | 0);
		err |= upd(a, BST_PEAK_CUR, 0x7f, (3700 - 1600) / 50 + 0x10);
		err |= upd(a, PWR_CTRL2, 0x30, 2u << 4);
	} else	/* boost bypass: FET on, VBST = VP */
		err |= upd(a, PWR_CTRL2, 0x30, 1u << 4);
	if (a->clk) {	/* sp8: SP_RATE_CTRL before the PLL, as Linux cs35l41_component_set_sysclk / ios-redfin 0953 */
		uint32_t v;
		err |= upd(a, SP_RATE_CTRL, 0x3f, BCLK_CODE_OF(a));	/* our 1.536 MHz BCLK code (redfin's 0x28 is 6.144 MHz) */
		if (!rd(a, SP_RATE_CTRL, &v))
			cs_say("cs35l41 %#x: clk: SP_RATE_CTRL read back %08x\n", a->addr, v);
	}
	/* PLL from BCLK 1.536 MHz (48 kHz x 16 bit x 2; pcm32: 3.072 MHz, 32 bit), cs35l41.c:841-856 */
	err |= upd(a, PLL_CLK_CTRL, 1u << 11, 1u << 11);		/* open loop */
	err |= upd(a, PLL_CLK_CTRL, 0x7e0, BCLK_CODE_OF(a) << 5);	/* freq */
	err |= upd(a, PLL_CLK_CTRL, 1u << 4, 0);
	err |= upd(a, PLL_CLK_CTRL, 0x7, 0);				/* ref = BCLK */
	err |= upd(a, PLL_CLK_CTRL, 1u << 4, 1u << 4);
	err |= upd(a, PLL_CLK_CTRL, 1u << 11, 0);			/* closed loop */
	err |= upd(a, GLOBAL_CLK_CTRL, 0x1f, 0x03);			/* 48 kHz; SP_RATE_CTRL: sp8 --amp-clk */
	if (a->clk > 1) {	/* sp8: FS monitor (--amp-fsmon only: 16|36<<12 is Linux's >6 MHz value, ours is 1.536 MHz), test keys around it (Linux cs35l41_dai_set_sysclk, ios-redfin 0953) */
		uint32_t v;
		err |= unlock(a);
		err |= set(a, TST_FS_MON0, 16 | 36u << 12);
		if (!rd(a, TST_FS_MON0, &v))
			cs_say("cs35l41 %#x: clk: TST_FS_MON0 read back %08x\n", a->addr, v);
		err |= lock(a);
	}
	/* ASP: I2S slave, 16-bit slots, RX1 from slot 0 (left), 16-bit words, SDOUT hi-z 3 (DT) */
	{	/* pcm32: --i2s-32 = 32-bit RX/TX slot widths; RX_WL stays 16 (top 16 bits of the MSB-first word) */
		uint32_t w = a->slot32 ? 32u : 16u;
		err |= upd(a, SP_FORMAT, 0xffff0755, w << 24 | w << 16 | 2u << 8);
	}
	err |= upd(a, SP_RX_SLOT, 0x3f, 0);
	err |= upd(a, SP_RX_WL, 0x3f, 16);
	err |= upd(a, SP_HIZ_CTRL, 0x3, 3);
	err |= upd(a, SP_ENABLES, 1u << 16, 1u << 16);
	err |= upd(a, DAC_PCM1_SRC, 0x7f, 0x08);
	/* digital volume: vol_db dB, 0.125 dB steps, 11-bit two's complement at [13:3] (-30 dB: 0xb880 with bit15) */
	uint32_t vol = (uint32_t)(-vol_db * 8);
	vol = (0x800 - vol) & 0x7ff;
	cs_say("cs35l41 %#x: digital volume %d dB (field %#x)\n", a->addr, vol_db, vol);
	err |= upd(a, AMP_DIG_VOL, 0x7ff << 3, vol << 3);
	if (a->out_mute) {	/* sp8: output muted until 2 ms after GLOBAL_EN (ios-redfin 0953 configure()) */
		cs_out_mute(a, "before set");
		err |= upd(a, AMP_OUT_MUTE, 0x10, 0x10);
		cs_out_mute(a, "after set");
	}
	err |= upd(a, PWR_CTRL2, 1, 1);					/* AMP_EN */
	return err ? -1 : 0;
}

static int cs_verify(struct cs_amp *a)
{
	int bad = 0;
	a->verified = 0;
	if (!a->configured)
		return -1;
	if (unlock(a))	/* the errata registers are test registers */
		bad = 1;
	for (int i = 0; i < a->nexp; i++) {
		uint32_t v;
		if (rd(a, a->exp[i].reg, &v)) {
			bad = 1;
			continue;
		}
		if ((v & a->exp[i].mask) != a->exp[i].val) {
			cs_say("cs35l41 %#x: verify %08x MISMATCH: read %08x mask %08x want %08x got %08x\n", a->addr,
			       a->exp[i].reg, v, a->exp[i].mask, a->exp[i].val, v & a->exp[i].mask);
			bad = 1;
		} else if (!quiet)
			cs_say("cs35l41 %#x: verify %08x ok (%08x & %08x)\n", a->addr, a->exp[i].reg, v, a->exp[i].mask);
	}
	if (lock(a))
		bad = 1;
	cs_say("cs35l41 %#x: read-back of %d registers: %s\n", a->addr, a->nexp, bad ? "FAILED (no GLOBAL_EN)" : "ok");
	a->verified = !bad;
	return bad ? -1 : 0;
}

static int cs_check(struct cs_amp *a)
{
	uint32_t s;
	if (rd(a, IRQ1_STATUS1, &s)) {
		a->err_bits |= IRQ_ERRORS;	/* unknown state: treat as an error */
		return -1;
	}
	if (s & IRQ_ERRORS) {
		cs_say("cs35l41 %#x: IRQ1 error bits %08x (status %08x)\n", a->addr, s & IRQ_ERRORS, s);
		a->err_bits |= s & IRQ_ERRORS;
		return -1;
	}
	return 0;
}

static int cs_power_up(struct cs_amp *a)
{
	if (!a->verified || !AMP_OK(a->addr) || a->err_bits) {	/* sp1: no GLOBAL_EN without a clean read-back */
		cs_say("cs35l41 %#x: power-up refused (verified %d, errors %08x)\n", a->addr, a->verified, a->err_bits);
		return -1;
	}
	if (patch(a, 0x002f1aa0) || wr(a, IRQ1_STATUS1, 0xffffffffu))
		return -1;
	a->up = 1;	/* from the GLOBAL_EN attempt on, cs_power_down runs the full power-down */
	if (upd(a, PWR_CTRL1, 1, 1))
		return -1;
	int t0 = 0;
	if (a->out_mute) {	/* sp8: unmute 2 ms after GLOBAL_EN, as ios-redfin 0953 sweet_amp_start() */
		cs_wait_us(2000);
		if (upd(a, AMP_OUT_MUTE, 0x10, 0))
			return -1;
		cs_out_mute(a, "after clear");
		t0 = 2;
	}
	int pll_ms = 0;	/* sp2: STATUS3 bit1 PLL_LOCK polled alongside PUP_DONE (lx/cs35l41.h:707) */
	uint32_t s = 0, s3 = 0;
	for (int i = t0; i < 100; i++) {
		cs_wait_us(1000);
		if (rd(a, IRQ1_STATUS1, &s) || rd(a, IRQ1_STATUS3, &s3))
			return -1;
		if ((s3 & IRQ3_PLL_LOCK) && !pll_ms)
			cs_say("cs35l41 %#x: PLL_LOCK first after %d ms\n", a->addr, pll_ms = i + 1);
		if (s & IRQ_ERRORS) {
			cs_say("cs35l41 %#x: IRQ1 error bits %08x at power-up\n", a->addr, s & IRQ_ERRORS);
			a->err_bits |= s & IRQ_ERRORS;
			return -1;
		}
		if (s & IRQ_PUP_DONE) {
			cs_say("cs35l41 %#x: PUP_DONE after %d ms (STATUS1 %08x STATUS3 %08x, PLL_LOCK %s)\n", a->addr, i + 1,
			       s, s3, pll_ms ? "seen" : "never");
			return wr(a, IRQ1_STATUS1, IRQ_PUP_DONE) ? -1 : 0;
		}
	}
	cs_say("cs35l41 %#x: no PUP_DONE in 100 ms (STATUS1 %08x STATUS3 %08x, PLL_LOCK %s)\n", a->addr, s, s3,
		       pll_ms ? "seen" : "never");
	return -1;
}

/* sp6: on-window snapshot, read-only. RAW bit names from Linux cs35l41.h (it names no LRCLK/BCLK/FS error bits) */
static void cs_diag(struct cs_amp *a, const char *tag)
{
	static const struct { const char *n; uint32_t r; } regs[] = {
		{ "raw1", 0x10090 }, { "raw2", 0x10094 }, { "raw3", 0x10098 }, { "raw4", 0x1009c },
		{ "clock_detect_1", 0x14000 }, { "ng_cfg", 0x6808 }, { "mixer_ngate_ch1", 0x12004 },
		{ "sp_enables", SP_ENABLES }, { "sp_format", SP_FORMAT }, { "sp_rx_wl", SP_RX_WL }, { "pwr_ctrl3", PWR_CTRL3 },
	};
	uint32_t v[sizeof(regs) / sizeof(regs[0])] = { 0 };
	for (unsigned i = 0; i < sizeof(regs) / sizeof(regs[0]); i++)
		if (!rd(a, regs[i].r, &v[i]))
			cs_say("cs35l41 %#x: diag %s %s %08x = %08x\n", a->addr, tag, regs[i].n, regs[i].r, v[i]);
	cs_say("cs35l41 %#x: diag %s decode:%s%s%s%s%s%s%s%s%s%s%s\n", a->addr, tag,
	       v[0] & IRQ_AMP_SHORT ? " AMP_SHORT" : "", v[0] & IRQ_TEMP_ERR ? " TEMP_ERR" : "",
	       v[0] & IRQ_TEMP_WARN ? " TEMP_WARN" : "", v[0] & IRQ_BST_SHORT ? " BST_SHORT" : "",
	       v[0] & IRQ_BST_UVP ? " BST_DCM_UVP" : "", v[0] & IRQ_BST_OVP ? " BST_OVP" : "",
	       v[0] & IRQ_PUP_DONE ? " PUP_DONE" : "", v[0] & IRQ_PDN_DONE ? " PDN_DONE" : "",
	       v[2] & IRQ3_PLL_LOCK ? " PLL_LOCK" : "", v[2] & 0x10 ? " PLL_UNLOCK" : "",
	       v[3] & 0x80000000u ? " OTP_BOOT_ERR" : v[3] & 0x2 ? " OTP_BOOT_DONE" : "");
}

/* sp25: the GLOBAL_EN half of the power-down; 0 = GLOBAL_EN 0 written and PDN_DONE seen */
static int global_off(struct cs_amp *a)
{
	int err = 0;
	if (a->out_mute)	/* sp8: mute before GLOBAL_EN 0, as ios-redfin 0953 */
		upd(a, AMP_OUT_MUTE, 0x10, 0x10);
	err |= upd(a, PWR_CTRL1, 1, 0);
	int i;
	for (i = 0; i < 100; i++) {
		uint32_t s;
		cs_wait_us(1000);
		if (rd(a, IRQ1_STATUS1, &s) || (s & IRQ_PDN_DONE))
			break;
	}
	if (i == 100) {
		cs_say("cs35l41 %#x: no PDN_DONE in 100 ms\n", a->addr);
		err = -1;
	}
	wr(a, IRQ1_STATUS1, 0xffffffffu);
	patch(a, 0x002f1aa3);
	a->up = 0;
	return err;
}

/* sp25: GLOBAL_EN off, configuration kept: the state cs_verify leaves, so cs_power_up can follow */
static int cs_standby(struct cs_amp *a)
{
	if (!AMP_OK(a->addr) || !a->configured || !a->up)
		return -1;
	return global_off(a);
}

/* sp25: before a power-up from standby: amp enable and the volume ceiling still as verified. A few ms */
static int cs_resume_check(struct cs_amp *a)
{
	for (int i = 0; i < a->nexp; i++) {
		uint32_t v;
		if (a->exp[i].reg != PWR_CTRL2 && a->exp[i].reg != AMP_DIG_VOL)
			continue;
		if (rd(a, a->exp[i].reg, &v))
			return -1;
		if ((v & a->exp[i].mask) != a->exp[i].val) {
			cs_say("cs35l41 %#x: standby lost %08x: read %08x want %08x\n", a->addr, a->exp[i].reg, v, a->exp[i].val);
			return -1;
		}
	}
	return 0;
}

static void cs_power_down(struct cs_amp *a)
{
	if (!AMP_OK(a->addr) || !a->configured)
		return;
	if (a->up)
		global_off(a);
	wr(a, PWR_CTRL2, 0);	/* AMP_EN 0, boost FET off */
	if (a->err_bits) {	/* error release (cs35l41.c:379); not re-enabled in this run */
		uint32_t rls = (a->err_bits & IRQ_AMP_SHORT ? 0x02 : 0) | (a->err_bits & IRQ_BST_SHORT ? 0x04 : 0) |
			       (a->err_bits & IRQ_BST_OVP ? 0x08 : 0) | (a->err_bits & IRQ_BST_UVP ? 0x10 : 0) |
			       (a->err_bits & IRQ_TEMP_WARN ? 0x20 : 0) | (a->err_bits & IRQ_TEMP_ERR ? 0x40 : 0);
		wr(a, PROTECT_REL, 0);
		wr(a, PROTECT_REL, rls);
		wr(a, PROTECT_REL, 0);
	}
	wr(a, AMP_GAIN_CTRL, a->gain_saved);	/* reset value back (amp off) */
	a->configured = a->verified = 0;
	cs_say("cs35l41 %#x: powered down\n", a->addr);
}


/* ---- for the DSP's driver (hakios.adsp_sm7150.c): one amplifier, brought up while the port's clock runs ---- */

static struct pmic_gpio amp_gpios[2] = { { .name = "PM6150L GPIO 8", .peripheral = 0xc7 }, { .name = "PM6150L GPIO 11", .peripheral = 0xca } };
static struct cs_amp the_amp;
static struct pmic_gpio *the_gpio;
static bool amp_started_once;

static bool
arbiter_map(void)
{
	if (sp_core == 0) {
		sp_core = ml_io_map(SP_CORE_PA, 0x2000);
		sp_cfg = ml_io_map(SP_CFG_PA, 0x1000);
		sp_obs = ml_io_map(SP_OBS_PA, 0x10000);
	}
	return sp_core != 0 && sp_cfg != 0 && sp_obs != 0 && mmio(sp_core, 0) >= 0x50000000u && mmio(sp_core, 0) < 0x70000000u;
}

/* The amplifier at `address' (0x40 top/earpiece, 0x41 bottom speaker) out of reset, identified, configured at vol_db
 * (at most -20), read back. The port's bit clock must be running already. False: it is back in reset. */
bool hakios_amp_prepare(uint8_t address, int vol_db);
bool hakios_amp_start(void);
void hakios_amp_stop(void);

void
hakios_amp_stop(void)
{
	if (the_gpio == NULL) {
		return;
	}
	(void)cs_check(&the_amp);
	cs_power_down(&the_amp);
	for (int r = 5; r >= 0; r--) {
		(void)sp_write(the_gpio, gpio_regs[r], the_gpio->before[r]);
	}
	say("amp: %#x powered down, %s put back as it was (in reset)\n", the_amp.addr, the_gpio->name);
	the_gpio = NULL;
}

bool
hakios_amp_prepare(uint8_t address, int vol_db)
{
	struct pmic_gpio *g = &amp_gpios[address == 0x41 ? 1 : 0];

	if (!AMP_OK(address) || the_gpio != NULL || !arbiter_map() || !gpio_look(g) || !g->ours) {
		say("amp: %#x cannot be prepared (its reset line is not ours, or an amplifier is in use)\n", address);
		return false;
	}
	if (!(sp_write(g, 0x40, 0x01) && sp_write(g, 0x44, 0x80) && sp_write(g, 0x46, 0x80))) {
		say("amp: %s could not be released\n", g->name);
		return false;
	}
	the_gpio = g;
	delay_for_interval(5, 1000 * 1000);
	memset(&the_amp, 0, sizeof(the_amp));
	the_amp.addr = address;
	the_amp.out_mute = 1;
	the_amp.clk = 1;
	if (cs_probe(&the_amp) != 0 || cs_config(&the_amp, vol_db, 0) != 0 || cs_verify(&the_amp) != 0) {
		say("amp: %#x did not take its configuration: powered down, back into reset\n", address);
		hakios_amp_stop();
		return false;
	}
	return true;
}

/* Between sounds: GLOBAL_EN off, the configuration kept (a few milliseconds each way, where a full set-up is a second or two). */
bool hakios_amp_pause(void);
bool
hakios_amp_pause(void)
{
	return the_gpio != NULL && cs_standby(&the_amp) == 0;
}

/* Still without an error bit? (false: the caller stops it) */
bool hakios_amp_healthy(void);
bool
hakios_amp_healthy(void)
{
	return the_gpio != NULL && cs_check(&the_amp) == 0;
}

/* GLOBAL_EN: true when the amplifier says its power-up is done (whether its PLL locked to the bit clock is said). */
bool
hakios_amp_start(void)
{
	bool up;

	if (the_gpio == NULL) {
		return false;
	}
	if (cs_resume_check(&the_amp) != 0) {
		return false;
	}
	up = cs_power_up(&the_amp) == 0;
	if (!up || !amp_started_once) {
		cs_diag(&the_amp, up ? "up" : "not up");        /* the long look only the first time, and when it fails */
	}
	amp_started_once = true;
	return up;
}

static void
amp_thread(__unused void *parameter, __unused wait_result_t result)
{
	struct pmic_gpio *gpios = amp_gpios;
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
	if (sp_core == 0 || sp_cfg == 0 || sp_obs == 0 || mmio(sp_core, 0) < 0x50000000u || mmio(sp_core, 0) >= 0x70000000u) {
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
