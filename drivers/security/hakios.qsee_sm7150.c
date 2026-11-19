/*
 * The secure world (QSEE, TrustZone) for a program: /dev/hakios-tz, for the fingerprint sensor's trusted app
 * (fpctzappfingerprint, on the vendor image as on Android) loaded and spoken to from userland, the way
 * DenialOfGravity/ios-redfin's phone/qsee (MIT) does it on the Pixel 5 over its /dev/pmem. The same ioctls as that
 * /dev/pmem (phone/gpu/pmem.h), so its code runs on top, but narrower:
 *
 *   PMEM_ALLOC   physically contiguous memory of ours for TZ (zeroed); its physical address back
 *   PMEM_FREE    given back
 *   PMEM_LIST    what is allocated
 *   PMEM_SMC     one SMC, x0..x7 in, x0..x3 out -- ONLY the QSEE calls in the table below (app manager, send data,
 *                listeners, feature version, the SHM bridge), each with the argument layout it must have
 *   read/write   at an offset that is a physical address INSIDE one of our allocations (cache cleaned/invalidated)
 *
 * Nothing else of the machine is reachable through it. Only with boot-arg qsee=1; root only (0600).
 */
#include <kern/locks.h>
#include <kern/clock.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <miscfs/devfs/devfs.h>
#include <pexpert/pexpert.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/ioccom.h>
#include <sys/systm.h>
#include <sys/sysctl.h>
extern void delay(int usec);
#include <sys/uio.h>
#include <vm/vm_kern.h>

extern uint64_t kvtophys(vm_offset_t va);

struct pmem_alloc {
	uint64_t size, align, pa;
};
struct pmem_smc {
	uint64_t x[8];
};
#define PMEM_LIST_MAX   64
struct pmem_list {
	uint32_t count, pad;
	struct {
		uint64_t pa, size;
	} ent[PMEM_LIST_MAX];
};
#define PMEM_ALLOC      _IOWR('P', 1, struct pmem_alloc)
#define PMEM_FREE       _IOW('P', 2, uint64_t)
#define PMEM_SMC        _IOWR('P', 3, struct pmem_smc)
#define PMEM_LIST       _IOR('P', 4, struct pmem_list)

/* The QSEE calls a fingerprint program needs (ios-redfin phone/qsee/qsee_wl.h): function, argument layout */
static const struct { uint64_t x0, x1; const char *name; } allowed[] = {
	{ 0x42000603, 0x01, "TZ_INFO_GET_FEATURE_VERSION" },
	{ 0x72000103, 0x22, "TZ_OS_APP_LOOKUP" },
	{ 0x42000c1e, 0x04, "TZ_SHM_BRIDGE_CREATE" },
	{ 0x42000c1d, 0x01, "TZ_SHM_BRIDGE_DELETE" },
	{ 0x72000101, 0x03, "TZ_OS_APP_START" },
	{ 0x72000102, 0x01, "TZ_OS_APP_SHUTDOWN" },
	{ 0x70000001, 0x885, "TZ_APP_QSAPP_SEND_DATA" },
	{ 0x70000006, 0x8887, "TZ_APP_QSAPP_SEND_DATA_WITH_WHITELIST" },
	{ 0x72000203, 0x02, "TZ_OS_LISTENER_RESPONSE_HANDLER" },
	{ 0x72000204, 0x01, "TZ_OS_CONTINUE_BLOCKED_REQUEST" },
	{ 0x72000207, 0x01, "TZ_OS_CONTINUE_BLOCKED_REQUEST_SMCINVOKE" },
	{ 0x72000206, 0x83, "TZ_OS_REGISTER_LISTENER_SMCINVOKE" },
	{ 0x72000201, 0x83, "TZ_OS_REGISTER_LISTENER" },
	{ 0x72000202, 0x01, "TZ_OS_DEREGISTER_LISTENER" },
	{ 0x72000205, 0x204, "TZ_OS_LISTENER_RESPONSE_HANDLER_WITH_WHITELIST" },
};
#define SCM_INTERRUPTED         1

#define MAX_BUFFERS             PMEM_LIST_MAX
static struct { vm_offset_t va; uint64_t pa, size; } buffers[MAX_BUFFERS];
static LCK_GRP_DECLARE(tz_group, "hakios.qsee");
static LCK_MTX_DECLARE(tz_lock, &tz_group);
static uint32_t tz_level;

