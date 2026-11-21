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
#include <vm/vm_kern.h>
#include <sys/vnode.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/kauth.h>
#include <sys/mount.h>
#include <sys/vnode_if.h>
#include <machine/machine_routines.h>

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
/* camss "top": the register/AXI fabric every camss block sits behind (camss.c powers these with VFE0).
 * Without cpas_ahb (from slow_ahb) the CSIPHY/CSID/VFE registers read 0 and writes are dropped. */
static void
camss_top_on(void)
{
    vm_offset_t gcc_cam = ml_io_map(0x10b000UL, 0x1000);    /* GCC camera branches */

    if (gcc_cam != 0) {
        WR(gcc_cam + 0x020, RD(gcc_cam + 0x020) | 1u);     /* gcc_camera_hf_axi_clk */
        WR(gcc_cam + 0x06c, RD(gcc_cam + 0x06c) | 1u);     /* gcc_camera_sf_axi_clk */
        __asm__ volatile ("dsb sy" ::: "memory");
    }
    camcc_rcg_set(0x7058, 0, 1);        /* slow_ahb_src: XO 19.2 MHz (src 0, /1) */
    WR(camcc + 0xc124, RD(camcc + 0xc124) | 1u);           /* cpas_ahb_clk */
    camcc_rcg_set(0xc12c, 2, 7);        /* camnoc_axi_src: PLL0_OUT_EVEN /4 = 150 MHz (field 2*4-1) */
    WR(camcc + 0xc148, RD(camcc + 0xc148) | 1u);           /* camnoc_axi_clk */
    __asm__ volatile ("dsb sy" ::: "memory");
    delay(1000);
    {
        char line[160];
        snprintf(line, sizeof(line), "camera: camss top - gcc hf_axi %#x sf_axi %#x, cpas_ahb %#x, camnoc_axi %#x (bit31=off)\n",
            gcc_cam ? RD(gcc_cam + 0x020) : 0xdead, gcc_cam ? RD(gcc_cam + 0x06c) : 0xdead,
            RD(camcc + 0xc124), RD(camcc + 0xc148));
        camlog(line);
    }
}

