/*
 * The Pixel 4a (sunfish) cameras: milestone 1, the probe. The HakiOS camera stack (drivers/camera/hakios.camera.c,
 * userland camera_device.c + camera_isp.c) is written for the POCO X5 Pro; this is the SM7150/Pixel-4a hardware, from
 * the phone's Android device tree (google/sm7150-sunfish-camera-*.dtsi) read as data (see
 * ios-sunfish research/camera/sunfish-camera-hw.md). Nothing here streams yet: it brings the power up step by step and
 * reads a chip's ID, so the chain is proven before the ISP is wired.
 *
 *   cam=1   the camera PMIC (SLG51000) read: is it powered and on its I2C bus?
 *   cam=2   ... and the rear sensor's full power-up + IMX363 chip ID (0x0363) over CCI0
 *
 * Buses are bit-banged on the TLMM pins Android muxes to the hardware I2C (the pins taken as GPIO for each transaction
 * and left as they were), the way drivers/camera/hakios.camera.c does it. Writes to no PMIC's SPMI here except the one
 * GPIO that enables the SLG51000 (PM6150L GPIO4); nothing else of the machine is touched.
 */
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <pexpert/pexpert.h>
#include <sys/systm.h>

/* ---- inlined imx363_regs.h (build copies only the .c into bsd/dev) ---- */
/* Generated from sm7150-mainline drivers/media/i2c/imx363.c (binned 2016x1512, 642 Mbps/lane, 24 MHz, 4-lane). */
struct imx_reg { uint16_t addr; uint32_t val; uint8_t size; };
static const struct imx_reg imx363_pll_642[] = {
	{ 0x0136, 0x1800, 2 },
	{ 0x0301, 0x5, 1 },
	{ 0x0303, 0x2, 1 },
	{ 0x0305, 0x4, 1 },
	{ 0x0306, 0x6b, 2 },
	{ 0x0309, 0xa, 1 },
	{ 0x030b, 0x1, 1 },
	{ 0x030d, 0x2, 1 },
	{ 0x030e, 0xd8, 2 },
	{ 0x0310, 0x0, 1 },
	{ 0x0114, 0x3, 1 },
	{ 0x0820, 0xa08, 2 },
	{ 0x0822, 0x0, 2 },
};
static const struct imx_reg imx363_common[] = {
	{ 0x31a3, 0x0, 1 },
	{ 0x64d4, 0x1, 1 },
	{ 0x64d5, 0xaa, 1 },
	{ 0x64d6, 0x1, 1 },
	{ 0x64d7, 0xa9, 1 },
	{ 0x64d8, 0x1, 1 },
	{ 0x64d9, 0xa5, 1 },
	{ 0x64da, 0x1, 1 },
	{ 0x64db, 0xa1, 1 },
	{ 0x720a, 0x24, 1 },
	{ 0x720b, 0x89, 1 },
	{ 0x720c, 0x85, 1 },
	{ 0x720d, 0xa1, 1 },
	{ 0x720e, 0x6e, 1 },
	{ 0x729c, 0x59, 1 },
	{ 0x817c, 0xff, 1 },
	{ 0x817d, 0x80, 1 },
	{ 0x9348, 0x96, 1 },
	{ 0x934b, 0x8c, 1 },
	{ 0x934c, 0x82, 1 },
	{ 0x9353, 0xaa, 1 },
	{ 0x9354, 0xaa, 1 },
	{ 0x30f4, 0x2, 1 },
	{ 0x30f5, 0x80, 1 },
	{ 0x31a5, 0x0, 1 },
	{ 0x31a6, 0x0, 1 },
	{ 0x560f, 0xbe, 1 },
	{ 0x5856, 0x8, 1 },
	{ 0x58d0, 0x10, 1 },
	{ 0x734a, 0x1, 1 },
	{ 0x734f, 0x2b, 1 },
	{ 0x7441, 0x55, 1 },
	{ 0x7914, 0x3, 1 },
	{ 0x7928, 0x4, 1 },
	{ 0x7929, 0x4, 1 },
	{ 0x793f, 0x3, 1 },
	{ 0x3038, 0x0, 1 },
	{ 0x303a, 0x10, 2 },
	{ 0x300d, 0x1, 1 },
	{ 0x0350, 0x0, 1 },
	{ 0x0204, 0x0, 2 },
	{ 0x020e, 0x100, 2 },
	{ 0x0210, 0x100, 2 },
	{ 0x0212, 0x100, 2 },
	{ 0x0214, 0x100, 2 },
	{ 0x7bcd, 0x0, 1 },
	{ 0x3030, 0x0, 1 },
	{ 0x3032, 0x0, 1 },
	{ 0x0220, 0x0, 1 },
	{ 0x0112, 0xa0a, 2 },
	{ 0x0342, 0x0, 2 },
	{ 0x0136, 0x18, 1 },
	{ 0x0137, 0x0, 1 },
	{ 0x31a3, 0x0, 1 },
	{ 0x64d4, 0x1, 1 },
	{ 0x64d5, 0xaa, 1 },
	{ 0x64d6, 0x1, 1 },
	{ 0x64d7, 0xa9, 1 },
	{ 0x64d8, 0x1, 1 },
	{ 0x64d9, 0xa5, 1 },
	{ 0x64da, 0x1, 1 },
	{ 0x64db, 0xa1, 1 },
	{ 0x720a, 0x24, 1 },
	{ 0x720b, 0x89, 1 },
	{ 0x720c, 0x85, 1 },
	{ 0x720d, 0xa1, 1 },
	{ 0x720e, 0x6e, 1 },
	{ 0x729c, 0x59, 1 },
	{ 0x817c, 0xff, 1 },
	{ 0x817d, 0x80, 1 },
	{ 0x9348, 0x96, 1 },
	{ 0x934b, 0x8c, 1 },
	{ 0x934c, 0x82, 1 },
	{ 0x9353, 0xaa, 1 },
	{ 0x9354, 0xaa, 1 },
	{ 0x4073, 0x30, 1 },
};
static const struct imx_reg imx363_2016x1512[] = {
	{ 0x0900, 0x0, 1 },
	{ 0x0901, 0x11, 1 },
	{ 0x0401, 0x1, 1 },
	{ 0x0404, 0x40, 2 },
	{ 0x0344, 0x0, 2 },
	{ 0x0346, 0x0, 2 },
	{ 0x0348, 0xfbf, 2 },
	{ 0x034a, 0xbcf, 2 },
	{ 0x0381, 0x1, 1 },
	{ 0x0383, 0x1, 1 },
	{ 0x0385, 0x1, 1 },
	{ 0x0387, 0x1, 1 },
	{ 0x0408, 0x0, 2 },
	{ 0x040a, 0x0, 2 },
	{ 0x040c, 0xfc0, 2 },
	{ 0x040e, 0xbd0, 2 },
	{ 0x034c, 0xfc0, 2 },
	{ 0x034e, 0xbd0, 2 },
	{ 0x0340, 0x0, 2 },
	{ 0x0112, 0xa, 1 },
	{ 0x0113, 0xa, 1 },
	{ 0x0114, 0x3, 1 },
	{ 0x0220, 0x0, 1 },
	{ 0x0221, 0x11, 1 },
	{ 0x0340, 0xc, 1 },
	{ 0x0341, 0x62, 1 },
	{ 0x0342, 0x22, 1 },
	{ 0x0343, 0x80, 1 },
	{ 0x0381, 0x1, 1 },
	{ 0x0383, 0x1, 1 },
	{ 0x0385, 0x1, 1 },
	{ 0x0387, 0x1, 1 },
	{ 0x0900, 0x1, 1 },
	{ 0x0901, 0x22, 1 },
	{ 0x30f4, 0x2, 1 },
	{ 0x30f5, 0x58, 1 },
	{ 0x30f6, 0x0, 1 },
	{ 0x30f7, 0xc8, 1 },
	{ 0x31a0, 0x0, 1 },
	{ 0x31a5, 0x0, 1 },
	{ 0x31a6, 0x0, 1 },
	{ 0x560f, 0xff, 1 },
	{ 0x5856, 0x8, 1 },
	{ 0x58d0, 0x10, 1 },
	{ 0x734a, 0x1, 1 },
	{ 0x734f, 0x2b, 1 },
	{ 0x7441, 0x55, 1 },
	{ 0x7914, 0x3, 1 },
	{ 0x7928, 0x4, 1 },
	{ 0x7929, 0x4, 1 },
	{ 0x793f, 0x3, 1 },
	{ 0xbc7b, 0x18, 1 },
	{ 0x0344, 0x0, 1 },
	{ 0x0345, 0x0, 1 },
	{ 0x0346, 0x0, 1 },
	{ 0x0347, 0x0, 1 },
	{ 0x0348, 0xf, 1 },
	{ 0x0349, 0xbf, 1 },
	{ 0x034a, 0xb, 1 },
	{ 0x034b, 0xcf, 1 },
	{ 0x034c, 0x7, 1 },
	{ 0x034d, 0xe0, 1 },
	{ 0x034e, 0x5, 1 },
	{ 0x034f, 0xe8, 1 },
	{ 0x0408, 0x0, 1 },
	{ 0x0409, 0x0, 1 },
	{ 0x040a, 0x0, 1 },
	{ 0x040b, 0x0, 1 },
	{ 0x040c, 0x7, 1 },
	{ 0x040d, 0xe0, 1 },
	{ 0x040e, 0x5, 1 },
	{ 0x040f, 0xe8, 1 },
	{ 0x0301, 0x3, 1 },
	{ 0x0303, 0x2, 1 },
	{ 0x0305, 0x4, 1 },
	{ 0x0306, 0x0, 1 },
	{ 0x0307, 0xd2, 1 },
	{ 0x0309, 0xa, 1 },
	{ 0x030b, 0x1, 1 },
	{ 0x030d, 0x4, 1 },
	{ 0x030e, 0x0, 1 },
	{ 0x030f, 0xdf, 1 },
	{ 0x0310, 0x1, 1 },
	{ 0x0202, 0xc, 1 },
	{ 0x0203, 0x52, 1 },
	{ 0x0224, 0x6, 1 },
	{ 0x0225, 0x29, 1 },
	{ 0x0204, 0x0, 1 },
	{ 0x0205, 0x0, 1 },
	{ 0x0216, 0x0, 1 },
	{ 0x0217, 0x0, 1 },
	{ 0x020e, 0x1, 1 },
	{ 0x020f, 0x0, 1 },
	{ 0x0226, 0x1, 1 },
	{ 0x0227, 0x0, 1 },
};


extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern void delay(int usec);
extern void loader_log_putc(char c);

static void camlog(const char *msg) { printf("%s", msg); for (const char *c = msg; *c; c++) loader_log_putc(*c); }

/* TLMM: base 0x03400000, tiles NORTH +0x500000, SOUTH +0x900000 (pinctrl-sdmmagpie.c, as the other sunfish drivers map them). */
#define TLMM_NORTH              0x03900000UL
#define TLMM_SOUTH              0x03d00000UL
#define TLMM_TILE_SIZE          0x300000UL
#define TLMM_CFG                0x0u            /* [1:0] pull, [5:2] function, [8:6] drive, [9] output enable */
#define TLMM_IO                 0x4u            /* [0] in, [1] out */

/* The camera PMIC (Dialog SLG51000) on the SE9 I2C pins (GPIO 46 SDA, 47 SCL, NORTH), 7-bit address 0x75,
 * 16-bit register, 8-bit data. Registers (Linux slg51000-regulator.h). */
#define SLG_SDA_GPIO            46
#define SLG_SCL_GPIO            47
#define SLG_ADDR                0x75
#define SLG_PATTERN_ID_B0       0x1105
#define SLG_LDO_VSEL(n)         ((n) == 1 ? 0x2000 : (n) == 2 ? 0x2200 : (n) == 3 ? 0x2300 : (n) == 4 ? 0x2500 : \
                                 (n) == 5 ? 0x2700 : (n) == 6 ? 0x2900 : 0x3100)       /* LDO1..7 VSEL */