static void
tz_cache(vm_offset_t va, uint64_t len)
{
	for (vm_offset_t a = va & ~(vm_offset_t)63; a < va + len; a += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (a) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
}

/* The allocation holding [pa, pa + len), or -1. */
static int
tz_find(uint64_t pa, uint64_t len)
{
	for (int i = 0; i < MAX_BUFFERS; i++) {
		if (buffers[i].size != 0 && pa >= buffers[i].pa && len <= buffers[i].size && pa - buffers[i].pa <= buffers[i].size - len) {
			return i;
		}
	}
	return -1;
}

static int
tz_smc(struct pmem_smc *s)
{
	uint64_t session = 0, function = s->x[0];
	const char *name = NULL;

	for (unsigned i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
		if (allowed[i].x0 == s->x[0]) {
			name = allowed[i].x1 == s->x[1] ? allowed[i].name : NULL;
			break;
		}
	}
	if (name == NULL) {
		printf("qsee: SMC %#llx (args %#llx) refused: not a QSEE call this device makes\n", s->x[0], s->x[1]);
		return EPERM;
	}
	for (int i = 0; i < 1000; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = s->x[1];
		register uint64_t x2 __asm__("x2") = s->x[2];
		register uint64_t x3 __asm__("x3") = s->x[3];
		register uint64_t x4 __asm__("x4") = s->x[4];
		register uint64_t x5 __asm__("x5") = s->x[5];
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			s->x[0] = x0;
			s->x[1] = x1;
			s->x[2] = x2;
			s->x[3] = x3;
			return 0;
		}
	}
	printf("qsee: %s: TZ keeps saying it was interrupted\n", name);
	return EIO;
}

static int
tz_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
tz_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	return 0;
}

static int
tz_rw(struct uio *uio)
{
	uint64_t pa = (uint64_t)uio_offset(uio), len = (uint64_t)uio_resid(uio);
	int i, error;

	lck_mtx_lock(&tz_lock);
	i = tz_find(pa, len);
	if (i < 0 || len == 0) {
		lck_mtx_unlock(&tz_lock);
		return EFAULT;
	}
	tz_cache(buffers[i].va + (pa - buffers[i].pa), len);
	error = uiomove((caddr_t)(buffers[i].va + (pa - buffers[i].pa)), (int)len, uio);
	tz_cache(buffers[i].va + (pa - buffers[i].pa), len);
	lck_mtx_unlock(&tz_lock);
	return error;
}

static int
tz_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	return tz_rw(uio);
}

static int
tz_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	return tz_rw(uio);
}