static void
camss_csiphy_clocks_on(void)
{
    camcc_pll0_up();                    /* PLL0 first: camnoc_axi_src and the CSIPHY clocks use OUT_EVEN */
    camss_top_on();
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

/* Generated from sm7150-mainline camss-csiphy-3ph-1-0.c lane_regs_sm7150[]. */
/* param: 0=write data; 2=settle_cnt&0xff; 3=settle high byte; 4,7=skip. base=0xace0000, common offset 0x800. */
struct csiphy_reg { uint16_t addr; uint8_t data; uint8_t delay_us; uint8_t param; };
static const struct csiphy_reg csiphy0_lane_regs[] = {
	{ 0x0030, 0x00, 0, 0 },
	{ 0x0904, 0x00, 0, 0 },
	{ 0x0910, 0x00, 0, 0 },
	{ 0x0900, 0x0f, 0, 0 },
	{ 0x0908, 0x06, 0, 0 },
	{ 0x0904, 0x07, 0, 0 },
	{ 0x002c, 0x01, 0, 0 },
	{ 0x0034, 0x0f, 0, 0 },
	{ 0x0010, 0x50, 0, 0 },
	{ 0x001c, 0x0a, 0, 0 },
	{ 0x0014, 0x60, 0, 0 },
	{ 0x0028, 0x00, 0, 0 },
	{ 0x003c, 0xb8, 0, 0 },
	{ 0x0000, 0x91, 0, 0 },
	{ 0x0004, 0x0c, 0, 0 },
	{ 0x0020, 0x00, 0, 0 },
	{ 0x0008, 0x10, 0, 2 },
	{ 0x0010, 0x52, 0, 0 },
	{ 0x0038, 0xfe, 0, 0 },
	{ 0x005c, 0xc0, 0, 0 },
	{ 0x0060, 0x0d, 0, 0 },
	{ 0x0800, 0x02, 0, 0 },
	{ 0x0000, 0x00, 0, 4 },
	{ 0x0730, 0x00, 0, 0 },
	{ 0x0c84, 0x00, 0, 0 },
	{ 0x0c90, 0x00, 0, 0 },
	{ 0x0c80, 0x0f, 0, 0 },
	{ 0x0c88, 0x06, 0, 0 },
	{ 0x0c84, 0x07, 0, 0 },
	{ 0x072c, 0x01, 0, 0 },
	{ 0x0734, 0x0f, 0, 0 },
	{ 0x0710, 0x50, 0, 0 },
	{ 0x071c, 0x0a, 0, 0 },
	{ 0x0714, 0x60, 0, 0 },
	{ 0x0728, 0x04, 0, 0 },
	{ 0x073c, 0xb8, 0, 0 },
	{ 0x0700, 0x80, 0, 0 },
	{ 0x0704, 0x0c, 0, 0 },
	{ 0x0720, 0x00, 0, 0 },
	{ 0x0708, 0x04, 0, 2 },
	{ 0x070c, 0xff, 0, 0 },
	{ 0x0710, 0x52, 0, 0 },
	{ 0x0738, 0x1f, 0, 0 },
	{ 0x0800, 0x02, 0, 0 },
	{ 0x0000, 0x00, 0, 4 },
	{ 0x0000, 0x00, 0, 4 },
	{ 0x0230, 0x00, 0, 0 },
	{ 0x0a04, 0x00, 0, 0 },
	{ 0x0a10, 0x00, 0, 0 },
	{ 0x0a00, 0x0f, 0, 0 },
	{ 0x0a08, 0x06, 0, 0 },
	{ 0x0a04, 0x07, 0, 0 },
	{ 0x022c, 0x01, 0, 0 },
	{ 0x0234, 0x0f, 0, 0 },
	{ 0x0210, 0x50, 0, 0 },
	{ 0x021c, 0x0a, 0, 0 },
	{ 0x0214, 0x60, 0, 0 },
	{ 0x0228, 0x00, 0, 0 },
	{ 0x023c, 0xb8, 0, 0 },
	{ 0x0200, 0x91, 0, 0 },
	{ 0x0204, 0x0c, 0, 0 },
	{ 0x0220, 0x00, 0, 0 },
	{ 0x0208, 0x04, 0, 2 },
	{ 0x0210, 0x52, 0, 0 },
	{ 0x0238, 0xfe, 0, 0 },
	{ 0x025c, 0xc0, 0, 0 },
	{ 0x0260, 0x0d, 0, 0 },
	{ 0x0800, 0x02, 0, 0 },
	{ 0x0000, 0x00, 0, 4 },
	{ 0x0430, 0x00, 0, 0 },
	{ 0x0b04, 0x00, 0, 0 },
	{ 0x0b10, 0x00, 0, 0 },
	{ 0x0b00, 0x0f, 0, 0 },
	{ 0x0b08, 0x06, 0, 0 },
	{ 0x0b04, 0x07, 0, 0 },
	{ 0x042c, 0x01, 0, 0 },
	{ 0x0434, 0x0f, 0, 0 },
	{ 0x0410, 0x50, 0, 0 },
	{ 0x041c, 0x0a, 0, 0 },
	{ 0x0414, 0x60, 0, 0 },
	{ 0x0428, 0x00, 0, 0 },
	{ 0x043c, 0xb8, 0, 0 },
	{ 0x0400, 0x91, 0, 0 },
	{ 0x0404, 0x0c, 0, 0 },
	{ 0x0420, 0x00, 0, 0 },
	{ 0x0408, 0x04, 0, 2 },
	{ 0x0410, 0x52, 0, 0 },
	{ 0x0438, 0xfe, 0, 0 },
	{ 0x045c, 0xc0, 0, 0 },
	{ 0x0460, 0x0d, 0, 0 },
	{ 0x0800, 0x02, 0, 0 },
	{ 0x0000, 0x00, 0, 4 },
	{ 0x0630, 0x00, 0, 0 },
	{ 0x0c04, 0x00, 0, 0 },
	{ 0x0c10, 0x00, 0, 0 },
	{ 0x0c00, 0x0f, 0, 0 },
	{ 0x0c08, 0x06, 0, 0 },
	{ 0x0c04, 0x07, 0, 0 },
	{ 0x062c, 0x01, 0, 0 },
	{ 0x0634, 0x0f, 0, 0 },
	{ 0x0610, 0x50, 0, 0 },
	{ 0x061c, 0x0a, 0, 0 },
	{ 0x0614, 0x60, 0, 0 },
	{ 0x0628, 0x00, 0, 0 },
	{ 0x063c, 0xb8, 0, 0 },
	{ 0x0600, 0x91, 0, 0 },
	{ 0x0604, 0x0c, 0, 0 },
	{ 0x0620, 0x00, 0, 0 },
	{ 0x0608, 0x04, 0, 2 },
	{ 0x0610, 0x52, 0, 0 },
	{ 0x0638, 0xfe, 0, 0 },
	{ 0x065c, 0xc0, 0, 0 },
	{ 0x0660, 0x0d, 0, 0 },
	{ 0x0800, 0x02, 0, 0 },
	{ 0x0000, 0x00, 0, 4 },
};

/* CSIPHY0 bring-up (camss-csiphy-3ph-1-0.c, CAMSS_7150). base 0xace0000, common ctrl offset 0x800,
 * status offset 0xb0, 300 MHz timer. Needs CSIPHY clocks on (camss_csiphy_clocks_on). */
static vm_offset_t csiphy0;
#define CSP_CTRL(n)    (csiphy0 + 0x800 + 4u * (n))
#define CSP_STAT(n)    (csiphy0 + 0x800 + 0xb0 + 4u * (n))

static void
csiphy0_init(void)
{
    uint32_t hw_version;
    int settle;
    char line[160];

    csiphy0 = ml_io_map(0xace0000UL, 0x4000);     /* 16 KB: covers lane regs + common block */
    if (csiphy0 == 0) {
        camlog("camera: CSIPHY0 map failed\n");
        return;
    }
    /* HW version (sanity: block alive + clocked): CTRL6 = SHOW_REV_ID(bit1), read status 12..15 */
    WR(CSP_CTRL(6), 0x2u);
    __asm__ volatile ("dsb sy" ::: "memory");
    hw_version  = (RD(CSP_STAT(12)) & 0xff);
    hw_version |= (RD(CSP_STAT(13)) & 0xff) << 8;
    hw_version |= (RD(CSP_STAT(14)) & 0xff) << 16;
    hw_version |= (RD(CSP_STAT(15)) & 0xff) << 24;

    /* settle count: link_freq = 321 MHz * mclk/24000 (DDR half of the 642 Mbps link); timer 300 MHz */
    {
        uint64_t link = 321000000ULL * (uint64_t)cam_mclk_khz / 24000ULL;
        uint64_t ui   = (1000000000000ULL / link) / 2ULL;      /* ps */
        uint64_t ths  = 85000ULL + 6ULL * ui;
        uint64_t tper = 1000000000000ULL / 300000000ULL;       /* 3333 ps */
        settle = (int)(ths / tper) - 6;
        if (settle < 0) settle = 0;
        if (settle > 0xff) settle = 0xff;
    }

    /* reset */
    WR(CSP_CTRL(0), 0x1u);
    __asm__ volatile ("dsb sy" ::: "memory");
    delay(5000);
    WR(CSP_CTRL(0), 0x0u);
    __asm__ volatile ("dsb sy" ::: "memory");

    /* common control: lane mask = CLK_ENABLE bit7 | BIT(pos*2) per data lane: the 3-phase combo PHY puts
     * the D-PHY data lanes on even PHY lanes (blocks 0x000/0x200/0x400/0x600), so DL0-3 = bits 0,2,4,6 -> 0xd5.
     * (0x8f enabled only DL0+DL1: CSID saw lane 0/1 FIFO overflow and no packets.) */
    WR(CSP_CTRL(5), 0xd5u);
    WR(CSP_CTRL(6), 0x1u);
    WR(CSP_CTRL(7), 0x02u);
    WR(CSP_CTRL(0), 0x00u);
    __asm__ volatile ("dsb sy" ::: "memory");

    /* apply the lane table (settle byte substituted where param==2) */
    for (unsigned i = 0; i < sizeof(csiphy0_lane_regs) / sizeof(csiphy0_lane_regs[0]); i++) {
        const struct csiphy_reg *r = &csiphy0_lane_regs[i];
        uint32_t val;
        if (r->param == 4 || r->param == 7) { continue; }      /* DNP / SKEW: skip */
        else if (r->param == 2) { val = (uint32_t)(settle & 0xff); }
        else if (r->param == 3) { val = (uint32_t)((settle >> 8) & 0xff); }
        else { val = r->data; }
        WR(csiphy0 + r->addr, val);
        if (r->delay_us) { delay(r->delay_us); }
    }
    __asm__ volatile ("dsb sy" ::: "memory");

    /* clear common ctrl 11..21 (interrupt masks) */
    for (unsigned n = 11; n <= 21; n++) { WR(CSP_CTRL(n), 0); }
    __asm__ volatile ("dsb sy" ::: "memory");

    snprintf(line, sizeof(line), "camera: CSIPHY0 init - hw_version %#x, settle_cnt %d (0x%x), link@%u kHz MCLK; CTRL5 readback %#x\n",
        hw_version, settle, settle, cam_mclk_khz, RD(CSP_CTRL(5)));
    camlog(line);
}


/* ---- CSID0 (Titan 170 gen2 CSID, 0xacb3000, in the IFE0 power domain) ----
 * camss-csid-gen2.c + downstream cam_ife_csid170.h. RDI0 takes VC0/DT 0x2b (RAW10) from CSIPHY0. */
static vm_offset_t csid0;
#define CSID_RD(o)      RD(csid0 + (o))
#define CSID_WR(o, v)   WR(csid0 + (o), (v))

static void
csid0_init(void)
{
    char line[160];
    uint32_t hwv, st = 0;
    int i;

    /* IFE0 GDSC (0xa004, POLL_CFG_GDSCR: power-up-complete is CFG_GDSCR bit16) */
    WR(camcc + 0xa004, RD(camcc + 0xa004) & ~1u);
    __asm__ volatile ("dsb sy" ::: "memory");
    for (i = 0; i < 200; i++) {
        if ((RD(camcc + 0xa004) & 0x80000000u) || (RD(camcc + 0xa008) & (1u << 16))) { break; }
        delay(50);
    }
    /* CSID + IFE0 clocks */
    camcc_rcg_set(0xa03c, 2, 3);                     /* ife_0_csid_src: PLL0_OUT_EVEN /2 = 300 MHz */
    camcc_rcg_set(0xa010, 0, 1);                     /* ife_0_clk_src: XO for now (PLL3 later, for the VFE) */
    WR(camcc + 0xa054, RD(camcc + 0xa054) | 1u);     /* ife_0_csid_clk */
    WR(camcc + 0xa07c, RD(camcc + 0xa07c) | 1u);     /* ife_0_cphy_rx_clk */
    WR(camcc + 0xa028, RD(camcc + 0xa028) | 1u);     /* ife_0_clk */
    WR(camcc + 0xa080, RD(camcc + 0xa080) | 1u);     /* ife_0_axi_clk */
    __asm__ volatile ("dsb sy" ::: "memory");
    delay(1000);
    snprintf(line, sizeof(line), "camera: IFE0 gdsc %#x cfg %#x | csid %#x cphy_rx %#x ife %#x axi %#x (bit31=off)\n",
        RD(camcc + 0xa004), RD(camcc + 0xa008), RD(camcc + 0xa054), RD(camcc + 0xa07c), RD(camcc + 0xa028), RD(camcc + 0xa080));
    camlog(line);

    csid0 = ml_io_map(0xacb3000UL, 0x1000);
    if (csid0 == 0) {
        camlog("camera: CSID0 map failed\n");
        return;
    }
    hwv = CSID_RD(0x0);
    /* reset (csid_reset): unmask + clear the top irq, strobe the resets (0x1e keeps registers), poll RESET_DONE */
    CSID_WR(0x78, 1); CSID_WR(0x80, 1);
    CSID_WR(0x74, 1); CSID_WR(0x80, 1);
    CSID_WR(0x10, 0x1e);
    __asm__ volatile ("dsb sy" ::: "memory");
    for (i = 0; i < 200; i++) {
        st = CSID_RD(0x70);
        if (st & 1u) { break; }
        delay(50);
    }
    CSID_WR(0x78, 1); CSID_WR(0x80, 1);

    /* RDI0 (csid gen2 __csid_configure_rdi_stream): byte counter, format measure, timestamps,
     * payload-only decode (0xf), DT 0x2b (RAW10), VC 0, DT_ID 0 */
    CSID_WR(0x300, (1u << 0) | (1u << 1) | (1u << 2) | (0xfu << 12) | (0x2bu << 16));
    CSID_WR(0x304, 2u);                               /* timestamp strobe post-irq */
    CSID_WR(0x310, 1); CSID_WR(0x30c, 0);             /* frame drop period/pattern */
    CSID_WR(0x318, 1); CSID_WR(0x314, 0);             /* irq subsample */
    CSID_WR(0x328, 1); CSID_WR(0x324, 0);             /* pixel drop */
    CSID_WR(0x330, 1); CSID_WR(0x32c, 0);             /* line drop */
    CSID_WR(0x308, 0);
    if (cam_level >= 7) {                            /* vertical crop lines 0..767: a frame of 768 x 2520 B fits 2 MiB */
        CSID_WR(0x320, (767u << 16) | 0u);
        CSID_WR(0x300, CSID_RD(0x300) | (1u << 6));
    }
    CSID_WR(0x300, CSID_RD(0x300) | (1u << 31));     /* RDI0 enable */
    /* CSI2 RX: 4 lanes (3), lane assign 0x3210, PHY 0, D-PHY; ECC correction + MISR */
    CSID_WR(0x100, 3u | (0x3210u << 4) | (0u << 20));
    CSID_WR(0x104, (1u << 0) | (1u << 6));
    /* capture the first long + short packet headers for VC0 (debug) */
    CSID_WR(0x108, (1u << 0) | (1u << 1));
    /* irq masks so the status registers latch (no interrupt line is enabled at the GIC) */
    CSID_WR(0x24, 0x0fffffffu);
    CSID_WR(0x44, 0xffffffffu);
    CSID_WR(0x308, 1u);                               /* RDI0 resume at frame boundary */
    __asm__ volatile ("dsb sy" ::: "memory");
    snprintf(line, sizeof(line), "camera: CSID0 init - hw_version %#x, reset %s (top irq %#x), RDI0 cfg0 %#x, RX cfg0 %#x\n",
        hwv, (st & 1u) ? "done" : "TIMEOUT", st, CSID_RD(0x300), CSID_RD(0x100));
    camlog(line);
}

/* after the sensor streams: packet counters, error stats, captured headers, measured frame */
static void
csid0_report(const char *when)
{
    char line[200];

    if (csid0 == 0) {
        return;
    }
    snprintf(line, sizeof(line),
        "camera: CSID0 [%s] RX pkts %u ecc %#x crc_err %u | rx_irq %#x rdi0_irq %#x rdi0_st %#x\n",
        when, CSID_RD(0x160), CSID_RD(0x164), CSID_RD(0x168), CSID_RD(0x20), CSID_RD(0x40), CSID_RD(0x350));
    camlog(line);
    snprintf(line, sizeof(line),
        "camera: CSID0 [%s] short pkt %#x/%#x long pkt %#x/%#x | measure %#x %#x %#x | bytes %u/%u | sof_ts %#x\n",
        when, CSID_RD(0x128), CSID_RD(0x12c), CSID_RD(0x130), CSID_RD(0x134),
        CSID_RD(0x378), CSID_RD(0x37c), CSID_RD(0x380), CSID_RD(0x3e0), CSID_RD(0x3e4), CSID_RD(0x390));
    camlog(line);
}


/* a camcc FABIA PLL (fabia_ops: OPMODE enable), as camcc_pll0_up, for any offset/config (PLL3 for the IFE clock) */
static void
camcc_fabia_up(uint32_t off, uint32_t l, uint32_t alpha, uint32_t pd_mask, uint32_t pd_val, uint32_t test_ctl, const char *name)
{
    vm_offset_t b = camcc + off;
    int locked = 0;
    char line[96];

    if (RD(b + PLL2_MODE) & 1u) {
        snprintf(line, sizeof(line), "camera: %s already running (mode %#x)\n", name, RD(b + PLL2_MODE));
        camlog(line);
        return;
    }
    WR(b + PLL2_L_VAL, l);
    if (alpha) { WR(b + PLL2_FRAC, alpha); }
    if (test_ctl) { WR(b + PLL_TEST_CTL, test_ctl); }
    WR(b + PLL2_USER_CTL, (RD(b + PLL2_USER_CTL) & ~pd_mask) | pd_val);
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 0x800000u);   /* UPDATE_BYPASS */
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 4u);          /* RESET_N */
    __asm__ volatile ("dsb sy" ::: "memory");
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) & ~1u);
    WR(b + PLL2_OPMODE, 0);
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 4u);
    __asm__ volatile ("dsb sy" ::: "memory");
    delay(10);
    WR(b + PLL2_OPMODE, 1);
    __asm__ volatile ("dsb sy" ::: "memory");
    for (int i = 0; i < 500; i++) {
        if (RD(b + PLL2_MODE) & 0x80000000u) { locked = 1; break; }
        delay(10);
    }
    WR(b + PLL2_USER_CTL, RD(b + PLL2_USER_CTL) | 0x7u);
    WR(b + PLL2_MODE, RD(b + PLL2_MODE) | 1u);
    __asm__ volatile ("dsb sy" ::: "memory");
    snprintf(line, sizeof(line), "camera: %s %s (mode %#x)\n", name, locked ? "LOCKED" : "did NOT lock", RD(b + PLL2_MODE));
    camlog(line);
}