#define SLG_MATRIX_CONF_A       0x110d          /* enable: BIT(ldo - 1) */

static uint32_t cam_level;
static uint32_t cam_mclk_khz = 24000;   /* effective MCLK0 rate (24000 via PLL2, or 19200 XO fallback) */
static struct { unsigned gpio; vm_offset_t va; } pin_map[8];
static unsigned pin_mapped;

/* one 16 KB page per GPIO (a 3 MB tile map can fail on kmem_alloc; see the clocks driver) */
static volatile uint32_t *
pin(unsigned gpio)
{
    vm_offset_t tile = (gpio == SLG_SDA_GPIO || gpio == SLG_SCL_GPIO || gpio == 43) ? TLMM_NORTH : TLMM_SOUTH;
    vm_offset_t page = tile + 0x1000UL * gpio, base = page & ~0x3fffUL;

    for (unsigned i = 0; i < pin_mapped; i++) {
        if (pin_map[i].gpio == gpio) {
            return (volatile uint32_t *)(pin_map[i].va + (page - base));
        }
    }
    if (pin_mapped < 8) {
        vm_offset_t va = ml_io_map(base, 0x4000);
        if (va != 0) {
            pin_map[pin_mapped].gpio = gpio;
            pin_map[pin_mapped].va = va;
            return (volatile uint32_t *)(pin_map[pin_mapped++].va + (page - base));
        }
    }
    return NULL;
}

/* An open-drain I2C line: driven low by making it an output-0, released by making it an input (the pull holds it high). */
static void
sda_scl_init(unsigned gpio)
{
    volatile uint32_t *p = pin(gpio);

    if (p == NULL) return;
    p[TLMM_CFG / 4] = 0;                         /* function 0 (GPIO), pull disabled, input */
    __asm__ volatile ("dsb sy" ::: "memory");
}

static void
line_low(unsigned gpio)
{
    volatile uint32_t *p = pin(gpio);

    if (p == NULL) return;
    p[TLMM_IO / 4] = 0;
    p[TLMM_CFG / 4] = 1u << 9;                   /* output enable */
    __asm__ volatile ("dsb sy" ::: "memory");
    delay(3);
}

static void
line_release(unsigned gpio)
{
    volatile uint32_t *p = pin(gpio);

    if (p == NULL) return;
    p[TLMM_CFG / 4] = 0;                         /* input: the pull takes it high */
    __asm__ volatile ("dsb sy" ::: "memory");
    delay(3);
}

static int
line_read(unsigned gpio)
{
    volatile uint32_t *p = pin(gpio);
    return p ? (int)(p[TLMM_IO / 4] & 1u) : 1;
}

/* One I2C bus, two pins. Standard bit-bang. */
struct i2c { unsigned sda, scl; };

static void i2c_start(struct i2c *b) { line_release(b->sda); line_release(b->scl); line_low(b->sda); line_low(b->scl); }
static void i2c_stop(struct i2c *b) { line_low(b->sda); line_release(b->scl); line_release(b->sda); }

static int
i2c_write_byte(struct i2c *b, uint8_t v)
{
    int ack;

    for (int i = 7; i >= 0; i--) {
        if (v & (1u << i)) {
            line_release(b->sda);
        } else {
            line_low(b->sda);
        }
        line_release(b->scl);
        line_low(b->scl);
    }
    line_release(b->sda);                        /* release for the ACK */
    line_release(b->scl);
    ack = line_read(b->sda) == 0;                /* the slave pulls it low to ACK */
    line_low(b->scl);
    return ack;
}

static uint8_t
i2c_read_byte(struct i2c *b, int ack)
{
    uint8_t v = 0;

    line_release(b->sda);
    for (int i = 7; i >= 0; i--) {
        line_release(b->scl);
        v = (uint8_t)(v << 1) | (uint8_t)line_read(b->sda);
        line_low(b->scl);
    }
    if (ack) {
        line_low(b->sda);
    } else {
        line_release(b->sda);
    }
    line_release(b->scl);
    line_low(b->scl);
    line_release(b->sda);
    return v;
}

/* read `count' bytes from a 16-bit register of a 7-bit-addressed device; 0 ok, -1 no ACK */
/* bare address probe: START, write-address, note ACK, STOP. 1 = device present. */
static int
i2c_ping(struct i2c *b, uint8_t addr)
{
    int ack;

    i2c_start(b);
    ack = i2c_write_byte(b, (uint8_t)(addr << 1));
    i2c_stop(b);
    return ack;
}

static int
i2c_read_reg16(struct i2c *b, uint8_t addr, uint16_t reg, uint8_t *out, unsigned count)
{
    i2c_start(b);
    if (!i2c_write_byte(b, (uint8_t)(addr << 1)) || !i2c_write_byte(b, (uint8_t)(reg >> 8)) ||
        !i2c_write_byte(b, (uint8_t)reg)) {
        i2c_stop(b);
        return -1;
    }
    i2c_start(b);                                /* repeated start for the read */
    if (!i2c_write_byte(b, (uint8_t)(addr << 1 | 1))) {
        i2c_stop(b);
        return -1;
    }
    for (unsigned i = 0; i < count; i++) {
        out[i] = i2c_read_byte(b, i + 1 < count);
    }
    i2c_stop(b);
    return 0;
}