static int
tz_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	int error = 0;

	lck_mtx_lock(&tz_lock);
	if (cmd == PMEM_ALLOC) {
		struct pmem_alloc *a = (struct pmem_alloc *)data;
		vm_offset_t va = 0;
		uint64_t size = (a->size + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
		int i;

		for (i = 0; i < MAX_BUFFERS && buffers[i].size != 0; i++) {
		}
		if (i == MAX_BUFFERS || size == 0 || size > 16 * 1024 * 1024 || (a->align & (a->align - 1)) != 0) {
			error = i == MAX_BUFFERS ? ENOSPC : EINVAL;
		} else if (kmem_alloc_contig(kernel_map, &va, size, a->align > PAGE_SIZE ? (vm_offset_t)a->align - 1 : PAGE_MASK, 0, 0,
		    KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			error = ENOMEM;
		} else {
			buffers[i].va = va;
			buffers[i].pa = kvtophys(va);
			buffers[i].size = size;
			tz_cache(va, size);
			a->pa = buffers[i].pa;
			a->size = size;
		}
	} else if (cmd == PMEM_FREE) {
		int i = tz_find(*(uint64_t *)data, 1);

		if (i < 0 || buffers[i].pa != *(uint64_t *)data) {
			error = EINVAL;
		} else {
			kmem_free(kernel_map, buffers[i].va, buffers[i].size);
			buffers[i].size = 0;
		}
	} else if (cmd == PMEM_LIST) {
		struct pmem_list *l = (struct pmem_list *)data;

		l->count = 0;
		for (int i = 0; i < MAX_BUFFERS; i++) {
			if (buffers[i].size != 0) {
				l->ent[l->count].pa = buffers[i].pa;
				l->ent[l->count].size = buffers[i].size;
				l->count++;
			}
		}
	} else if (cmd == PMEM_SMC) {
		error = tz_smc((struct pmem_smc *)data);
	} else {
		error = ENOTTY;
	}
	lck_mtx_unlock(&tz_lock);
	return error;
}

static const struct cdevsw tz_cdevsw = {
	.d_open = tz_open, .d_close = tz_close, .d_read = tz_read, .d_write = tz_write,
	.d_ioctl = tz_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

/*
 * The fingerprint sensor's two lines (Android's sm7150-sunfish-fingerprint.dtsi, fp_fpc1020): reset GPIO 91, output,
 * 2 mA, no pull (low: in reset); interrupt GPIO 90, input, 2 mA, pull-down (high: the sensor has something). Both in
 * the TLMM's north tile (0x03400000 + 0x500000, a 4 KiB page per pin: +0 config, +4 in/out). The sensor itself is the
 * secure world's (its SPI belongs to TZ). Only with qsee=1.
 *   kern.hakios.fpc_reset    0/1 sets the reset line (and reads it)
 *   kern.hakios.fpc_irq      the interrupt line now
 *   kern.hakios.fpc_wait     write N ms: waits until the interrupt line is high or N ms pass; read: 1 high, 0 not
 */
#define TLMM_NORTH              0x03900000ULL
#define FPC_RESET_GPIO          91
#define FPC_IRQ_GPIO            90
#define TLMM_CFG(pull, drive_ma, oe)    ((pull) | (0u << 2) | ((((drive_ma) / 2u) - 1u) << 6) | ((oe) ? (1u << 9) : 0u))
static volatile uint32_t *fpc_reset_page, *fpc_irq_page;
static int fpc_waited;

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

static bool
fpc_pins(void)
{
	if (fpc_reset_page == NULL) {
		fpc_reset_page = (volatile uint32_t *)ml_io_map(TLMM_NORTH + 0x1000ULL * FPC_RESET_GPIO, 0x1000);
		fpc_irq_page = (volatile uint32_t *)ml_io_map(TLMM_NORTH + 0x1000ULL * FPC_IRQ_GPIO, 0x1000);
		if (fpc_reset_page == NULL || fpc_irq_page == NULL) {
			return false;
		}
		fpc_irq_page[0] = TLMM_CFG(1, 2, 0);            /* input, pull-down */
		__asm__ volatile ("dsb sy" ::: "memory");
	}
	return true;
}

static int
sysctl_fpc_reset(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value, error;

	if (!tz_level || !fpc_pins()) {
		return ENODEV;
	}
	value = (fpc_reset_page[1] >> 1) & 1;
	error = sysctl_handle_int(oidp, &value, 0, req);
	if (error != 0 || req->newptr == USER_ADDR_NULL) {
		return error;
	}
	fpc_reset_page[1] = value ? 2u : 0u;                    /* the level first, then the pin as an output */
	fpc_reset_page[0] = TLMM_CFG(0, 2, 1);
	__asm__ volatile ("dsb sy" ::: "memory");
	printf("qsee: fingerprint sensor reset line %s\n", value ? "high (running)" : "low (in reset)");
	return 0;
}

static int
sysctl_fpc_irq(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int value;

	if (!tz_level || !fpc_pins()) {
		return ENODEV;
	}
	value = fpc_irq_page[1] & 1;
	return sysctl_handle_int(oidp, &value, 0, req);
}

/* In one call, as ios-redfin's kern.sweet_fpc_wait: the new value is how many ms to wait, the old one 1 or 0. */
static int
sysctl_fpc_wait(__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
{
	int ms = 0, hit = 0, error;

	if (!tz_level || !fpc_pins()) {
		return ENODEV;
	}
	if (req->newptr != USER_ADDR_NULL) {
		if ((error = SYSCTL_IN(req, &ms, sizeof(ms))) != 0) {
			return error;
		}
		for (int t = 0; t <= ms && ms > 0 && ms <= 60000; t += 2) {
			if (fpc_irq_page[1] & 1) {
				hit = 1;
				break;
			}
			delay(2000);
		}
		fpc_waited = hit;
	} else {
		hit = fpc_waited;
	}
	return SYSCTL_OUT(req, &hit, sizeof(hit));
}

SYSCTL_DECL(_kern_hakios);
SYSCTL_PROC(_kern_hakios, OID_AUTO, fpc_reset, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_fpc_reset, "I",
    "the fingerprint sensor's reset line, GPIO 91 (1 = running)");
SYSCTL_PROC(_kern_hakios, OID_AUTO, fpc_irq, CTLTYPE_INT | CTLFLAG_RD | CTLFLAG_LOCKED, NULL, 0, sysctl_fpc_irq, "I",
    "the fingerprint sensor's interrupt line, GPIO 90");
SYSCTL_PROC(_kern_hakios, OID_AUTO, fpc_wait, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED, NULL, 0, sysctl_fpc_wait, "I",
    "write N ms: wait for the interrupt line; read: 1 it came, 0 not");

static thread_call_t node_call;

static void
qsee_make_node(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	int major = cdevsw_add(-1, &tz_cdevsw);

	/* the sensor out of reset, as Android's fpc1020 driver leaves it: low 5 ms, then high */
	if (fpc_pins()) {
		fpc_reset_page[1] = 0;
		fpc_reset_page[0] = TLMM_CFG(0, 2, 1);
		__asm__ volatile ("dsb sy" ::: "memory");
		delay(5000);
		fpc_reset_page[1] = 2;
		__asm__ volatile ("dsb sy" ::: "memory");
		delay(5000);
		printf("qsee: fingerprint sensor reset (GPIO 91 low 5 ms, high); its interrupt line (GPIO 90) reads %u\n",
		    fpc_irq_page[1] & 1);
	}
	printf("qsee: /dev/hakios-tz %s\n", major >= 0 &&
	    devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "hakios-tz") != NULL ? "made" : "CANNOT BE MADE");
}

__startup_func
static void
qsee_init(void)
{
	uint64_t deadline;

	if (!PE_parse_boot_argn("qsee", &tz_level, sizeof(tz_level)) || tz_level == 0) {
		return;
	}
	/* (devfs is not there yet at this stage of the boot: the node a few seconds on, as the other drivers make theirs) */
	node_call = thread_call_allocate(qsee_make_node, NULL);
	clock_interval_to_deadline(5, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(node_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, qsee_init);