/* ---- VFE0 (Titan 170 IFE0, 0xacaf000): RDI0 -> bus write master 0 -> DMA into memory (camss-vfe-17x.c) ----
 * The camera AXI goes through the apps SMMU, which faults unmatched streams and refuses bypass: the buffer is
 * shown at a 32-bit IOVA through a translating bank of our own (hakios_sm7150_smmu_pages_masked, Wi-Fi driver). */
extern int hakios_sm7150_smmu_pages_masked(const uint32_t *sids, const uint32_t *masks, unsigned n, uint64_t iova, uint64_t pa, uint32_t pages);
extern void hakios_sm7150_smmu_status(uint32_t *fsr, uint64_t *far, uint32_t *gfsr, uint32_t *gfsynr1);
extern uint64_t kvtophys(vm_offset_t va);

#define CAM_IOVA        0x10000000ULL
#define CAM_BUF_SIZE    0x400000u               /* 4 MiB: two SMMU L3 stretches; a 2016x1512 RAW10 frame is 3810240 B */
#define CAM_FRAME_BYTES 3822336u                /* 1512 lines x 2528 B: the WM pads each 2520 B RAW10 line to 16 B */
#define CAM_LINE_BYTES  2528u                   /* 2016 px of MIPI-packed RAW10 = 2520 B, padded to 16 B */
static vm_offset_t vfe0;
static vm_offset_t cam_buf;
static uint64_t cam_buf_pa;
static int cam_smmu_bank = -1;
#define VFE_RD(o)       RD(vfe0 + (o))
#define VFE_WR(o, v)    WR(vfe0 + (o), (v))