static int
i2c_write_reg16(struct i2c *b, uint8_t addr, uint16_t reg, uint8_t value)
{
    int ok;

    i2c_start(b);
    ok = i2c_write_byte(b, (uint8_t)(addr << 1)) && i2c_write_byte(b, (uint8_t)(reg >> 8)) &&
         i2c_write_byte(b, (uint8_t)reg) && i2c_write_byte(b, value);
    i2c_stop(b);
    return ok ? 0 : -1;
}


/* ---- the SPMI arbiter (v5), to drive the PM6150L GPIOs that enable the SLG51000 (as the torch driver does) ---- */
#define ARB_CORE_PHYS       0x0c440000UL
#define ARB_CNFG_PHYS       0x0c40a000UL
#define ARB_CHNLS_PHYS      0x0c600000UL
#define ARB_CHANNEL(n)      (0x900 + 4 * (n))   /* [19:8] slave<<8|peripheral */
#define ARB_OWNER(n)        (0x700 + 4 * (n))   /* [2:0] owner; 0 = us */
#define ARB_FEATURES        0x004
#define CH_COMMAND          0x00
#define CH_STATUS           0x08
#define CH_WDATA0           0x10
#define CMD_WRITE_LONG      (0u << 27)          /* [11:4] reg, [2:0] bytes-1 */
extern void hakios_spmi_lock(void);
extern void hakios_spmi_unlock(void);
static vm_offset_t arb_core, arb_cnfg;

/* write one byte to (slave, peripheral<<8|reg) of a PMIC through the arbiter; 0 ok */
static int
spmi_write8(unsigned slave, unsigned periph, unsigned reg, uint8_t value)
{
    uint32_t id = (slave << 8) | periph, channels, n, found = ~0u;
    vm_offset_t ch;
    int ok = 0;

    if (arb_core == 0) {
        arb_core = ml_io_map(ARB_CORE_PHYS, 0x1100);
        arb_cnfg = ml_io_map(ARB_CNFG_PHYS, 0x1000);
    }
    if (arb_core == 0 || arb_cnfg == 0) {
        return -1;
    }
    channels = *(volatile uint32_t *)(arb_core + ARB_FEATURES) & 0x7ff;
    for (n = 0; n < channels; n++) {
        uint32_t map = *(volatile uint32_t *)(arb_core + ARB_CHANNEL(n));

        if (((map >> 8) & 0xfff) == id) {
            if ((*(volatile uint32_t *)(arb_cnfg + ARB_OWNER(n)) & 7) == 0) {
                found = n;
                break;
            }
            if (found == ~0u) {
                found = n;
            }
        }
    }
    if (found == ~0u) {
        return -2;
    }
    ch = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * found, 0x1000);
    if (ch == 0) {
        return -1;
    }
    hakios_spmi_lock();
    *(volatile uint32_t *)(ch + CH_WDATA0) = value;
    *(volatile uint32_t *)(ch + CH_COMMAND) = CMD_WRITE_LONG | (uint32_t)reg << 4;   /* 1 byte */
    __asm__ volatile ("dsb sy" ::: "memory");
    for (int t = 0; t < 50; t++) {
        uint32_t st = *(volatile uint32_t *)(ch + CH_STATUS);
        if (st & 1u) { ok = 1; break; }         /* DONE */
        if (st & 0xeu) { break; }               /* FAILURE/DENIED/DROPPED */
        delay(40);
    }
    hakios_spmi_unlock();
    return ok ? 0 : -3;
}

#define CH_RDATA0           0x18
#define CMD_READ_LONG       (1u << 27)          /* EXT_READL opcode */

/* read one byte from (slave, peripheral<<8|reg); returns 0..255, or negative on error */
static int
spmi_read8(unsigned slave, unsigned periph, unsigned reg)
{
    uint32_t id = (slave << 8) | periph, channels, n, found = ~0u;
    vm_offset_t ch;
    int val = -3;

    if (arb_core == 0) {
        arb_core = ml_io_map(ARB_CORE_PHYS, 0x1100);
        arb_cnfg = ml_io_map(ARB_CNFG_PHYS, 0x1000);
    }
    if (arb_core == 0 || arb_cnfg == 0) {
        return -1;
    }
    channels = *(volatile uint32_t *)(arb_core + ARB_FEATURES) & 0x7ff;
    for (n = 0; n < channels; n++) {
        uint32_t map = *(volatile uint32_t *)(arb_core + ARB_CHANNEL(n));
        if (((map >> 8) & 0xfff) == id) {
            if ((*(volatile uint32_t *)(arb_cnfg + ARB_OWNER(n)) & 7) == 0) { found = n; break; }
            if (found == ~0u) { found = n; }
        }
    }
    if (found == ~0u) {
        return -2;
    }
    ch = ml_io_map(ARB_CHNLS_PHYS + 0x10000UL * found, 0x1000);
    if (ch == 0) {
        return -1;
    }
    hakios_spmi_lock();
    *(volatile uint32_t *)(ch + CH_COMMAND) = CMD_READ_LONG | (uint32_t)reg << 4;   /* 1 byte */
    __asm__ volatile ("dsb sy" ::: "memory");
    for (int t = 0; t < 50; t++) {
        uint32_t st = *(volatile uint32_t *)(ch + CH_STATUS);
        if (st & 1u) { val = *(volatile uint32_t *)(ch + CH_RDATA0) & 0xff; break; }
        if (st & 0xeu) { break; }
        delay(40);
    }
    hakios_spmi_unlock();
    return val;
}

/* a PM6150L GPIO (slave 4, GPIOn peripheral 0xc000 + (n-1)*0x100) to constant output-high, LV/MV, push-pull, no pull
 * (the bytes pinctrl-spmi-gpio.c writes: MODE_CTL 0x40=1 output, OUT_SOURCE 0x44=0x80 value-high, DIG_OUT 0x45 push-pull,
 * PULL 0x42=5 none, VIN 0x41=0, EN_CTL 0x46=0x80). */
