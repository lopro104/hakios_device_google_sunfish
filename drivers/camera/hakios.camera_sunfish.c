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
    /* MCLK0 RCG: source XO (19.2 MHz), div 1 (no PLL), then UPDATE; branch on */
    *(volatile uint32_t *)(camcc + MCLK0_CFG_RCGR) = (*(volatile uint32_t *)(camcc + MCLK0_CFG_RCGR) & ~0x371fu) | 0x01u;
    *(volatile uint32_t *)(camcc + MCLK0_CMD_RCGR) |= RCG_UPDATE;
    __asm__ volatile ("dsb sy" ::: "memory");
    for (int i = 0; i < 100 && (*(volatile uint32_t *)(camcc + MCLK0_CMD_RCGR) & RCG_UPDATE); i++) { delay(100); }
    *(volatile uint32_t *)(camcc + MCLK0_CBCR) |= 1u;
    __asm__ volatile ("dsb sy" ::: "memory");
    /* GPIO 13 to the cam_mclk function (index 1), 4 mA */
    g = pin(MCLK0_GPIO);
    if (g != NULL) {
        g[TLMM_CFG / 4] = (1u << 2) | (1u << 6);     /* function 1, 4 mA, no pull */
        __asm__ volatile ("dsb sy" ::: "memory");
    }
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
    {
        char line[96];
        int drv = i2c_ping(&slg, 0x5a);     /* the DRV2624 haptic, known present on this bus */
        int slgp = i2c_ping(&slg, SLG_ADDR);
        snprintf(line, sizeof(line), "camera: bus check - DRV2624(0x5a) ACK %d, SLG51000(0x75) ACK %d\n", drv, slgp);
        camlog(line);
    }
    camlog("camera: reading the SLG51000\n");
    r = i2c_read_reg16(&slg, SLG_ADDR, SLG_PATTERN_ID_B0, id, 3);
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