static void
vfe0_init(void)
{
    char line[200];
    uint32_t st = 0;
    int i;

    /* IFE0 core clock: PLL3 760 MHz -> OUT_EVEN 380 MHz (ife_0_clk_src src 6, /1); XO is far too slow for 4-lane RDI */
    camcc_fabia_up(0x3000, 0x27, 0x9555, 0xf00u, 0x100u, 0x40000000u, "PLL3");
    camcc_rcg_set(0xa010, 6, 1);
    WR(camcc + 0xa028, RD(camcc + 0xa028) | 1u);
    __asm__ volatile ("dsb sy" ::: "memory");

    vfe0 = ml_io_map(0xacaf000UL, 0x4000);
    if (vfe0 == 0) {
        camlog("camera: VFE0 map failed\n");
        return;
    }
    /* global reset (vfe_global_reset): only RESET_ACK unmasked; core, camif, bus, bus bdg, regs, testgen, dsp,
     * idle cgc, rdi0-3; then poll IRQ status 0 bit31 and clear it */
    VFE_WR(0x05c, 1u << 31);
    __asm__ volatile ("dsb sy" ::: "memory");
    VFE_WR(0x018, 0x3f9fu);
    for (i = 0; i < 200; i++) {
        st = VFE_RD(0x06c);
        if (st & (1u << 31)) { break; }
        delay(50);
    }
    VFE_WR(0x064, 1u << 31);
    VFE_WR(0x058, 1u);

    /* DMA buffer: 4 MiB physically contiguous, filled with 0xa5 so the written part shows */
    if (kmem_alloc_contig(kernel_map, &cam_buf, CAM_BUF_SIZE, PAGE_MASK, 0, 0,
        KMA_DATA | KMA_ZERO | KMA_PERMANENT, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
        camlog("camera: no DMA buffer\n");
        return;
    }
    memset((void *)cam_buf, 0xa5, CAM_BUF_SIZE);
    for (uint32_t o = 0; o < CAM_BUF_SIZE; o += 64) {
        __asm__ volatile ("dc civac, %0" : : "r" ((uint8_t *)cam_buf + o) : "memory");
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    cam_buf_pa = kvtophys(cam_buf);
    {
        /* the IFE's stream groups (downstream msm_cam_smmu_ife, de-duplicated: the masks overlap) */
        static const uint32_t sids[3]  = { 0x900, 0x880, 0x820 };
        static const uint32_t masks[3] = { 0x460, 0x460, 0x440 };
        cam_smmu_bank = hakios_sm7150_smmu_pages_masked(sids, masks, 3, CAM_IOVA, cam_buf_pa, CAM_BUF_SIZE >> 12);
    }

    /* bus write master 0 (RDI0), as vfe_wm_start: frame-based (width/stride 0xff01), MIPI raw */
    VFE_WR(0x226c, (1u << 1) | (1u << 7));      /* WM debug status cfg */
    VFE_WR(0x2080, 0);                          /* addr sync: frame header */
    VFE_WR(0x200c, 0xfffffu);                   /* no clock gating at the bus input */
    VFE_WR(0x211c, 0);                          /* test bus off */
    VFE_WR(0x2084, 0xffffffu);                  /* addr no sync */
    VFE_WR(0x225c, 0xfu);                       /* burst limit */
    VFE_WR(0x221c, 0xff01u);                    /* buffer width: frame-based */
    VFE_WR(0x2220, 0);                          /* buffer height */
    VFE_WR(0x2224, 0);                          /* packer */
    VFE_WR(0x2228, 0xff01u);                    /* stride: frame-based */
    VFE_WR(0x2248, 0); VFE_WR(0x224c, 1);       /* irq subsample: every frame */
    VFE_WR(0x2250, 0); VFE_WR(0x2254, 1);       /* frame drop: keep every frame */
    VFE_WR(0x2214, (uint32_t)CAM_IOVA);         /* image address */
    VFE_WR(0x2258, 0);                          /* frame increment 0: frames overwrite the one buffer, never walk past it */
    /* status latching (no interrupt line is enabled at the GIC) */
    VFE_WR(0x05c, ~0u); VFE_WR(0x060, ~0u);
    VFE_WR(0x2044, ~0u); VFE_WR(0x2048, ~0u); VFE_WR(0x204c, ~0u);
    VFE_WR(0x2208, 1u | (1u << 1));             /* WM0 enable, MIPI raw */
    __asm__ volatile ("dsb sy" ::: "memory");
    VFE_WR(0x4ac, 1u << 1);                     /* reg update: RDI0 */
    __asm__ volatile ("dsb sy" ::: "memory");
    snprintf(line, sizeof(line),
        "camera: VFE0 init - ife clk cfg %#x cbcr %#x, reset %s (st0 %#x), buf pa %#llx, smmu bank %d, WM0 cfg %#x addr %#x\n",
        RD(camcc + 0xa014), RD(camcc + 0xa028), (st & (1u << 31)) ? "ack" : "TIMEOUT", st, cam_buf_pa, cam_smmu_bank,
        VFE_RD(0x2208), VFE_RD(0x2214));
    camlog(line);
}

/* after the sensor has streamed a while: bus/WM status, SMMU faults, then stop WM0 and see what landed in the buffer */
static void
vfe0_report(void)
{
    char line[220];
    uint32_t fsr, gfsr, gfsynr1, written = 0, first = ~0u, last = 0;
    uint64_t far;
    const uint8_t *p = (const uint8_t *)cam_buf;

    if (vfe0 == 0 || cam_buf == 0) {
        return;
    }
    hakios_sm7150_smmu_status(&fsr, &far, &gfsr, &gfsynr1);
    snprintf(line, sizeof(line),
        "camera: VFE0 irq st0 %#x st1 %#x | bus st %#x %#x %#x | WM0 st %#x %#x | violation %#x | smmu fsr %#x far %#llx gfsr %#x gfsynr1 %#x\n",
        VFE_RD(0x06c), VFE_RD(0x070), VFE_RD(0x205c), VFE_RD(0x2060), VFE_RD(0x2064), VFE_RD(0x2200), VFE_RD(0x2204),
        VFE_RD(0x07c), fsr, far, gfsr, gfsynr1);
    camlog(line);
    VFE_WR(0x2208, 0);                          /* WM0 off */
    VFE_WR(0x4ac, 1u << 1);
    __asm__ volatile ("dsb sy" ::: "memory");
    delay(100000);
    for (uint32_t o = 0; o < CAM_BUF_SIZE; o += 64) {
        __asm__ volatile ("dc civac, %0" : : "r" ((uint8_t *)cam_buf + o) : "memory");
    }
    __asm__ volatile ("dsb sy" ::: "memory");
    for (uint32_t o = 0; o < CAM_BUF_SIZE; o++) {
        if (p[o] != 0xa5) {
            written++;
            if (first == ~0u) { first = o; }
            last = o;
        }
    }
    snprintf(line, sizeof(line), "camera: FRAME BUFFER - %u of %u bytes written (first %#x, last %#x = %u lines of %u B)\n",
        written, CAM_BUF_SIZE, first, last, last / CAM_LINE_BYTES, CAM_LINE_BYTES);
    camlog(line);
    if (written) {
        uint32_t sum = 0, o0 = first;
        for (uint32_t o = 0; o < 4096 && o0 + o < CAM_BUF_SIZE; o++) { sum += p[o0 + o]; }
        snprintf(line, sizeof(line),
            "camera: FRAME BUFFER - bytes @%#x: %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x; mean of 4 KiB %u\n",
            o0, p[o0], p[o0 + 1], p[o0 + 2], p[o0 + 3], p[o0 + 4], p[o0 + 5], p[o0 + 6], p[o0 + 7],
            p[o0 + 8], p[o0 + 9], p[o0 + 10], p[o0 + 11], p[o0 + 12], p[o0 + 13], p[o0 + 14], p[o0 + 15], sum / 4096);
        camlog(line);
    }
    /* keep the frame: /private/var is the persistent HFS+ data volume, so it can be fetched from a normal boot */
    if (written) {
        vfs_context_t ctx = vfs_context_kernel();
        vnode_t vp = NULLVP;
        int resid = 0, err;

        err = vnode_open("/private/var/root/cam-frame.raw", O_CREAT | O_TRUNC | FWRITE, 0644, 0, &vp, ctx);
        if (err == 0) {
            err = vn_rdwr(UIO_WRITE, vp, (caddr_t)cam_buf, CAM_FRAME_BYTES, 0, UIO_SYSSPACE, IO_SYNC | IO_NOAUTH,
                vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
            (void)VNOP_FSYNC(vp, MNT_WAIT, ctx);
            vnode_close(vp, FWRITE, ctx);
        }
        snprintf(line, sizeof(line), "camera: saved /private/var/root/cam-frame.raw (%u B RAW10 2016x1512, stride 2528): err %d resid %d\n",
            CAM_FRAME_BYTES, err, resid);
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
    {
        uint32_t gain = 0, exp = 0;                    /* cam_gain: analog gain code 0..480 (0 = 1x); cam_exp: lines */
        if (PE_parse_boot_argn("cam_gain", &gain, sizeof(gain)) && gain <= 480) {
            (void)i2c_write_reg16(cci, IMX_ADDR, 0x0204, (uint8_t)(gain >> 8));
            (void)i2c_write_reg16(cci, IMX_ADDR, 0x0205, (uint8_t)gain);
        }
        if (PE_parse_boot_argn("cam_exp", &exp, sizeof(exp)) && exp >= 4 && exp <= 0x0c58) {
            (void)i2c_write_reg16(cci, IMX_ADDR, 0x0202, (uint8_t)(exp >> 8));
            (void)i2c_write_reg16(cci, IMX_ADDR, 0x0203, (uint8_t)exp);
        }
    }
    (void)i2c_write_reg16(cci, IMX_ADDR, 0x0100, 0x01);   /* STREAM ON */
    delay(5000);
    (void)i2c_read_reg16(cci, IMX_ADDR, 0x0100, &s, 1);
    snprintf(line, sizeof(line), "camera: IMX363 STREAM ON (2016x1512, 642 Mbps/lane); mode_select readback=%02x (want 01)\n", s);
    camlog(line);
    csid0_report("+5ms");
    delay(200000);
    csid0_report("+205ms");
    delay(500000);
    csid0_report("+705ms");
    vfe0_report();
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
    if (cam_level >= 5) {
        csiphy0_init();                 /* M2b: CSIPHY0 reset + lane config + settle (arm receive path) */
    }
    if (cam_level >= 6) {
        csid0_init();                   /* M2b: CSID0 RDI0 <- VC0/RAW10, armed before stream-on */
    }
    if (cam_level >= 7) {
        vfe0_init();                    /* M2b: VFE0 RDI0 write master -> DMA into a 2 MiB buffer */
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