static void
pm6150l_gpio_high(unsigned gpio)
{
    unsigned periph = 0xc0 + (gpio - 1);        /* 0xc000 + (n-1)*0x100 -> peripheral byte */
    int a, b, c, d, e, f;

    b = spmi_write8(4, periph, 0x41, 0x00);     /* DIG_VIN_CTL: VIN0 */
    c = spmi_write8(4, periph, 0x42, 0x05);     /* DIG_PULL_CTL: no pull */
    a = spmi_write8(4, periph, 0x44, 0x80);     /* LV_MV_DIG_OUT_SOURCE_CTL: output value high */
    d = spmi_write8(4, periph, 0x45, 0x01);     /* DIG_OUT_CTL: push-pull (CMOS), low drive */
    e = spmi_write8(4, periph, 0x40, 0x01);     /* MODE_CTL: digital output */
    f = spmi_write8(4, periph, 0x46, 0x80);     /* EN_CTL: master enable */
    {
        char line[96];
        snprintf(line, sizeof(line), "camera: PM6150L GPIO%u high: writes %d %d %d %d %d %d (0 ok)\n", gpio, b, c, a, d, e, f);
        camlog(line);
    }
}

/* drive the same GPIO's output value low (keeps it a configured push-pull output) - for a clean low->high toggle */
static void
pm6150l_gpio_low(unsigned gpio)
{
    (void)spmi_write8(4, 0xc0 + (gpio - 1), 0x44, 0x00);        /* OUT_SOURCE: value low */
}


/* ---- camcc (0xad00000), the titan power domain, and MCLK0, to clock the sensor ---- */
#define CAMCC_PHYS          0x0ad00000UL
#define TITAN_GDSCR_PHYS    0x0ad0c1c4UL
#define MCLK0_CMD_RCGR      0x5004
#define MCLK0_CFG_RCGR      0x5008
#define MCLK0_CBCR          0x501c
#define RCG_UPDATE          1u
#define MCLK0_GPIO          13
#define CAM_RESET_GPIO      43
#define CCI0_SDA_GPIO       17
#define CCI0_SCL_GPIO       18
#define IMX_ADDR            0x1a            /* IMX363 7-bit slave */
#define IMX_CHIP_ID_REG     0x0016
static vm_offset_t camcc, titan;

/* set an SLG51000 LDO to vsel (voltage code), then enable the chosen LDOs (mask of BIT(ldo-1) in reg 0x110d). */
static void
slg_ldo(struct i2c *b, unsigned ldo, uint8_t vsel)
{
    (void)i2c_write_reg16(b, SLG_ADDR, SLG_LDO_VSEL(ldo), vsel);
}

#define MCLK0_M             0x500c
#define MCLK0_N             0x5010
#define MCLK0_D             0x5014
#define RD(a)               (*(volatile uint32_t *)(a))
#define WR(a,v)             (*(volatile uint32_t *)(a) = (v))
/* camcc PLL2 (FABIA, offset 0x2000) to 1920 MHz; OUT_AUX (USER_CTL postdiv /4) = 480 MHz. sm7150 camcc-sm7150.c values. */
#define PLL2                0x2000
#define PLL2_MODE           0x00            /* [0] OUTCTRL, [2] RESET_N, [31] lock */
#define PLL2_L_VAL          0x04
#define PLL2_USER_CTL       0x0c
#define PLL2_USER_CTL_U     0x10
#define PLL2_CONFIG_CTL     0x14
#define PLL2_CONFIG_CTL_U   0x18
#define PLL2_STATUS         0x24
#define PLL2_OPMODE         0x2c
#define PLL2_FRAC           0x38

static void
camcc_pll2_up(void)
{
    vm_offset_t b = camcc + PLL2;

    if (RD(b + PLL2_MODE) & 1u) {               /* already running (OUTCTRL) */
        return;
    }
    /* clk_fabia_pll_configure: write_config skips zero values, so only the nonzero
     * regs (L, CONFIG_CTL, CONFIG_CTL_U, USER_CTL) are written - NOT FRAC/USER_CTL_U. */
    WR(b + PLL2_L_VAL, 0x64);
    WR(b + PLL2_CONFIG_CTL, 0x20000954);
    WR(b + PLL2_CONFIG_CTL_U, 0x400003d6);
    WR(b + PLL2_USER_CTL, 0x0000030b);          /* bits[9:8]=3 -> aux postdiv /4 */
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 0x800000u); /* PLL_UPDATE_BYPASS (BIT23): required by configure */
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 4u);        /* RESET_N (set in configure) */
    __asm__ volatile ("dsb sy" ::: "memory");
    /* clk_alpha_pll_enable: agera (camcc_pll2) uses the CLASSIC sequence, not fabia OPMODE -
     * BYPASSNL, >=5 us, RESET_N, wait LOCK_DET, then OUTCTRL. Outputs are already enabled by USER_CTL=0x30b. */
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 2u);   /* BYPASSNL */
    __asm__ volatile ("dsb sy" ::: "memory");
    delay(10);                                    /* udelay(5) */
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 4u);   /* RESET_N */
    __asm__ volatile ("dsb sy" ::: "memory");
    {
        int locked = 0;
        char line[96];
        for (int i = 0; i < 500; i++) {
            if (RD(b + PLL2_MODE) & 0x80000000u) { locked = 1; break; }
            delay(10);
        }
        WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 1u);             /* OUTCTRL */
        __asm__ volatile ("dsb sy" ::: "memory");
        snprintf(line, sizeof(line), "camera: PLL2 %s (mode %#x)\n", locked ? "LOCKED" : "did NOT lock", RD(b + PLL2_MODE));
        camlog(line);
    }
}

/* camcc PLL0 (FABIA, offset 0x0) -> 1200 MHz; OUT_EVEN = 600 MHz, source for the CSIPHY/phytimer 300 MHz.
 * PLL0 uses clk_alpha_pll_fabia_ops: the FABIA OPMODE enable (STANDBY/RUN), unlike PLL2 (agera/classic). */
#define PLL0                0x0
#define PLL_TEST_CTL        0x1c
static void
camcc_pll0_up(void)
{
    vm_offset_t b = camcc + PLL0;

    if (RD(b + PLL2_MODE) & 1u) {               /* already running (OUTCTRL) */
        return;
    }
    /* configure (write_config skips zeros): L, FRAC(alpha), TEST_CTL, USER_CTL postdiv */
    WR(b + PLL2_L_VAL, 0x3e);
    WR(b + PLL2_FRAC, 0x8000);                  /* alpha (fractional 0.5) */
    WR(b + PLL_TEST_CTL, 0x40000000);
    WR(b + PLL2_USER_CTL, (RD(b + PLL2_USER_CTL) & ~0xff00u) | 0x3100u);  /* post_div bits[15:8]=0x31 */
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 0x800000u);   /* UPDATE_BYPASS */
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 4u);          /* RESET_N (configure) */
    __asm__ volatile ("dsb sy" ::: "memory");
    /* fabia enable: OUTCTRL off, OPMODE STANDBY, RESET_N, OPMODE RUN, wait lock, OUT_MASK, OUTCTRL */
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) & ~1u);
    WR(b + PLL2_OPMODE, 0);
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 4u);
    __asm__ volatile ("dsb sy" ::: "memory");
    delay(10);
    WR(b + PLL2_OPMODE, 1);
    __asm__ volatile ("dsb sy" ::: "memory");
    {
        int locked = 0;
        char line[96];
        for (int i = 0; i < 500; i++) {
            if (RD(b + PLL2_MODE) & 0x80000000u) { locked = 1; break; }
            delay(10);
        }
        WR(b + PLL2_USER_CTL, RD(b + PLL2_USER_CTL) | 0x7u);   /* PLL_OUT_MASK (fabia enables outputs) */
        WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 1u);             /* OUTCTRL */
        __asm__ volatile ("dsb sy" ::: "memory");
        snprintf(line, sizeof(line), "camera: PLL0 %s (mode %#x)\n", locked ? "LOCKED" : "did NOT lock", RD(b + PLL2_MODE));
        camlog(line);
    }
}

/* program a camcc root clock (RCG): write CFG (src_sel<<8 | src_div field), pulse UPDATE */
static void
camcc_rcg_set(uint32_t cmd_off, uint32_t src_sel, uint32_t div_field)
{
    WR(camcc + cmd_off + 4, (RD(camcc + cmd_off + 4) & ~0x37ffu) | (src_sel << 8) | div_field);
    WR(camcc + cmd_off, RD(camcc + cmd_off) | RCG_UPDATE);
    __asm__ volatile ("dsb sy" ::: "memory");
    for (int i = 0; i < 100 && (RD(camcc + cmd_off) & RCG_UPDATE); i++) { delay(100); }
}

/* CSIPHY0 data clocks: cphy_rx (300 MHz) and csi0phytimer (300 MHz), both PLL0_OUT_EVEN/2.
 * titan_top GDSC is already on (camcc_mclk0_on). */
static void
camss_csiphy_clocks_on(void)
{
    camcc_pll0_up();
    camcc_rcg_set(0x6004, 2, 3);        /* csi0phytimer_src: src 2 (OUT_EVEN), /2 (field 2*2-1=3) */
    camcc_rcg_set(0xa064, 2, 3);        /* cphy_rx_src: src 2 (OUT_EVEN), /2 */
    WR(camcc + 0x601c, RD(camcc + 0x601c) | 1u);   /* csi0phytimer_clk branch */
    WR(camcc + 0x6020, RD(camcc + 0x6020) | 1u);   /* csiphy0_clk branch */
    __asm__ volatile ("dsb sy" ::: "memory");
    delay(1000);
    {
        char line[128];
        snprintf(line, sizeof(line), "camera: CSIPHY clocks - phytimer CBCR %#x, csiphy0 CBCR %#x, cphy_rx cfg %#x (bit31=off)\n",
            RD(camcc + 0x601c), RD(camcc + 0x6020), RD(camcc + 0xa068));
        camlog(line);
    }
}

static void
camcc_mclk0_on(void)
{
    volatile uint32_t *g;

    /* titan top power domain on (clear SW_COLLAPSE bit0, wait bit31 powered) */
    if (titan != 0) {
        volatile uint32_t *t = (volatile uint32_t *)titan;
        *t &= ~1u;
        __asm__ volatile ("dsb sy" ::: "memory");
        for (int i = 0; i < 100 && !(*t & 0x80000000u); i++) { delay(100); }
    }
    if (camcc == 0) {
        return;
    }
    camcc_pll2_up();            /* bring PLL2 up (locked); used for 24 MHz only when mclk24=1 */
    {
        uint32_t want24 = 0;
        int on24 = 0;
        (void)PE_parse_boot_argn("mclk24", &want24, sizeof(want24));
        if (want24) {
            /* MCLK0 from PLL2_OUT_AUX (480 MHz), MND 1/20, /1 -> 24 MHz */
            WR(camcc + MCLK0_M, 1);
            WR(camcc + MCLK0_N, (uint32_t)(~(20u - 1)) & 0xff);    /* ~(n-m) */
            WR(camcc + MCLK0_D, (uint32_t)(~(2u * 19u)) & 0xff);   /* ~(2*d), d=clamp(n,m,n-m)=19 */
            WR(camcc + MCLK0_CFG_RCGR, (RD(camcc + MCLK0_CFG_RCGR) & ~0x37ffu) | (5u << 8) | (2u << 12) | 0x01u);
            WR(camcc + MCLK0_CMD_RCGR, RD(camcc + MCLK0_CMD_RCGR) | RCG_UPDATE);
            __asm__ volatile ("dsb sy" ::: "memory");
            for (int i = 0; i < 100 && (RD(camcc + MCLK0_CMD_RCGR) & RCG_UPDATE); i++) { delay(100); }
            WR(camcc + MCLK0_CBCR, RD(camcc + MCLK0_CBCR) | 1u);
            __asm__ volatile ("dsb sy" ::: "memory");
            delay(1000);
            on24 = !(RD(camcc + MCLK0_CBCR) & 0x80000000u);
        }
        if (on24) {
            cam_mclk_khz = 24000;
        } else {
            /* XO 19.2 MHz: proven clean clock (M1 read the chip id on it). src 0 (TCXO), no MND, /1.
             * At 19.2 the sensor's MIPI link runs 0.8x; the CSIPHY settle count adjusts for it. */
            WR(camcc + MCLK0_CFG_RCGR, (RD(camcc + MCLK0_CFG_RCGR) & ~0x37ffu) | (0u << 8) | (0u << 12) | 0x01u);
            WR(camcc + MCLK0_CMD_RCGR, RD(camcc + MCLK0_CMD_RCGR) | RCG_UPDATE);
            __asm__ volatile ("dsb sy" ::: "memory");
            for (int i = 0; i < 100 && (RD(camcc + MCLK0_CMD_RCGR) & RCG_UPDATE); i++) { delay(100); }
            WR(camcc + MCLK0_CBCR, RD(camcc + MCLK0_CBCR) | 1u);
            __asm__ volatile ("dsb sy" ::: "memory");
            delay(1000);
            cam_mclk_khz = 19200;
        }
    }
    {
        char line[128];
        snprintf(line, sizeof(line), "camera: MCLK0 %u kHz, CBCR %#x (bit31=off) cfg %#x | PLL2 mode %#x\n",
            cam_mclk_khz, RD(camcc + MCLK0_CBCR), RD(camcc + MCLK0_CFG_RCGR), RD(camcc + PLL2 + PLL2_MODE));
        camlog(line);
    }
    /* GPIO 13 to the cam_mclk function (index 1), 4 mA */
    g = pin(MCLK0_GPIO);
    if (g != NULL) {
        g[TLMM_CFG / 4] = (1u << 2) | (1u << 6);     /* function 1, 4 mA, no pull */
        __asm__ volatile ("dsb sy" ::: "memory");
    }
}

static void
imx_write_seq(struct i2c *cci, const struct imx_reg *regs, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        uint8_t sz = regs[i].size ? regs[i].size : 1;
        /* IMX "N-byte" registers are consecutive 8-bit regs, MSB at addr, LSB last */
        for (uint8_t b = 0; b < sz; b++) {
            uint8_t byte = (uint8_t)((regs[i].val >> (8 * (sz - 1 - b))) & 0xff);
            (void)i2c_write_reg16(cci, IMX_ADDR, (uint16_t)(regs[i].addr + b), byte);
        }
    }
}

static void
rear_sensor_stream(struct i2c *cci)
{
    uint8_t s = 0xff;
    char line[120];

    /* imx363_start_streaming: sw reset, 12 ms, PLL table, common regs, continuous clock, mode regs, stream on */
    (void)i2c_write_reg16(cci, IMX_ADDR, 0x0103, 0x01);
    delay(12000);
    imx_write_seq(cci, imx363_pll_642,    sizeof(imx363_pll_642)    / sizeof(imx363_pll_642[0]));
    imx_write_seq(cci, imx363_common,     sizeof(imx363_common)     / sizeof(imx363_common[0]));
    (void)i2c_write_reg16(cci, IMX_ADDR, 0x4040, 0x00);   /* CLK_BLANK_STOP=0: continuous clock */
    imx_write_seq(cci, imx363_2016x1512,  sizeof(imx363_2016x1512)  / sizeof(imx363_2016x1512[0]));
    (void)i2c_write_reg16(cci, IMX_ADDR, 0x0100, 0x01);   /* STREAM ON */
    delay(5000);
    (void)i2c_read_reg16(cci, IMX_ADDR, 0x0100, &s, 1);
    snprintf(line, sizeof(line), "camera: IMX363 STREAM ON (2016x1512, 642 Mbps/lane); mode_select readback=%02x (want 01)\n", s);
    camlog(line);
}

static void
rear_sensor_probe(struct i2c *slg)
{
    struct i2c cci = { CCI0_SDA_GPIO, CCI0_SCL_GPIO };
    volatile uint32_t *rst;
    uint8_t v[2] = { 0, 0 };
    char line[112];
    int r;

    camcc = ml_io_map(CAMCC_PHYS, 0x10000);
    titan = ml_io_map(TITAN_GDSCR_PHYS & ~0x3fffUL, 0x4000);
    if (titan != 0) {
        titan += (TITAN_GDSCR_PHYS & 0x3fffUL);
    }
    if (pin(CCI0_SDA_GPIO) == NULL || pin(CCI0_SCL_GPIO) == NULL || pin(CAM_RESET_GPIO) == NULL) {
        camlog("camera: could not map the sensor GPIOs\n");
        return;
    }
    /* rear rails: vdig LDO1 2.85V (0x82), vana LDO3 2.85V (0xa5), vio LDO7 1.8V (0x3c), custom1 LDO4 1.8V (0x3c) */
    slg_ldo(slg, 1, 0x82);
    slg_ldo(slg, 3, 0xa5);
    slg_ldo(slg, 7, 0x3c);
    slg_ldo(slg, 4, 0x3c);
    (void)i2c_write_reg16(slg, SLG_ADDR, SLG_MATRIX_CONF_A, 0x4d);       /* enable LDO1,3,4,7 */
    delay(5000);
    camcc_mclk0_on();
    if (cam_level >= 4) {
        camss_csiphy_clocks_on();       /* M2b: PLL0 + CSIPHY0/phytimer 300 MHz data clocks */
    }
    delay(2000);
    /* reset high */
    rst = pin(CAM_RESET_GPIO);
    rst[TLMM_IO / 4] = 2;                        /* output value 1 */
    rst[TLMM_CFG / 4] = 1u << 9;                 /* output enable */
    __asm__ volatile ("dsb sy" ::: "memory");
    delay(10000);
    sda_scl_init(CCI0_SDA_GPIO);
    sda_scl_init(CCI0_SCL_GPIO);
    delay(1000);
    r = i2c_read_reg16(&cci, IMX_ADDR, IMX_CHIP_ID_REG, v, 2);
    snprintf(line, sizeof(line), "camera: IMX363 chip id read %s: %02x%02x (want 0363) on CCI0 GPIO %u/%u\n",
        r == 0 ? "ok" : "NO ACK", v[0], v[1], CCI0_SDA_GPIO, CCI0_SCL_GPIO);
    camlog(line);
    if (r == 0 && cam_level >= 3) {
        rear_sensor_stream(&cci);
    }
}

static void
camera_probe(void)
{
    struct i2c slg = { SLG_SDA_GPIO, SLG_SCL_GPIO };
    uint8_t id[3] = { 0, 0, 0 };
    int r;

    camlog("camera: probe starting\n");
    if (pin(SLG_SDA_GPIO) == NULL || pin(SLG_SCL_GPIO) == NULL) {
        camlog("camera: could not map the GPIO pages\n");
        return;
    }
    sda_scl_init(SLG_SDA_GPIO);
    sda_scl_init(SLG_SCL_GPIO);
    delay(1000);

    /* the camera PMIC's pattern id (0x1105..0x1107): if it ACKs, the SLG51000 is powered (ABL left its chip-enable on)
     * and its I2C bus is ours; if not, PM6150L GPIO4 (its chip-enable) has to be driven, next milestone. */
    camlog("camera: enabling the SLG51000 (PM6150L GPIO 1,5 enable; 4,6 cs)\n");
    pm6150l_gpio_high(1);       /* dlg,enable-gpios */
    pm6150l_gpio_high(5);
    pm6150l_gpio_high(4);       /* dlg,cs-gpios */
    pm6150l_gpio_high(6);
    delay(5000);
    /* confirm the enable GPIOs actually latched high (SPMI write DONE != pin driven) */
    {
        char line[136];
        unsigned g;
        for (g = 1; g <= 5; g += 4) {           /* GPIO1 then GPIO5 */
            unsigned p = 0xc0 + (g - 1);
            snprintf(line, sizeof(line),
                "camera: PM6150L GPIO%u readback MODE(40)=%#x OUT_SRC(44)=%#x DIG_OUT(45)=%#x EN(46)=%#x STATUS(08)=%#x\n",
                g, spmi_read8(4, p, 0x40), spmi_read8(4, p, 0x44), spmi_read8(4, p, 0x45),
                spmi_read8(4, p, 0x46), spmi_read8(4, p, 0x08));
            camlog(line);
        }
    }
    {
        char line[96];
        int drv = i2c_ping(&slg, 0x5a);     /* the DRV2624 haptic, known present on this bus */
        int slgp = i2c_ping(&slg, SLG_ADDR);
        snprintf(line, sizeof(line), "camera: bus check - DRV2624(0x5a) ACK %d, SLG51000(0x75) ACK %d\n", drv, slgp);
        camlog(line);
    }
    camlog("camera: reading the SLG51000\n");
    r = i2c_read_reg16(&slg, SLG_ADDR, SLG_PATTERN_ID_B0, id, 3);
    /* cold-boot tolerant: a cold SLG may need a clean enable rising edge. Toggle 1,5 low->high and retry. */
    for (int attempt = 1; r != 0 && attempt <= 5; attempt++) {
        char line[80];
        pm6150l_gpio_low(1);
        pm6150l_gpio_low(5);
        delay(10000);
        pm6150l_gpio_high(1);
        pm6150l_gpio_high(5);
        delay(10000 * (unsigned)attempt);
        r = i2c_read_reg16(&slg, SLG_ADDR, SLG_PATTERN_ID_B0, id, 3);
        snprintf(line, sizeof(line), "camera: SLG enable toggle retry %d -> %s\n", attempt, r == 0 ? "ACK" : "silent");
        camlog(line);
    }
    if (r == 0 && cam_level >= 2) {
        rear_sensor_probe(&slg);
    }
    if (r == 0) {
        char line[96];
        snprintf(line, sizeof(line), "camera: SLG51000 answered on GPIO %u/%u: pattern id %02x %02x %02x\n",
            SLG_SDA_GPIO, SLG_SCL_GPIO, id[0], id[1], id[2]);
        camlog(line);
    } else {
        char line[112];
        snprintf(line, sizeof(line), "camera: SLG51000 at %#x did not ACK (not powered or chip-enable off)\n", SLG_ADDR);
        camlog(line);
    }
}

static thread_call_t cam_call;

static void
camera_run(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
    camera_probe();
}

__startup_func
static void
camera_init(void)
{
    uint64_t deadline;

    if (!PE_parse_boot_argn("cam", &cam_level, sizeof(cam_level)) || cam_level == 0) {
        return;
    }
    /* at 90 s, so the result reaches the run log (early kernel messages are lost); the camera is idle until then */
    camlog("camera: cam set; probe in 90 s\n");
    cam_call = thread_call_allocate(camera_run, NULL);
    clock_interval_to_deadline(90, NSEC_PER_SEC, &deadline);
    thread_call_enter_delayed(cam_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, camera_init);
