/*
 * The audio DSP (ADSP, a Hexagon inside LPASS) of the SM7150 in the Pixel 4a: started through TrustZone and linked.
 * Milestone 0 of sound and of the sensors (which this phone has behind the same DSP): the DSP runs, says so over SMP2P,
 * and opens its GLINK channels, whose names are written down. Nothing of audio is played yet.
 *
 * Status: steps 1 to 4 RUN ON THE PHONE 2026-11-16 (logs/140814-run.log): started, SMP2P up, six channels opened by the DSP.
 *
 * The way is the modem's on this phone (hakios.mpss_sm7150.c, which runs: PAS load, SMEM, SMP2P, GLINK by polling), with
 * the DSP's own numbers, each from the phone's Android device tree (scratch/android-ref/dt, read off the phone):
 *   qcom,lpass@62400000   qcom,pas-id 1, firmware "adsp" (adsp.mdt, adsp.b02..b30 from the vendor image's /firmware),
 *                         memory-region pil_adsp 0x96b00000 size 0x2200000 (the image is linked at 0x93c00000 and
 *                         relocated: the same span), proxy supplies vdd_lpi_cx / vdd_lpi_mx (RPMh lcx.lvl, lmx.lvl,
 *                         384 = turbo), qcom,signal-aop (the always-on processor is told "adsp" is loaded)
 *   qcom,smp2p-adsp       qcom,smem <443 429>: its item 443, ours 429; remote-pid 2; qcom,ipc bit 26 (0x1a)
 *   qcom,glink/adsp       transport smem, remote-pid 2, mboxes bit 24 (0x18); the modem's are 12 and 14, as that file kicks
 * The other port of this phone (Yofuness/ios-sunfish, MIT; docs/features/audio.md) reports the same facts and that the
 * chain works there from a userland loader; nothing of its code is here.
 *
 * Boot-arg adsp=N does steps 1 to N, then stops the DSP again (unless adsp_keep=1) -- a step says what it is about to
 * do before it does it:
 *   1  reads   the shared memory's partition with the DSP (hosts 0 and 2), what items are in it; the firmware files:
 *              are they there, do they fit the reserved memory
 *   2  writes  our SMP2P item (429) and GLINK's descriptor and our FIFO (478, 479) into that partition -- BEFORE the
 *              DSP runs, so that the two never allocate in it at the same time (this side takes no SMEM lock)
 *   3  asks    RPMh for the rails, TrustZone to take the header, the place, the segments (copied here), and to
 *              authenticate and start the DSP; tells the always-on processor
 *   4  watches SMP2P (its item 443 and what it says) and GLINK (versions both ways, every channel it opens answered
 *              and named) for adsp_watch seconds (default 30)
 *   5  talks   APR on the channel apr_audio_svc, opened from here as well: is the core service's state "up", and what
 *              the audio framework says of its versions
 *   6  runs    the secondary I2S port for adsp_hold seconds (default 3): LPASS's core voted, the bit clock, the port's
 *              format, the port started, LPASS's pins 8 to 11 given to it, the clock pins looked at; then all of it back.
 *              The amplifiers stay in reset. (steps 1 to 6 seen on the phone 2026-11-16)
 *   7  ... and with the port running, the bottom speaker's amplifier (0x41) configured at -102 dB and powered up with
 *              no samples played: its PLL lock and power-up are reported, then it goes back into reset. Silent.
 *              (seen on the phone 2026-11-16: PLL_LOCK and PUP_DONE after 3 ms)
 *   8  A TONE  (instead of 7): a buffer shown to the DSP through the SMMU (stream 0x1b21), a device path (ADM) to the
 *              port, a stream (ASM) routed to it, 1 kHz at -20 dBFS written for adsp_tone seconds (default 2, at most 3);
 *              the amplifier at 0x41, adsp_below decibels down (default 30, 20 at least), on once the DSP takes the
 *              samples and never for more than 3 s; then everything back. AUDIBLE. (HEARD by the user 2026-11-16)
 *   9  SOUND   Darwin's own (/dev/hakios-audio) in place of the tone, until the run ends: the stream kept running, each
 *              period filled from what the audio server writes; the amplifier (adsp_below, default 6 dB down, 0 the
 *              loudest; the analog gain stays at its lowest) on while there is sound, paused after 3 s of silence, powered down for good on an
 *              error bit; a constant level in the samples is replaced by silence. (not yet run)
 * adsp_delay=S: seconds after the kernel's start before step 1 (default 45: the root with the firmware is mounted and
 * the modem, which shares the rails' and the always-on processor's mailboxes, has long started).
 * The important lines also go into the loader's log (the kernel's log loses its middle on a long run).
 */
#include <sys/param.h>
#include <stdarg.h>
#include <sys/systm.h>
#include <sys/errno.h>
#include <sys/fcntl.h>
#include <sys/uio.h>
#include <sys/vnode.h>
#include <sys/kauth.h>
#include <kern/startup.h>
#include <kern/thread_call.h>
#include <kern/thread.h>
#include <kern/clock.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <vm/vm_kern.h>
#include <sys/hakios.audio.h>
#include <sys/conf.h>
#include <miscfs/devfs/devfs.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern int hakios_rpmh_crypto_vote(uint32_t vote);
extern int hakios_rpmh_arc_votes(const char *const *res, const uint32_t *level, unsigned n);
extern int hakios_rpmh_ddr_vote(uint32_t vote, bool send);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_log_putc(char c);
extern kern_return_t thread_terminate(thread_t);
extern bool hakios_sm7150_aop_send(const char *msg);
extern bool hakios_sm7150_smmu_pages(uint32_t sid, uint64_t iova, uint64_t pa, uint32_t pages);     /* hakios.mpss_sm7150.c */
extern bool hakios_amp_prepare(uint8_t address, int vol_db);      /* hakios.cs35l41_sunfish.c */
extern bool hakios_amp_start(void);
extern void hakios_amp_stop(void);
extern bool hakios_amp_pause(void);
extern bool hakios_amp_healthy(void);
extern unsigned hakios_amp_count(void);
extern void hakios_amp_quiesce(void);            /* hakios.mpss_sm7150.c: the one mailbox, the one sender */

#define WIMG_WCOMB              0x6u    /* VM_WIMG_WCOMB: not cacheable | coherent (osfmk/arm/memory_types.h) */
#define WIMG_IO                 0x7u    /* VM_WIMG_IO: guarded, coherent, not cacheable */
#define ADSP_MEMORY             0x96b00000ull
#define ADSP_MEMORY_SIZE        0x2200000u
#define PAS_ADSP                1
#define CHUNK                   0x100000

#define SMP2P_IN                443
#define SMP2P_OUT               429
#define KICK_GLINK              24
#define KICK_SMP2P              26
#define SMEM_HOST_ADSP          2

#define SCM_64                  (1ull << 30)
#define SCM_SIP                 (2ull << 24)
#define SCM_CALL(service, command) ((uint64_t)(service) << 8 | (command))
#define SCM_PAS_INIT_IMAGE      SCM_CALL(2, 1)
#define SCM_PAS_MEM_SETUP       SCM_CALL(2, 2)
#define SCM_PAS_AUTH_AND_RESET  SCM_CALL(2, 5)
#define SCM_PAS_SHUTDOWN        SCM_CALL(2, 6)
#define SCM_ONE_VALUE           1
#define SCM_THREE_VALUES        3
#define SCM_VALUE_AND_BUFFER    (2 | 2 << 6)
#define SCM_INTERRUPTED         1

#define ELF_PT_LOAD             1
#define MDT_TYPE_HASH           (2u << 24)

struct elf32_phdr {
	uint32_t type, offset, vaddr, paddr, filesz, memsz, flags, align;
};

static struct elf32_phdr phdrs[64];
static const char *fw_dir = "/redwood/fw-adsp/";

/* A line into the kernel's log and the loader's. */
static void say(const char *format, ...) __printflike(1, 2);

static bool say_nothing;        /* set on the way back to the loader: printf there panics (console I/O with interrupts off) */

static void
say(const char *format, ...)
{
	char line[256];
	va_list arguments;

	if (say_nothing) {
		return;
	}

	va_start(arguments, format);
	vsnprintf(line, sizeof(line), format, arguments);
	va_end(arguments);
	printf("%s", line);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
}

static void
sleep_ms(uint32_t ms)
{
	delay_for_interval(ms, 1000 * 1000);
}

static int32_t
scm(const char *what, uint64_t function, uint64_t arguments, uint64_t first, uint64_t second, uint64_t third, uint64_t *answer)
{
	uint64_t session = 0;

	for (int i = 0; i < 100; i++) {
		register uint64_t x0 __asm__("x0") = i == 0 ? function : SCM_INTERRUPTED;
		register uint64_t x1 __asm__("x1") = arguments;
		register uint64_t x2 __asm__("x2") = first;
		register uint64_t x3 __asm__("x3") = second;
		register uint64_t x4 __asm__("x4") = third;
		register uint64_t x5 __asm__("x5") = 0;
		register uint64_t x6 __asm__("x6") = session;
		register uint64_t x7 __asm__("x7") = 0;

		__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3), "+r" (x4), "+r" (x5), "+r" (x6),
		    "+r" (x7) : : "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
		session = x6;
		if (x0 != SCM_INTERRUPTED) {
			say("adsp: TrustZone %s: returns %d, answer %#llx\n", what, (int32_t)x0, x1);
			*answer = x1;
			return (int32_t)x0;
		}
	}
	say("adsp: TrustZone %s: keeps saying the call was interrupted\n", what);
	return -1;
}

static int
fw_read(const char *name, off_t offset, void *buffer, int length, off_t *size)
{
	vfs_context_t context = vfs_context_kernel();
	char path[64];
	vnode_t vnode = NULLVP;
	struct vnode_attr attributes;
	int resid = 0, error;

	snprintf(path, sizeof(path), "%s%s", fw_dir, name);
	error = vnode_open(path, FREAD, 0, 0, &vnode, context);
	if (error != 0) {
		return error;
	}
	if (size != NULL) {
		VATTR_INIT(&attributes);
		VATTR_WANTED(&attributes, va_data_size);
		error = vnode_getattr(vnode, &attributes, context);
		*size = (off_t)attributes.va_data_size;
	}
	if (error == 0 && length > 0) {
		error = vn_rdwr(UIO_READ, vnode, (caddr_t)buffer, length, offset, UIO_SYSSPACE, 0, vfs_context_ucred(context),
		    &resid, vfs_context_proc(context));
		if (error == 0 && resid != 0) {
			error = EIO;
		}
	}
	vnode_close(vnode, FREAD, context);
	return error;
}

/* Bytes of the DSP's memory (zeros when data is NULL): a window of at most CHUNK mapped at a time. */
static bool
adsp_write(uint64_t offset, const uint8_t *data, uint64_t length)
{
	while (length != 0) {
		uint64_t phys = ADSP_MEMORY + offset;
		uint64_t base = phys & ~(uint64_t)PAGE_MASK;
		uint64_t n = MIN(length, (uint64_t)CHUNK);
		volatile uint8_t *window;

		n = MIN(n, base + CHUNK - phys);
		window = (volatile uint8_t *)ml_io_map_unmappable((vm_offset_t)base, CHUNK, WIMG_WCOMB);
		if (window == NULL) {
			say("adsp: cannot map %#llx\n", base);
			return false;
		}
		for (uint64_t b = 0; b < n; b++) {
			window[phys - base + b] = data != NULL ? data[b] : 0;
		}
		ml_io_unmap((vm_offset_t)window, CHUNK);
		offset += n;
		length -= n;
		if (data != NULL) {
			data += n;
		}
	}
	return true;
}

static bool
loadable(const struct elf32_phdr *segment)
{
	return segment->type == ELF_PT_LOAD && (segment->flags & (7u << 24)) != MDT_TYPE_HASH && segment->memsz != 0;
}

/* ---- the shared memory (as hakios.mpss_sm7150.c reads it) ---- */

#define SMEM_MEMORY             0x86000000ull
#define SMEM_SIZE               0x200000u

struct smem_ptable_entry {
	uint32_t offset, size, flags;
	uint16_t host0, host1;
	uint32_t cacheline, reserved[7];
};

struct smem_ptable {
	uint8_t magic[4];       /* "$TOC" */
	uint32_t version, num_entries, reserved[5];
	struct smem_ptable_entry entry[];
};

struct smem_partition_header {
	uint8_t magic[4];       /* "$PRT" */
	uint16_t host0, host1;
	uint32_t size, offset_free_uncached, offset_free_cached, reserved[3];
};

struct smem_private_entry {
	uint16_t canary;        /* 0xa5a5 */
	uint16_t item;
	uint32_t size;
	uint16_t padding_data, padding_hdr;
	uint32_t reserved;
};

struct smp2p_smem_item {
	uint32_t magic;         /* "$SMP" */
	uint8_t version;
	uint32_t features : 24;
	uint16_t local_pid, remote_pid, total_entries, valid_entries;
	uint32_t flags;
	struct {
		uint8_t name[16];
		uint32_t value;
	} entries[16];
} __attribute__((packed));

static struct smem_partition_header *part;      /* the partition of hosts 0 and 2, mapped for good */
static uint32_t part_size;
static volatile uint32_t *apcs;

static void
kick(uint32_t bit)
{
	if (apcs != NULL) {
		__asm__ volatile ("dsb sy" ::: "memory");
		apcs[3] = 1u << bit;
		__asm__ volatile ("dsb sy" ::: "memory");
	}
}

static void *
smem_get(uint16_t item, uint32_t *out_size)
{
	uint32_t cur = sizeof(*part);

	while (cur + sizeof(struct smem_private_entry) <= part->offset_free_uncached &&
	    cur + sizeof(struct smem_private_entry) <= part_size) {
		struct smem_private_entry *e = (struct smem_private_entry *)((uint8_t *)part + cur);

		if (e->canary != 0xa5a5) {
			break;
		}
		if (e->item == item) {
			if (out_size != NULL) {
				*out_size = e->size - e->padding_data;
			}
			return (uint8_t *)part + cur + sizeof(*e) + e->padding_hdr;
		}
		cur += sizeof(*e) + e->padding_hdr + e->size;
	}
	return NULL;
}

static void *
smem_alloc(uint16_t item, uint32_t size)
{
	void *existing = smem_get(item, NULL);
	uint32_t at = part->offset_free_uncached, aligned = (size + 7) & ~7u;
	uint32_t total = (uint32_t)sizeof(struct smem_private_entry) + aligned;
	struct smem_private_entry *e;

	if (existing != NULL) {
		return existing;
	}
	if (at + total > part->offset_free_cached || at + total > part_size) {
		say("adsp: no room in the partition for item %u (%u bytes)\n", item, size);
		return NULL;
	}
	e = (struct smem_private_entry *)((uint8_t *)part + at);
	e->canary = 0xa5a5;
	e->item = item;
	e->size = aligned;
	e->padding_data = (uint16_t)(aligned - size);
	e->padding_hdr = 0;
	e->reserved = 0;
	memset(e + 1, 0, aligned);
	__asm__ volatile ("dsb sy" ::: "memory");
	part->offset_free_uncached = at + total;
	__asm__ volatile ("dsb sy" ::: "memory");
	say("adsp: item %u (%u bytes) made at %#x of the partition\n", item, size, at);
	return e + 1;
}

static bool
smem_map(void)
{
	volatile uint8_t *table_window = (volatile uint8_t *)ml_io_map_unmappable(SMEM_MEMORY + SMEM_SIZE - PAGE_SIZE, PAGE_SIZE,
	    WIMG_WCOMB);
	struct smem_ptable *table = NULL;
	uint64_t phys = 0, base, map_size;
	volatile uint8_t *window;

	if (table_window == NULL) {
		return false;
	}
	if (memcmp((const void *)(table_window + (PAGE_SIZE - 4096)), "$TOC", 4) == 0) {
		table = (struct smem_ptable *)(table_window + (PAGE_SIZE - 4096));
	}
	for (uint32_t i = 0; table != NULL && i < table->num_entries && i < 32; i++) {
		const struct smem_ptable_entry *e = &table->entry[i];

		if (e->size != 0 && ((e->host0 == 0 && e->host1 == SMEM_HOST_ADSP) || (e->host0 == SMEM_HOST_ADSP && e->host1 == 0))) {
			phys = SMEM_MEMORY + e->offset;
			part_size = e->size;
		}
	}
	ml_io_unmap((vm_offset_t)table_window, PAGE_SIZE);
	if (phys == 0) {
		say("adsp: the shared memory has no partition for hosts 0 and %u\n", SMEM_HOST_ADSP);
		return false;
	}
	base = phys & ~(uint64_t)PAGE_MASK;
	map_size = (phys - base + part_size + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
	if (map_size > CHUNK) {
		say("adsp: the partition is %#x bytes: more than this maps (%#x)\n", part_size, CHUNK);
		return false;
	}
	window = (volatile uint8_t *)ml_io_map_unmappable(base, map_size, WIMG_WCOMB);
	apcs = (volatile uint32_t *)ml_io_map_unmappable(0x17c00000, PAGE_SIZE, WIMG_IO);
	if (window == NULL || apcs == NULL) {
		return false;
	}
	part = (struct smem_partition_header *)(window + (phys - base));
	if (memcmp(part->magic, "$PRT", 4) != 0) {
		say("adsp: the partition at %#llx does not begin with $PRT\n", phys);
		part = NULL;
		return false;
	}
	say("adsp: partition of hosts %u and %u at %#llx, %#x bytes, uncached to %#x, cached from %#x\n", part->host0,
	    part->host1, phys, part_size, part->offset_free_uncached, part->offset_free_cached);
	return true;
}

/* ---- step 1: what is there ---- */

static uint64_t image_low = ~0ull, image_high;
static uint32_t segments;
static uint8_t *mdt;
static off_t mdt_size;

static bool
step_look(void)
{
	static const uint16_t items[] = { SMP2P_IN, SMP2P_OUT, 478, 479, 480 };
	vm_offset_t page = 0;
	int error = 0;

	if (!smem_map()) {
		return false;
	}
	for (unsigned i = 0; i < sizeof(items) / sizeof(items[0]); i++) {
		uint32_t size = 0;
		void *p = smem_get(items[i], &size);

		say("adsp: item %u: %s (%u bytes)\n", items[i], p != NULL ? "there" : "not there", size);
	}
	/* the header goes to TrustZone by its physical address: one run of pages */
	if (kmem_alloc_contig(kernel_map, &page, 4 * PAGE_SIZE, PAGE_MASK, 0, 0, KMA_DATA | KMA_ZERO, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	mdt = (uint8_t *)page;
	for (int tries = 0; tries < 30; tries++) {
		error = fw_read("adsp.mdt", 0, NULL, 0, &mdt_size);
		if (error != 0 && tries == 0) {
			fw_dir = "/private/preboot/redwood/fw-adsp/";
		} else if (error == 0) {
			break;
		}
		sleep_ms(1000);
	}
	if (error != 0 || mdt_size < 52 + 32 * 2 || mdt_size > 3 * PAGE_SIZE || fw_read("adsp.mdt", 0, mdt, (int)mdt_size, NULL) != 0) {
		say("adsp: no adsp.mdt in %s (error %d, %lld bytes): build with FW_ADSP=<the vendor image's firmware directory>\n",
		    fw_dir, error, (long long)mdt_size);
		return false;
	}
	segments = mdt[44] | mdt[45] << 8;
	if (memcmp(mdt, "\177ELF\1", 5) != 0 || segments < 2 || segments > 64 || mdt_size < 52 + 32 * segments) {
		say("adsp: adsp.mdt is not an ELF32 header with segments (%u)\n", segments);
		return false;
	}
	for (uint32_t i = 0; i < segments; i++) {
		memcpy(&phdrs[i], mdt + 52 + 32 * i, sizeof(struct elf32_phdr));
		if (loadable(&phdrs[i])) {
			char name[24];
			off_t size = 0;

			image_low = MIN(image_low, phdrs[i].paddr);
			image_high = MAX(image_high, (uint64_t)phdrs[i].paddr + phdrs[i].memsz);
			snprintf(name, sizeof(name), "adsp.b%02u", i);
			if (phdrs[i].filesz != 0 && (fw_read(name, 0, NULL, 0, &size) != 0 || size < phdrs[i].filesz)) {
				say("adsp: %s missing or short (%lld bytes, the header says %u)\n", name, (long long)size, phdrs[i].filesz);
				return false;
			}
		}
	}
	say("adsp: firmware in %s: %u segments, %#llx..%#llx (the reserved memory: %#x at %#llx)\n", fw_dir, segments,
	    image_low, image_high, ADSP_MEMORY_SIZE, ADSP_MEMORY);
	return image_high > image_low && image_high - image_low <= ADSP_MEMORY_SIZE;
}

/* ---- step 2: our side of SMP2P and GLINK, before the DSP runs ---- */

static volatile uint32_t *descs;        /* item 478: [0] our tail, [1] our head, [2] its tail (ours to move), [3] its head */
static volatile uint8_t *tx_fifo, *rx_fifo;
#define FIFO_SIZE 16384u

static bool
step_ours(void)
{
	struct smp2p_smem_item *out = smem_alloc(SMP2P_OUT, sizeof(struct smp2p_smem_item));

	descs = smem_alloc(478, 32);
	tx_fifo = smem_alloc(479, FIFO_SIZE);
	if (out == NULL || descs == NULL || tx_fifo == NULL) {
		return false;
	}
	out->magic = 0x504d5324;
	out->local_pid = 0;
	out->remote_pid = SMEM_HOST_ADSP;
	out->total_entries = 16;
	out->valid_entries = 2;
	out->features = 1;              /* SSR ack */
	memcpy(out->entries[0].name, "master-kernel", 14);
	memcpy(out->entries[1].name, "sleepstate", 11);
	__asm__ volatile ("dsb sy" ::: "memory");
	out->version = 1;
	descs[1] = 0;
	descs[2] = 0;
	__asm__ volatile ("dsb sy" ::: "memory");
	say("adsp: our SMP2P item (%u), GLINK's descriptor and our FIFO are in the partition\n", SMP2P_OUT);
	return true;
}

/* ---- step 3: the DSP loaded and started ---- */

static bool dsp_started;

static void
adsp_shutdown(const char *why)
{
	uint64_t answer = 0;

	if (dsp_started) {
		say("adsp: about to ask TrustZone to shut the DSP down (%s)\n", why);
		(void)scm("shutdown", SCM_64 | SCM_SIP | SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer);
		(void)hakios_sm7150_aop_send("{class: image, res: load_state, name: adsp, val: off}");
		dsp_started = false;
	}
}

static bool
step_start(void)
{
	static const char *const rails[2] = { "lcx.lvl", "lmx.lvl" };
	static const uint32_t levels[2] = { 384, 384 };
	vm_offset_t chunk = 0;
	uint64_t answer = ~0ull;
	uint32_t loaded = 0;
	int error = EBUSY;

	if (kmem_alloc(kernel_map, &chunk, CHUNK, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
		return false;
	}
	say("adsp: about to vote the rails lcx and lmx (384) and the crypto engine's path\n");
	for (int tries = 0; tries < 20 && error == EBUSY; tries++) {      /* (EBUSY: the modem's thread is in RPMh this moment) */
		error = hakios_rpmh_arc_votes(rails, levels, 2);
		if (error == EBUSY) {
			sleep_ms(50);
		}
	}
	say("adsp: rail votes %d, crypto vote %d, memory vote %d\n", error, hakios_rpmh_crypto_vote(0x3fff),
	    hakios_rpmh_ddr_vote(0x3fff, true));
	say("adsp: the always-on processor told the image is loading: %s\n",
	    hakios_sm7150_aop_send("{class: image, res: load_state, name: adsp, val: on}") ? "acknowledged" : "NOT acknowledged");

	say("adsp: about to ask TrustZone to give up an image it may hold, then to take the header and the place\n");
	(void)scm("give up an old image", SCM_64 | SCM_SIP | SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer);
	if (scm("the header", SCM_64 | SCM_SIP | SCM_PAS_INIT_IMAGE, SCM_VALUE_AND_BUFFER, PAS_ADSP, kvtophys((vm_offset_t)mdt), 0,
	    &answer) != 0 || answer != 0) {
		say("adsp: TrustZone does not take the header\n");
		return false;
	}
	if (scm("the place", SCM_64 | SCM_SIP | SCM_PAS_MEM_SETUP, SCM_THREE_VALUES, PAS_ADSP, ADSP_MEMORY, image_high - image_low,
	    &answer) != 0 || answer != 0) {
		say("adsp: TrustZone does not take the place\n");
		return false;
	}
	say("adsp: about to copy the segments to %#llx\n", ADSP_MEMORY);
	for (uint32_t i = 0; i < segments; i++) {
		const struct elf32_phdr *segment = &phdrs[i];
		char name[24];
		off_t done = 0;

		if (!loadable(segment)) {
			continue;
		}
		snprintf(name, sizeof(name), "adsp.b%02u", i);
		while (done < segment->filesz) {
			int n = (int)MIN((off_t)CHUNK, (off_t)segment->filesz - done);

			if (fw_read(name, done, (void *)chunk, n, NULL) != 0 ||
			    !adsp_write(segment->paddr - image_low + done, (uint8_t *)chunk, (uint64_t)n)) {
				say("adsp: %s could not be copied at %lld\n", name, (long long)done);
				return false;
			}
			done += n;
		}
		if (segment->memsz > segment->filesz && !adsp_write(segment->paddr - image_low + segment->filesz, NULL,
		    segment->memsz - segment->filesz)) {
			return false;
		}
		loaded++;
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	say("adsp: %u segments copied; about to ask TrustZone to authenticate and start the DSP\n", loaded);
	if (scm("authenticate and start", SCM_64 | SCM_SIP | SCM_PAS_AUTH_AND_RESET, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer) != 0 ||
	    answer != 0) {
		say("adsp: TrustZone does not authenticate/start the DSP\n");
		return false;
	}
	dsp_started = true;
	say("adsp: TRUSTZONE HAS AUTHENTICATED AND STARTED THE AUDIO DSP\n");
	kick(KICK_SMP2P);
	return true;
}

/* ---- step 4: what it says ---- */

struct glink_msg {
	uint16_t cmd, param1;
	uint32_t param2;
} __attribute__((packed));

static void
rx_peek(uint32_t tail, void *dst, size_t count)
{
	size_t len = MIN(count, (size_t)(FIFO_SIZE - tail));

	memcpy(dst, (const void *)(rx_fifo + tail), len);
	if (len < count) {
		memcpy((uint8_t *)dst + len, (const void *)rx_fifo, count - len);
	}
}

static void
rx_advance(size_t count)
{
	descs[2] = (descs[2] + (uint32_t)count) % FIFO_SIZE;
	__asm__ volatile ("dsb sy" ::: "memory");
}

static bool
tx(const void *data, size_t len)
{
	uint32_t head = descs[1], tail = descs[0];
	uint32_t room = tail <= head ? FIFO_SIZE - head + tail : tail - head;
	size_t aligned = (len + 7) & ~(size_t)7;
	uint8_t padded[256];

	if (aligned > sizeof(padded) || room < aligned + 16) {
		return false;
	}
	memset(padded, 0, sizeof(padded));
	memcpy(padded, data, len);
	for (size_t i = 0; i < aligned; i++) {
		tx_fifo[(head + i) % FIFO_SIZE] = padded[i];
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	descs[1] = (head + (uint32_t)aligned) % FIFO_SIZE;
	kick(KICK_GLINK);
	return true;
}

static uint32_t channels;

/*
 * Step 5: the one channel spoken on, "apr_audio_svc" (Linux's apr.c: APR packets as GLINK data). Our id for it is the
 * DSP's own (so the two conventions for whose id a data message carries agree). Its receive buffers ("intents") are what
 * we may send into; ours (one, 4096 bytes, reused) is what it sends into.
 */
static struct {
	bool remote_open, open_sent, acked, intent_asked;
	uint16_t id;
	uint32_t intent_id, intent_size;         /* one of the DSP's intents, 0: none yet */
	uint32_t received;
	uint32_t last_token, last_opcode, last_words[8], last_payload;      /* the newest packet received */
	bool intent_busy;                        /* sent into, and the DSP has not said it is done with it */
	uint32_t written_done, write_errors, underflows;     /* the stream's events (step 8) */
	uint32_t done_periods;                   /* bit p: period p's buffer is ours again */
	uint32_t read_done, read_errors, read_bytes[8];  /* the capture stream's events (adsp=10): bit p, buffer p is filled */
} apr;

#define APR_HDR_SIZE        20
#define APR_SVC_CORE        0x03
#define APR_DOMAIN_ADSP     0x04
#define APR_DOMAIN_APPS     0x05
#define AVCS_GET_STATE      0x0001290Cu      /* answered by 0x0001290D: one word, 1 = the audio services are up */
#define AVCS_GET_FWK        0x0001292Cu      /* answered by 0x0001292D: the framework's versions and its services */

static bool
apr_send(uint32_t opcode, uint32_t token)
{
	struct {
		struct glink_msg msg;           /* TX_DATA: param1 our channel id, param2 the intent used */
		uint32_t chunk, left;
		uint8_t apr[APR_HDR_SIZE];
	} __attribute__((packed)) packet;

	if (apr.intent_id == 0 && apr.intent_size == 0) {
		return false;
	}
	memset(&packet, 0, sizeof(packet));
	packet.msg.cmd = 9;
	packet.msg.param1 = apr.id;
	packet.msg.param2 = apr.intent_id;
	packet.chunk = APR_HDR_SIZE;
	packet.apr[0] = 0x50;                   /* version 0, header of 5 words */
	packet.apr[1] = 0x02;                   /* a command that is answered */
	packet.apr[2] = APR_HDR_SIZE;           /* the packet's size, 16 bits */
	packet.apr[4] = APR_SVC_CORE;           /* from: service, domain, port (16 bits) */
	packet.apr[5] = APR_DOMAIN_APPS;
	packet.apr[8] = APR_SVC_CORE;           /* to */
	packet.apr[9] = APR_DOMAIN_ADSP;
	memcpy(&packet.apr[12], &token, 4);
	memcpy(&packet.apr[16], &opcode, 4);
	say("adsp: APR: sending opcode %#x (token %u) into the DSP's intent %u\n", opcode, token, apr.intent_id);
	return tx(&packet, sizeof(packet));
}

static void
apr_received(const uint8_t *p, uint32_t len)
{
	uint32_t opcode = 0, token = 0, words[8] = { 0 }, header = len >= 1 ? (uint32_t)(p[0] >> 4) * 4 : 0;

	if (len < APR_HDR_SIZE || header < APR_HDR_SIZE || header > len) {
		say("adsp: APR: %u bytes with a header of %u: not a packet\n", len, header);
		return;
	}
	memcpy(&token, p + 12, 4);
	memcpy(&opcode, p + 16, 4);
	memcpy(words, p + header, MIN(len - header, (uint32_t)sizeof(words)));
	apr.received++;
	if (opcode == 0x00010D99u && (token & 0x80000000u)) {      /* a period was played: buffer address (2 words), handle, status */
		apr.written_done++;
		apr.write_errors += words[3] != 0;
		apr.done_periods |= 1u << (token & 7u);
		return;
	}
	if (opcode == 0x00010D9Au && (token & 0x40000000u)) {      /* a capture buffer filled: status, address (2), handle, size */
		apr.read_errors += words[0] != 0;
		apr.read_bytes[token & 7u] = words[4];
		apr.read_done |= 1u << (token & 7u);
		return;
	}
	if (opcode == 0x00010C17u) {                              /* the stream ran dry */
		apr.underflows++;
		return;
	}
	if (opcode == 0x000110E8u && (token & 0x80000000u)) {      /* a write refused outright */
		apr.write_errors++;
		apr.done_periods |= 1u << (token & 7u);
		say("adsp: APR: the write of period %u was refused (status %#x)\n", token & 7u, words[1]);
		return;
	}
	apr.last_token = token;
	apr.last_opcode = opcode;
	apr.last_payload = len - header;
	memcpy(apr.last_words, words, sizeof(apr.last_words));
	/* (the header's length is the packet's own to say: this DSP answers with 8 words of header, not the 5 we send) */
	say("adsp: APR: RECEIVED opcode %#x token %u, header %u bytes (first word %#x), %u bytes of payload: %#x %#x %#x %#x %#x %#x %#x %#x\n",
	    opcode, token, header, (uint32_t)(p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24), len - header, words[0], words[1],
	    words[2], words[3], words[4], words[5], words[6], words[7]);
	if (opcode == 0x0001290D) {
		say("adsp: APR: THE AUDIO SERVICES ARE %s\n", words[0] == 1 ? "UP" : "not up yet");
	}
}

static void
glink_poll(void)
{
	static uint8_t data[512];

	for (int iter = 0; iter < 32; iter++) {
		uint32_t head = descs[3], tail = descs[2];
		uint32_t have = head >= tail ? head - tail : FIFO_SIZE - tail + head;
		struct glink_msg msg;

		if (head >= FIFO_SIZE || tail >= FIFO_SIZE || have < sizeof(msg)) {
			return;
		}
		rx_peek(tail, &msg, sizeof(msg));
		if (msg.cmd == 0) {                             /* VERSION */
			struct glink_msg ack = { .cmd = 1, .param1 = 1, .param2 = msg.param2 & 1 };

			say("adsp: GLINK: its version %u, features %#x; acknowledged\n", msg.param1, msg.param2);
			rx_advance(sizeof(msg));
			(void)tx(&ack, sizeof(ack));
		} else if (msg.cmd == 1) {                      /* VERSION_ACK */
			say("adsp: GLINK: it acknowledges our version (%u, features %#x)\n", msg.param1, msg.param2);
			rx_advance(sizeof(msg));
		} else if (msg.cmd == 2) {                      /* OPEN: param1 its channel id, param2 the name's length */
			uint32_t name_len = msg.param2 & 0xffff, packet = ((uint32_t)sizeof(msg) + name_len + 7) & ~7u;
			struct glink_msg ack = { .cmd = 4, .param1 = msg.param1, .param2 = 0 };
			char name[40] = { 0 };

			if (have < packet) {
				return;
			}
			rx_peek((tail + (uint32_t)sizeof(msg)) % FIFO_SIZE, name, MIN(name_len, (uint32_t)sizeof(name) - 1));
			rx_advance(packet);
			channels++;
			say("adsp: GLINK: THE DSP OPENS CHANNEL \"%s\" (its id %u); acknowledged\n", name, msg.param1);
			(void)tx(&ack, sizeof(ack));
			if (strcmp(name, "apr_audio_svc") == 0) {
				apr.remote_open = true;
				apr.id = msg.param1;
			}
		} else if (msg.cmd == 4) {                      /* OPEN_ACK: param1 our id */
			rx_advance(sizeof(msg));
			if (apr.open_sent && msg.param1 == apr.id) {
				apr.acked = true;
				say("adsp: GLINK: the DSP acknowledges our opening of apr_audio_svc\n");
			}
		} else if (msg.cmd == 5) {                      /* INTENT: param1 its channel id, param2 how many; then (size, id) each */
			uint32_t count = msg.param2, packet = ((uint32_t)sizeof(msg) + count * 8 + 7) & ~7u;

			if (count > 32) {
				say("adsp: GLINK: %u intents at once: the queue is out of step; dropped\n", count);
				rx_advance(have);
				return;
			}
			if (have < packet) {
				return;
			}
			for (uint32_t i = 0; i < count; i++) {
				uint32_t pair[2];

				rx_peek((tail + (uint32_t)sizeof(msg) + i * 8) % FIFO_SIZE, pair, sizeof(pair));
				if (msg.param1 == apr.id && apr.remote_open && pair[0] >= 64 && apr.intent_size == 0) {
					apr.intent_size = pair[0];
					apr.intent_id = pair[1];
					say("adsp: GLINK: the DSP offers an intent of %u bytes (id %u) on apr_audio_svc\n", pair[0], pair[1]);
				}
			}
			rx_advance(packet);
		} else if (msg.cmd == 9 || msg.cmd == 12) {     /* TX_DATA: param1 its channel id, param2 our intent; chunk, left; data */
			uint32_t sizes[2], packet;

			if (have < sizeof(msg) + 8) {
				return;
			}
			rx_peek((tail + (uint32_t)sizeof(msg)) % FIFO_SIZE, sizes, sizeof(sizes));
			packet = ((uint32_t)sizeof(msg) + 8 + sizes[0] + 7) & ~7u;
			if (sizes[0] > FIFO_SIZE / 2) {
				say("adsp: GLINK: data of %u bytes: the queue is out of step; dropped\n", sizes[0]);
				rx_advance(have);
				return;
			}
			if (have < packet) {
				return;
			}
			if (msg.param1 == apr.id && apr.remote_open) {
				struct glink_msg done = { .cmd = 14, .param1 = apr.id, .param2 = msg.param2 };    /* RX_DONE, the intent kept */
				uint32_t n = MIN(sizes[0], (uint32_t)sizeof(data));

				rx_peek((tail + (uint32_t)sizeof(msg) + 8) % FIFO_SIZE, data, n);
				rx_advance(packet);
				apr_received(data, n);
				(void)tx(&done, sizeof(done));
			} else {
				rx_advance(packet);
				say("adsp: GLINK: %u bytes of data on its channel %u: not ours, let go\n", sizes[0], msg.param1);
			}
		} else if (msg.cmd == 7) {                      /* RX_INTENT_REQ: param1 its channel id, param2 the size it wants of us */
			struct glink_msg ack = { .cmd = 8, .param1 = msg.param1, .param2 = msg.param1 == apr.id ? 1 : 0 };

			rx_advance(sizeof(msg));
			(void)tx(&ack, sizeof(ack));
		} else if (msg.cmd == 6 || msg.cmd == 14 || msg.cmd == 8 || msg.cmd == 15 || msg.cmd == 13 || msg.cmd == 3 || msg.cmd == 11) {
			/* done with what we sent (the intent is ours to use again), an intent request's answer, signals, notices, closes */
			if (msg.cmd == 8) {
				say("adsp: GLINK: our request for an intent is %s\n", msg.param2 ? "granted" : "REFUSED");
			}
			if ((msg.cmd == 6 || msg.cmd == 14) && msg.param1 == apr.id) {
				apr.intent_busy = false;
			}
			if (msg.cmd == 6 && msg.param1 == apr.id) {     /* done WITHOUT reuse: that intent is gone, the next offer is taken */
				apr.intent_size = 0;
				apr.intent_asked = false;
			}
			rx_advance(sizeof(msg));
		} else {
			say("adsp: GLINK: command %u (%u, %#x) with %u bytes waiting: unknown, queue dropped\n", msg.cmd, msg.param1,
			    msg.param2, have);
			rx_advance(have);
		}
	}
}

/* Our half of apr_audio_svc, one thing a pass; then the two questions. */
static void
apr_step(uint32_t steps)
{
	static uint32_t asked, waited;

	if (steps < 5 || !apr.remote_open) {
		return;
	}
	if (!apr.open_sent) {
		struct {
			struct glink_msg msg;
			char name[16];
		} __attribute__((packed)) open;
		struct {
			struct glink_msg msg;           /* INTENT: param1 our channel id, param2 one of them */
			uint32_t size, id;
		} __attribute__((packed)) intent = { { 5, apr.id, 1 }, 4096, 1 };

		memset(&open, 0, sizeof(open));
		open.msg.cmd = 2;
		open.msg.param1 = apr.id;
		open.msg.param2 = sizeof("apr_audio_svc");
		memcpy(open.name, "apr_audio_svc", sizeof("apr_audio_svc"));
		apr.open_sent = tx(&open, sizeof(open)) && tx(&intent, sizeof(intent));
		say("adsp: GLINK: apr_audio_svc opened from here as channel %u, one intent of 4096 bytes offered: %s\n", apr.id,
		    apr.open_sent ? "sent" : "NOT sent");
		return;
	}
	if (!apr.acked) {
		return;
	}
	if (apr.intent_size == 0) {
		if (!apr.intent_asked && ++waited > 10) {       /* half a second without an offer: ask for one */
			struct glink_msg ask = { .cmd = 7, .param1 = apr.id, .param2 = 256 };

			apr.intent_asked = tx(&ask, sizeof(ask));
			say("adsp: GLINK: no intent offered on apr_audio_svc; one of 256 bytes asked for\n");
		}
		return;
	}
	if (asked == 0) {
		asked = apr_send(AVCS_GET_STATE, 1) ? 1 : 0;
	} else if (asked == 1 && apr.received >= 1) {
		asked = apr_send(AVCS_GET_FWK, 2) ? 2 : 1;
	}
}

/* ---- step 6: the port the amplifiers listen on, running; nothing played, the amplifiers left in reset ---- */

#define APR_SVC_AFE             0x04
#define APR_BASIC_RESULT        0x000110E8u     /* payload: the opcode answered, its status (0 = done) */
#define AFE_SVC_SET_PARAM       0x000100F3u
#define AFE_PORT_SET_PARAM_V2   0x000100EFu
#define AFE_PORT_START          0x000100E5u
#define AFE_PORT_STOP           0x000100E6u
#define AFE_HW_VOTE             0x000100F4u     /* answered by 0x000100F5: a handle */
#define AFE_HW_DEVOTE           0x000100F6u
#define AFE_PORT_SEC_MI2S_RX    0x1002
#define AFE_CLK_SEC_MI2S_IBIT   0x102
#define LPI_PA                  0x62b40000ul    /* LPASS's own pin controller: gpio n at + 0x1000 * n */

/* One APR packet sent with `token' (the DSP's one intent is waited for: one packet in it at a time). */
static bool
apr_post(uint8_t service, uint16_t port, uint32_t token, uint32_t opcode, const void *payload, uint16_t len)
{
	struct {
		struct glink_msg msg;
		uint32_t chunk, left;
		uint8_t apr[APR_HDR_SIZE + 96];
	} __attribute__((packed)) packet;
	uint16_t size = (uint16_t)(APR_HDR_SIZE + len);

	if (len > 96) {
		return false;
	}
	for (int waited = 0; (apr.intent_size == 0 || apr.intent_busy) && waited < 400; waited++) {
		if (apr.intent_size == 0 && !apr.intent_asked && waited == 20) {
			struct glink_msg ask = { .cmd = 7, .param1 = apr.id, .param2 = 256 };

			apr.intent_asked = tx(&ask, sizeof(ask));
		}
		sleep_ms(1);
		glink_poll();
	}
	if (apr.intent_size == 0 || apr.intent_busy || size > apr.intent_size) {
		say("adsp: APR: no intent of the DSP's free for %u bytes (it offers %u, %s)\n", size, apr.intent_size,
		    apr.intent_busy ? "still in use" : "not in use");
		return false;
	}
	memset(&packet, 0, sizeof(packet));
	packet.msg.cmd = 9;
	packet.msg.param1 = apr.id;
	packet.msg.param2 = apr.intent_id;
	packet.chunk = size;
	packet.apr[0] = 0x50;
	packet.apr[1] = 0x02;
	memcpy(&packet.apr[2], &size, 2);
	packet.apr[4] = service;
	packet.apr[5] = APR_DOMAIN_APPS;
	if (service == 0x07) {                  /* a stream's packets come from its own port, as they go to it */
		memcpy(&packet.apr[6], &port, 2);
	}
	packet.apr[8] = service;
	packet.apr[9] = APR_DOMAIN_ADSP;
	memcpy(&packet.apr[10], &port, 2);
	memcpy(&packet.apr[12], &token, 4);
	memcpy(&packet.apr[16], &opcode, 4);
	if (len != 0) {
		memcpy(&packet.apr[APR_HDR_SIZE], payload, len);
	}
	apr.intent_busy = true;
	if (!tx(&packet, 16 + (size_t)size)) {
		apr.intent_busy = false;
		say("adsp: APR: opcode %#x could not be put into the FIFO\n", opcode);
		return false;
	}
	return true;
}

/* One APR command and its answer (3 s at most); the answer is in apr.last_*. */
static bool
apr_call(uint8_t service, uint16_t port, uint32_t opcode, const void *payload, uint16_t len)
{
	static uint32_t token = 0x100;

	token++;
	apr.last_token = 0;
	if (!apr_post(service, port, token, opcode, payload, len)) {
		return false;
	}
	for (int waited = 0; waited < 3000; waited++) {
		sleep_ms(1);
		glink_poll();
		if (apr.last_token == token) {
			return true;
		}
	}
	say("adsp: APR: NO ANSWER to opcode %#x (service %u, port %#x) in 3 s\n", opcode, service, port);
	return false;
}

/* ... and one that is answered by the basic result: true when its status is 0. */
static bool
afe_do(const char *what, uint16_t port, uint32_t opcode, const void *payload, uint16_t len)
{
	bool answered = apr_call(APR_SVC_AFE, port, opcode, payload, len);
	bool ok = answered && apr.last_opcode == APR_BASIC_RESULT && apr.last_words[0] == opcode && apr.last_words[1] == 0;

	say("adsp: AFE: %s: %s (answer %#x: %#x, status %#x)\n", what, ok ? "done" : answered ? "REFUSED" : "no answer",
	    apr.last_opcode, apr.last_words[0], apr.last_words[1]);
	return ok;
}

static bool
afe_clock(bool enable)
{
	/* in band: the payload's size, then no address and no memory handle; module, parameter, size; the clock */
	uint32_t body[12] = { 32, 0, 0, 0, 0x0001028F, 0x00010290, 20, 1 /* version */, AFE_CLK_SEC_MI2S_IBIT,
		              48000 * 16 * 2, 1 /* attribute 1 (couple no), root 0 */, enable };

	return afe_do(enable ? "the bit clock of the secondary I2S, 1.536 MHz, on" : "the bit clock off", 0, AFE_SVC_SET_PARAM, body,
	    sizeof(body));
}

/* ---- step 8: a tone. The first thing here that can be heard. ---- */

#define APR_SVC_ASM             0x07
#define APR_SVC_ADM             0x08
#define ASM_MAP                 0x00010D92u     /* answered by 0x00010D93: the handle */
#define ASM_UNMAP               0x00010D94u
#define ASM_OPEN_WRITE          0x00010DB3u
#define ASM_FORMAT              0x00010D98u
#define ASM_RUN                 0x00010DAAu
#define ASM_PAUSE               0x00010BD3u
#define ASM_WRITE               0x00010DABu     /* its event when played: 0x00010D99 */
#define ASM_CLOSE               0x00010BCDu
#define ADM_OPEN                0x00010326u     /* answered by 0x00010329: status, the device path's id */
#define ADM_CLOSE               0x00010327u
#define ADM_ROUTE               0x00010325u
#define STREAM_PORT             0x0101          /* session 1, stream 1 */
#define PERIODS                 8
#define PERIOD_FRAMES           480             /* 10 ms at 48 kHz */
#define PERIOD_BYTES            (PERIOD_FRAMES * 4)
#define PERIOD_STRIDE           0x1000
#define TONE_SID                0x1b21          /* the audio stream of the application processors' SMMU (qcom,msm-audio-ion) */
#define TONE_IOVA               0xa0000000ull
#define TONE_DSP_ADDRESS        (TONE_IOVA | 1ull << 32)        /* the stream's low four bits in bits 32 to 35 */

static bool
basic_ok(const char *what, uint8_t service, uint16_t port, uint32_t opcode, const void *payload, uint16_t len)
{
	bool answered = apr_call(service, port, opcode, payload, len);
	bool ok = answered && apr.last_opcode == APR_BASIC_RESULT && apr.last_words[0] == opcode && apr.last_words[1] == 0;

	say("adsp: stream: %s: %s (answer %#x: %#x, status %#x)\n", what, ok ? "done" : answered ? "REFUSED" : "no answer",
	    apr.last_opcode, apr.last_words[0], apr.last_words[1]);
	return ok;
}

static int16_t *tone_buffer;
static uint32_t tone_at, tone_handle, tone_sent;

/*
 * Step 9: what Darwin plays (/dev/hakios-audio, hakios.audio.c) in place of the tone. The audio server's thread
 * offers frames to the sink below, which must not wait: they go into a ring of a fifth of a second, and each period
 * the DSP hands back is filled from it (with silence when it is empty).
 */
#define RING_FRAMES     9600u
static int16_t ring[RING_FRAMES * 2];
static volatile uint32_t ring_in, ring_out;     /* frames ever put in and taken out */
static uint32_t ring_lost;

static uint32_t
darwin_sink(const int16_t *frames, uint32_t count)
{
	uint32_t in = ring_in, room = RING_FRAMES - (in - ring_out), n = MIN(count, room);

	for (uint32_t i = 0; i < n; i++) {
		uint32_t at = (in + i) % RING_FRAMES;

		ring[2 * at] = frames[2 * i];
		ring[2 * at + 1] = frames[2 * i + 1];
	}
	__asm__ volatile ("dmb ish" ::: "memory");
	ring_in = in + n;
	ring_lost += count - n;
	return n;
}

/* A period from the ring: true when it holds sound. A period whose mean is far from zero is not sound but a fault
 * upstream (a constant level is a current through the speaker's coil): ten of them in a row and silence goes out
 * instead until it is over -- the amplifier has no protection of its own here (no DSP firmware in it). */
static bool
darwin_fill(int16_t *p)
{
	static uint32_t dc_periods;
	uint32_t out = ring_out, have = ring_in - out, n = MIN(have, (uint32_t)PERIOD_FRAMES);
	int32_t sum = 0;
	bool sound = false, mono = hakios_amp_count() < 2;     /* one speaker: both channels into it */

	for (uint32_t f = 0; f < PERIOD_FRAMES; f++) {
		int16_t l = 0, r = 0;

		if (f < n) {
			uint32_t at = (out + f) % RING_FRAMES;

			l = ring[2 * at];
			r = ring[2 * at + 1];
		}
		if (mono) {             /* one speaker plays: both channels into it, nothing of the right one lost */
			int16_t m = (int16_t)(((int32_t)l + (int32_t)r) / 2);

			l = r = m;
		}
		p[2 * f] = l;
		p[2 * f + 1] = r;
		sum += l;
		sound = sound || l > 8 || l < -8 || r > 8 || r < -8;
	}
	ring_out = out + n;
	if (sum / (int32_t)PERIOD_FRAMES > 2000 || sum / (int32_t)PERIOD_FRAMES < -2000) {
		if (++dc_periods == 10) {
			say("adsp: sound: a constant level (mean %d) for 100 ms: silence goes out until it is over\n",
			    sum / (int32_t)PERIOD_FRAMES);
		}
	} else {
		if (dc_periods >= 10) {
			say("adsp: sound: the constant level is over\n");
		}
		dc_periods = 0;
	}
	if (dc_periods >= 10) {
		memset(p, 0, PERIOD_BYTES);
		return false;
	}
	return sound;
}

static bool darwin_mode, period_had_sound;

/* A period of 1 kHz at -20 dBFS in both channels (or of silence), where the DSP reads it, and the write that says so. */
static bool
tone_write(uint32_t period, bool sound)
{
	static const int16_t quarter[13] = { 0, 428, 848, 1254, 1638, 1995, 2317, 2600, 2838, 3027, 3165, 3249, 3277 };
	int16_t *p = (int16_t *)((uint8_t *)tone_buffer + PERIOD_STRIDE * period);
	uint64_t address = TONE_DSP_ADDRESS + PERIOD_STRIDE * period;
	uint32_t write[8] = { (uint32_t)address, (uint32_t)(address >> 32), tone_handle, PERIOD_BYTES, period, 0, 0, 0 };

	if (darwin_mode) {
		period_had_sound = darwin_fill(p);
	}
	for (uint32_t f = 0; f < PERIOD_FRAMES && !darwin_mode; f++) {
		uint32_t k = tone_at++ % 48, q = k % 24;
		int16_t v = quarter[q <= 12 ? q : 24 - q];

		p[2 * f] = p[2 * f + 1] = !sound ? 0 : k < 24 ? v : (int16_t)-v;
	}
	for (uint32_t o = 0; o < PERIOD_BYTES; o += 64) {       /* the DSP reads the memory, not this processor's cache */
		__asm__ volatile ("dc cvac, %0" : : "r" ((uint8_t *)p + o) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	tone_sent++;
	return apr_post(APR_SVC_ASM, STREAM_PORT, 0x80000000u | period, ASM_WRITE, write, sizeof(write));
}

/* With the port running: the buffer, the path, the stream, the tone; the amplifier on once the DSP takes the samples. */
/* ---- step 10: the microphones. The tertiary TDM port takes the RT5514's frames; a stream reads them; levels said ---- */

extern bool hakios_rt5514_start(void);         /* hakios.rt5514_sunfish.c */
extern void hakios_rt5514_stop(void);

#define AFE_PORT_TERT_TDM_TX    0x9021
#define AFE_GROUP_TERT_TDM_TX   0x9121
#define AFE_CLK_TER_TDM_IBIT    0x204
#define TLMM_WEST_PA            0x03500000ul    /* gpio 53..56 (pinctrl-sdmmagpie.c), function 1 = ter_mi2s */
#define MIC_PORT                0x0201          /* session 2, stream 1 */
#define MIC_PERIODS             8
#define MIC_PERIOD_FRAMES       480             /* 10 ms */
#define MIC_CHANNELS            2               /* slots 0 and 1: Stereo1's left and right (the two edges of DMIC1) */
#define MIC_PERIOD_BYTES        (MIC_PERIOD_FRAMES * 2 * MIC_CHANNELS)
#define MIC_IOVA                (TONE_IOVA + PERIODS * PERIOD_STRIDE)   /* right after the speaker's: one SMMU mapping holds both */
#define MIC_DSP_ADDRESS         (MIC_IOVA | 1ull << 32)

static int16_t *mic_buffer;
static uint32_t mic_handle;

static bool
afe_svc_param(const char *what, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t body[96];
	uint32_t head[7] = { 12u + size, 0, 0, 0, module, param, size };

	if (size > sizeof(body) - sizeof(head)) {
		return false;
	}
	memcpy(body, head, sizeof(head));
	memcpy(body + sizeof(head), data, size);
	return afe_do(what, 0, AFE_SVC_SET_PARAM, body, (uint16_t)(sizeof(head) + size));
}

static bool
afe_port_param(const char *what, uint16_t port, uint32_t module, uint32_t param, const void *data, uint16_t size)
{
	uint8_t body[96];
	struct { uint16_t port, size; uint32_t zero[3]; uint32_t module, param; uint16_t param_size, reserved; } __attribute__((packed))
	head = { port, (uint16_t)(12u + size), { 0, 0, 0 }, module, param, size, 0 };

	if (size > sizeof(body) - sizeof(head)) {
		return false;
	}
	memcpy(body, &head, sizeof(head));
	memcpy(body + sizeof(head), data, size);
	return afe_do(what, port, AFE_PORT_SET_PARAM_V2, body, (uint16_t)(sizeof(head) + size));
}

static bool
mic_read(uint32_t period)
{
	uint64_t address = MIC_DSP_ADDRESS + PERIOD_STRIDE * period;
	uint32_t read[5] = { (uint32_t)address, (uint32_t)(address >> 32), mic_handle, MIC_PERIOD_BYTES, period };
	uint8_t *frames = (uint8_t *)mic_buffer + PERIOD_STRIDE * period;

	for (uint32_t o = 0; o < MIC_PERIOD_BYTES; o += 64) {   /* nothing of ours left in the cache to be written over the DSP's */
		__asm__ volatile ("dc civac, %0" : : "r" (frames + o) : "memory");
	}
	__asm__ volatile ("dsb sy" ::: "memory");
	return apr_post(APR_SVC_ASM, MIC_PORT, 0x40000000u | period, 0x00010DACu, read, sizeof(read));
}

/*
 * /dev/hakios-mic, as drivers/audio/hakios.mic.c (the redwood's) makes it for the audio plug-in's input "Codec"
 * (userland/src/audio_plugin.c): 16-bit samples, one channel, 48 kHz; open while Darwin records, reads never wait and
 * keep the reader about 40 ms behind; the path up while it is open (the sound loop below brings it up and down).
 */
#define MRING_FRAMES            16384
#define MRING_KEEP              1920
#define MRING_MOST              4800

static int16_t mring[MRING_FRAMES];
static volatile uint64_t mring_written;
static uint64_t mring_read, mring_skipped, mring_dry;
static bool mring_primed;
static volatile uint32_t mic_wanted;           /* the device is open */

static void
mring_put(const int16_t *samples, uint32_t count, uint32_t stride)
{
	uint64_t at = mring_written;

	for (uint32_t i = 0; i < count; i++) {
		mring[(at + i) % MRING_FRAMES] = samples[i * stride];
	}
	__atomic_store_n(&mring_written, at + count, __ATOMIC_RELEASE);
}

static int
mic_dev_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint64_t written = __atomic_load_n(&mring_written, __ATOMIC_ACQUIRE), avail, want;
	int error = 0;

	if (uio_resid(uio) < 2) {
		return 0;
	}
	want = (uint64_t)uio_resid(uio) / 2;
	avail = written - mring_read;
	if (avail > MRING_MOST) {
		mring_skipped += avail - MRING_KEEP;
		mring_read = written - MRING_KEEP;
		avail = MRING_KEEP;
	}
	if (!mring_primed) {
		if (avail < MRING_KEEP) {
			return 0;
		}
		mring_primed = true;
	}
	if (avail < want) {
		mring_dry++;
		mring_primed = false;
	}
	while (want > 0 && avail > 0 && error == 0) {
		uint64_t at = mring_read % MRING_FRAMES, n = MIN(want, avail);

		n = MIN(n, MRING_FRAMES - at);
		error = uiomove((caddr_t)&mring[at], (int)(n * 2), uio);
		mring_read += n;
		want -= n;
		avail -= n;
	}
	return error;
}

static int
mic_dev_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	mring_read = __atomic_load_n(&mring_written, __ATOMIC_ACQUIRE);
	mring_primed = false;
	mic_wanted = 1;
	return 0;
}

static int
mic_dev_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
	mic_wanted = 0;
	return 0;
}

static const struct cdevsw mic_cdevsw = {
	.d_open = mic_dev_open, .d_close = mic_dev_close, .d_read = mic_dev_read, .d_write = eno_rdwrt,
	.d_ioctl = eno_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = eno_select,
	.d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};

static bool mic_node_made;

static void
mic_make_node(void)
{
	int major;

	if (mic_node_made) {
		return;
	}
	major = cdevsw_add(-1, &mic_cdevsw);
	mic_node_made = major >= 0 && devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0666, "hakios-mic") != NULL;
	say("adsp: mic: /dev/hakios-mic %s\n", mic_node_made ? "made: the microphone is Darwin's while it records" : "CANNOT BE MADE");
}

/* the path: what step 10 found to work (logs/201100-run.log) */
static struct {
	bool voted, grouped, clocked, started, codec, mapped, path_open, stream_open, running, failed;
	uint32_t vote_handle, path_id, periods, errors_said;
	uint32_t pins_before[4];
	vm_offset_t west[4], memory;
	/* levels, for step 10 */
	uint64_t sum[MIC_CHANNELS];
	int32_t peak[MIC_CHANNELS];
	uint32_t count;
} mic;

static void mic_down(void);

static bool
mic_up(void)
{
	struct { uint32_t version; uint16_t group, reserved, ports[8]; uint32_t channels, rate, bits; uint16_t slots, slot_width;
		 uint32_t slot_mask; } __attribute__((packed)) group = { 1, AFE_GROUP_TERT_TDM_TX, 0, { AFE_PORT_TERT_TDM_TX },
		 8, 48000, 32, 8, 32, 0xff };
	uint16_t enable[2] = { AFE_GROUP_TERT_TDM_TX, 1 };
	uint32_t clock_on[5] = { 1, AFE_CLK_TER_TDM_IBIT, 12288000, 1 /* attribute 1, root 0 */, 1 };
	struct { uint32_t version, channels, rate, bits; uint16_t format, sync_mode, sync_src, slots, data_out, invert, delay, slot_width;
		 uint32_t slot_mask; } __attribute__((packed)) tdm = { 1, MIC_CHANNELS, 48000, 16, 0, 0, 1, 8, 0, 0, 1, 32, 0x3 };
	struct { uint32_t version; uint16_t channels, bits; uint32_t align; uint16_t offset[8]; } __attribute__((packed)) slots =
	    { 1, MIC_CHANNELS, 16, 0, { 0, 4, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff } };
	struct { uint16_t pool, regions; uint32_t property, low, high, size; } __attribute__((packed)) map =
	    { 3, 1, 0, (uint32_t)MIC_DSP_ADDRESS, (uint32_t)(MIC_DSP_ADDRESS >> 32), MIC_PERIODS * PERIOD_STRIDE };
	struct { uint16_t flags, mode, endpoint1, endpoint2; uint32_t topology; uint16_t channels, bits; uint32_t rate; uint8_t channel_map[8]; }
	__attribute__((packed)) path = { 0, 2 /* capture */, AFE_PORT_TERT_TDM_TX, 0xffff, 0x00010312, MIC_CHANNELS, 16, 48000, { 1, 2 } };
	struct { uint32_t flags, endpoint, topology, format; uint16_t bits, reserved; } __attribute__((packed)) open =
	    { 0, 0 /* the device matrix */, 0x00010C68, 0x00010DA5, 16, 0 };
	struct { uint32_t matrix, sessions; uint16_t session, paths, path[2]; } __attribute__((packed)) route = { 1, 1, 2, 1, { 0, 0 } };
	struct { uint32_t param, size, frames_per_buffer, block_size; uint16_t channels, bits; uint32_t rate; uint16_t is_signed, reserved;
		 uint8_t channel_map[8]; } __attribute__((packed)) encoder = { 0x00010DA3, 28, 0, 20, MIC_CHANNELS, 16, 48000, 1, 0, { 1, 2 } };
	uint32_t run[3] = { 0, 0, 0 };
	struct { uint32_t block; char name[8]; } vote = { 2, "hakmic" };
	uint16_t start[2] = { AFE_PORT_TERT_TDM_TX, 0 };
	bool posted = true;

	memset(&mic, 0, sizeof(mic));
	if (apr_call(APR_SVC_AFE, 0, AFE_HW_VOTE, &vote, sizeof(vote)) && apr.last_opcode == 0x000100F5u) {
		mic.voted = true;
		mic.vote_handle = apr.last_words[0];
	}
	/* the TDM pins to the port (function 1, 8 mA, no pull, as Android's ter_tdm_active) */
	for (unsigned i = 0; i < 4; i++) {
		mic.west[i] = ml_io_map(TLMM_WEST_PA + 0x1000ul * (53 + i), 0x1000);
		if (mic.west[i] != 0) {
			mic.pins_before[i] = *(volatile uint32_t *)mic.west[i];
			*(volatile uint32_t *)mic.west[i] = 1u << 2 | 3u << 6;
		}
	}
	mic.grouped = afe_svc_param("the TDM group: 8 slots of 32 bits at 48 kHz", 0x00010254, 0x0001029E, &group, sizeof(group)) &&
	    afe_svc_param("the TDM group enabled", 0x00010254, 0x00010256, enable, sizeof(enable));
	mic.clocked = mic.grouped && afe_svc_param("the TDM bit clock, 12.288 MHz, on", 0x0001028F, 0x00010290, clock_on, sizeof(clock_on));
	if (mic.clocked && afe_port_param("the port's TDM format: 2 channels, 16 bits, slots 0 and 1", AFE_PORT_TERT_TDM_TX, 0x0001020C,
	    0x0001029D, &tdm, sizeof(tdm)) && afe_port_param("the port's slot mapping", AFE_PORT_TERT_TDM_TX, 0x0001028A, 0x00010297,
	    &slots, sizeof(slots))) {
		mic.started = afe_do("the port started", AFE_PORT_TERT_TDM_TX, AFE_PORT_START, start, sizeof(start));
	}
	mic.codec = mic.started && hakios_rt5514_start();
	if (mic.codec && mic_buffer == NULL) {
		vm_offset_t memory = 0;

		if (kmem_alloc_contig(kernel_map, &memory, MIC_PERIODS * PERIOD_STRIDE, PAGE_MASK, 0, 0, KMA_DATA | KMA_ZERO,
		    VM_KERN_MEMORY_IOKIT) == KERN_SUCCESS && hakios_sm7150_smmu_pages(TONE_SID, MIC_IOVA, kvtophys(memory),
		    MIC_PERIODS * PERIOD_STRIDE >> 12)) {
			mic_buffer = (int16_t *)memory;         /* kept for the rest of the run, and the SMMU's mapping with it */
		}
	}
	if (mic.codec && mic_buffer != NULL && apr_call(APR_SVC_ASM, 0, ASM_MAP, &map, sizeof(map)) && apr.last_opcode == 0x00010D93u &&
	    apr.last_words[0] != 0) {
		mic.mapped = true;
		mic_handle = apr.last_words[0];
	}
	if (mic.mapped && apr_call(APR_SVC_ADM, AFE_PORT_TERT_TDM_TX, ADM_OPEN, &path, sizeof(path)) && apr.last_opcode == 0x00010329u &&
	    apr.last_words[0] == 0) {
		mic.path_open = true;
		mic.path_id = apr.last_words[1] & 0xffff;
	}
	if (mic.path_open) {
		route.path[0] = (uint16_t)mic.path_id;
		mic.stream_open = basic_ok("the stream opened for reading", APR_SVC_ASM, MIC_PORT, 0x00010DB4u, &open, sizeof(open));
	}
	if (mic.stream_open && basic_ok("the stream routed from the path", APR_SVC_ADM, 0, ADM_ROUTE, &route, sizeof(route)) &&
	    basic_ok("the encoder: 48 kHz, 16 bits, 2 channels", APR_SVC_ASM, MIC_PORT, 0x00010C10u, &encoder, sizeof(encoder))) {
		mic.running = basic_ok("the stream run", APR_SVC_ASM, MIC_PORT, ASM_RUN, run, sizeof(run));
	}
	apr.read_done = apr.read_errors = 0;
	for (uint32_t p = 0; p < MIC_PERIODS && mic.running && posted; p++) {
		posted = mic_read(p);
	}
	if (!mic.running || !posted) {
		say("adsp: mic: the path did not come up: given back\n");
		mic_down();
		mic.failed = true;
		return false;
	}
	say("adsp: mic: THE MICROPHONE LISTENS (capture path %u)\n", mic.path_id);
	return true;
}

/* the buffers the DSP filled: into the ring (slot 0) and the levels; each handed back. False when one cannot be. */
static bool
mic_poll(void)
{
	for (uint32_t p = 0; p < MIC_PERIODS; p++) {
		int16_t *frames;
		uint32_t n;

		if (!(apr.read_done & (1u << p))) {
			continue;
		}
		frames = (int16_t *)((uint8_t *)mic_buffer + PERIOD_STRIDE * p);
		n = MIN(apr.read_bytes[p], (uint32_t)MIC_PERIOD_BYTES) / 2;
		apr.read_done &= ~(1u << p);
		for (uint32_t o = 0; o < MIC_PERIOD_BYTES; o += 64) {
			__asm__ volatile ("dc ivac, %0" : : "r" ((uint8_t *)frames + o) : "memory");
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		mring_put(frames, n / MIC_CHANNELS, MIC_CHANNELS);
		for (uint32_t f = 0; f < n; f++) {
			int32_t v = frames[f];
			unsigned c = f % MIC_CHANNELS;

			mic.sum[c] += (uint64_t)((int64_t)v * v);
			mic.peak[c] = MAX(mic.peak[c], v < 0 ? -v : v);
		}
		mic.count += n / MIC_CHANNELS;
		mic.periods++;
		if (!mic_read(p)) {
			return false;
		}
	}
	if (apr.read_errors != mic.errors_said && apr.read_errors % 100 == 1) {
		mic.errors_said = apr.read_errors;
		say("adsp: mic: %u capture buffers came back with an error\n", apr.read_errors);
	}
	return true;
}

/* back, in the opposite order, as far as it was done */
static void
mic_down(void)
{
	uint16_t disable[2] = { AFE_GROUP_TERT_TDM_TX, 0 }, start[2] = { AFE_PORT_TERT_TDM_TX, 0 };
	uint32_t clock_off[5] = { 1, AFE_CLK_TER_TDM_IBIT, 12288000, 1, 0 };

	if (mic.running) {
		(void)basic_ok("the stream paused", APR_SVC_ASM, MIC_PORT, ASM_PAUSE, NULL, 0);
	}
	if (mic.stream_open) {
		(void)basic_ok("the stream closed", APR_SVC_ASM, MIC_PORT, ASM_CLOSE, NULL, 0);
	}
	if (mic.path_open) {
		(void)basic_ok("the capture path closed", APR_SVC_ADM, (uint16_t)mic.path_id, ADM_CLOSE, NULL, 0);
	}
	if (mic.mapped) {
		(void)basic_ok("the buffer taken back", APR_SVC_ASM, 0, ASM_UNMAP, &mic_handle, sizeof(mic_handle));
	}
	if (mic.codec) {
		hakios_rt5514_stop();
	}
	if (mic.started) {
		(void)afe_do("the port stopped", AFE_PORT_TERT_TDM_TX, AFE_PORT_STOP, start, sizeof(start));
	}
	if (mic.clocked) {
		(void)afe_svc_param("the TDM bit clock off", 0x0001028F, 0x00010290, clock_off, sizeof(clock_off));
	}
	if (mic.grouped) {
		(void)afe_svc_param("the TDM group disabled", 0x00010254, 0x00010256, disable, sizeof(disable));
	}
	for (unsigned i = 0; i < 4; i++) {
		if (mic.west[i] != 0) {
			*(volatile uint32_t *)mic.west[i] = mic.pins_before[i];
		}
	}
	if (mic.voted) {
		uint32_t devote[2] = { 2, mic.vote_handle };

		(void)afe_do("the vote for LPASS's core given back", 0, AFE_HW_DEVOTE, devote, sizeof(devote));
	}
	if (mic.running) {
		say("adsp: mic: the microphone stopped after %u periods (%u with an error)\n", mic.periods, apr.read_errors);
	}
	mic.voted = mic.grouped = mic.clocked = mic.started = mic.codec = mic.mapped = mic.path_open = mic.stream_open = false;
	mic.running = false;
}

/* step 10: the path for `seconds', its levels said every second (no Darwin) */
static void
step_mic(uint32_t seconds)
{
	uint32_t waited = 0;

	say("adsp: step 10: THE MICROPHONES for %u s: the tertiary TDM port (AFE %#x), the RT5514, a capture stream\n",
	    seconds, AFE_PORT_TERT_TDM_TX);
	if (!mic_up()) {
		return;
	}
	while (waited < seconds * 1000 && mic_poll()) {
		sleep_ms(1);
		waited++;
		glink_poll();
		if (waited % 1000 == 0) {
			uint32_t rms[MIC_CHANNELS] = { 0 };

			for (unsigned c = 0; c < MIC_CHANNELS; c++) {
				uint64_t mean = mic.count != 0 ? mic.sum[c] / mic.count : 0;

				while ((uint64_t)(rms[c] + 1) * (rms[c] + 1) <= mean) {
					rms[c]++;
				}
				mic.sum[c] = 0;
			}
			say("adsp: mic: %u s: %u periods (%u errors): slot 0 rms %u peak %d, slot 1 rms %u peak %d\n", waited / 1000,
			    mic.periods, apr.read_errors, rms[0], mic.peak[0], rms[1], mic.peak[1]);
			mic.count = 0;
			mic.peak[0] = mic.peak[1] = 0;
		}
	}
	mic_down();
}

static void
step_tone(bool darwin)
{
	struct { uint16_t pool, regions; uint32_t property, low, high, size; } __attribute__((packed)) map =
	    { 3 /* pages of 4 KiB */, 1, 0, (uint32_t)TONE_DSP_ADDRESS, (uint32_t)(TONE_DSP_ADDRESS >> 32), PERIODS * PERIOD_STRIDE };
	struct { uint16_t flags, mode, endpoint1, endpoint2; uint32_t topology; uint16_t channels, bits; uint32_t rate; uint8_t channel_map[8]; }
	__attribute__((packed)) path = { 0, 1 /* playback */, AFE_PORT_SEC_MI2S_RX, 0xffff, 0x00010312 /* no processing */, 2, 16, 48000, { 1, 2 } };
	struct { uint32_t flags; uint16_t sink, bits; uint32_t topology, format; } __attribute__((packed)) open =
	    { 0, 0, 16, 0x00010C68 /* no processing */, 0x00010DA5 /* linear PCM, channels mapped */ };
	struct { uint32_t matrix, sessions; uint16_t session, paths, path[2]; } __attribute__((packed)) route = { 0, 1, 1, 1, { 0, 0 } };
	struct { uint32_t size; uint16_t channels, bits; uint32_t rate; uint16_t is_signed, reserved; uint8_t channel_map[8]; }
	__attribute__((packed)) format = { 20, 2, 16, 48000, 1, 0, { 1, 2 } };
	uint32_t run[3] = { 0, 0, 0 }, seconds = 2, tone_periods, played = 0, path_id = 0;
	int volume = -30;
	uint32_t below = 30;
	bool mapped = false, path_open = false, stream_open = false, running = false, amp_ready = false, amp_on = false;
	vm_offset_t memory = 0;

	PE_parse_boot_argn("adsp_tone", &seconds, sizeof(seconds));
	PE_parse_boot_argn("adsp_below", &below, sizeof(below));        /* decibels under the amplifier's full digital volume: 20 at least */
	seconds = MIN(MAX(seconds, 1u), 3u);
	volume = -(int)MIN(MAX(below, 20u), 102u);
	tone_periods = seconds * 100;
	if (darwin) {
		below = 0;              /* Darwin's sound at the amplifier's full digital volume, as the stock system: the amplifier's driver
		                         * holds it 6 dB lower by itself when the protection firmware is not running in it */
		PE_parse_boot_argn("adsp_below", &below, sizeof(below));
		volume = -(int)MIN(below, 102u);
		say("adsp: step 9: DARWIN'S SOUND to the bottom speaker until the run ends, its amplifier at %d dB\n", volume);
	} else {
		say("adsp: step 8: A TONE: 1 kHz at -20 dBFS for %u s from the bottom speaker, its amplifier at %d dB\n", seconds, volume);
	}

	/*
	 * The speaker's buffers and the microphone's together, in one mapping: a second hakios_sm7150_smmu_pages() for the
	 * same stream makes a new page table for it and the first mapping is gone (the DSP stopped answering when the
	 * microphone's buffer was mapped while the speaker played, logs/202230-run.log).
	 */
	if (kmem_alloc_contig(kernel_map, &memory, (PERIODS + MIC_PERIODS) * PERIOD_STRIDE, PAGE_MASK, 0, 0, KMA_DATA | KMA_ZERO,
	    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS || !hakios_sm7150_smmu_pages(TONE_SID, TONE_IOVA, kvtophys(memory),
	    (PERIODS + MIC_PERIODS) * PERIOD_STRIDE >> 12)) {
		say("adsp: stream: no buffer the DSP can read (memory or the SMMU)\n");
		return;
	}
	tone_buffer = (int16_t *)memory;
	mic_buffer = (int16_t *)(memory + PERIODS * PERIOD_STRIDE);
	say("adsp: stream: %u bytes at pa %#llx shown to the DSP as %#llx (stream %#x)\n", PERIODS * PERIOD_STRIDE, kvtophys(memory),
	    TONE_DSP_ADDRESS, TONE_SID);
	/* the amplifier configured and read back before anything plays; GLOBAL_EN only once the DSP is taking samples */
	amp_ready = hakios_amp_prepare(0x41, volume);
	if (darwin) {
		uint32_t stereo = 1;            /* boot-arg adsp_stereo=0: the bottom speaker alone */

		PE_parse_boot_argn("adsp_stereo", &stereo, sizeof(stereo));
		if (stereo && hakios_amp_prepare(0x40, volume)) {
			amp_ready = true;
			say("adsp: sound: STEREO: the earpiece's amplifier (0x40) is the left speaker, the bottom one (0x41) the right\n");
		}
		/*
		 * The first power-up is slow (the firmware's core is started then, with a reset and the DSP clock cycled: some
		 * 100 ms each), and done in the middle of a sound it cost frames (6304 lost at the first sound, logs/154212-run.log):
		 * done once here, with the port's clock running and nothing playing, then paused; a sound later only resumes.
		 */
		if (amp_ready && hakios_amp_start() && hakios_amp_pause()) {
			say("adsp: sound: the amplifiers warmed up and paused before anything plays\n");
		}
		amp_ready = hakios_amp_count() != 0;
	}
	if (!amp_ready) {
		say("adsp: the amplifier is not ready: the stream is run without it (nothing will be heard)\n");
	}
	if (apr_call(APR_SVC_ASM, 0, ASM_MAP, &map, sizeof(map)) && apr.last_opcode == 0x00010D93u && apr.last_words[0] != 0) {
		mapped = true;
		tone_handle = apr.last_words[0];
	}
	say("adsp: stream: the buffer made known to the DSP: %s (answer %#x: %#x %#x)\n", mapped ? "a handle" : "REFUSED", apr.last_opcode,
	    apr.last_words[0], apr.last_words[1]);
	if (mapped && apr_call(APR_SVC_ADM, AFE_PORT_SEC_MI2S_RX, ADM_OPEN, &path, sizeof(path)) && apr.last_opcode == 0x00010329u &&
	    apr.last_words[0] == 0) {
		path_open = true;
		path_id = apr.last_words[1] & 0xffff;
	}
	if (mapped) {
		say("adsp: stream: the device path to the port: %s (answer %#x: status %#x, id %u)\n", path_open ? "open" : "REFUSED",
		    apr.last_opcode, apr.last_words[0], path_id);
	}
	if (path_open) {
		route.path[0] = (uint16_t)path_id;
		stream_open = basic_ok("the stream opened for writing", APR_SVC_ASM, STREAM_PORT, ASM_OPEN_WRITE, &open, sizeof(open));
	}
	if (stream_open && basic_ok("the stream routed to the path", APR_SVC_ADM, 0, ADM_ROUTE, &route, sizeof(route)) &&
	    basic_ok("the stream's format: 48 kHz, 16 bits, stereo", APR_SVC_ASM, STREAM_PORT, ASM_FORMAT, &format, sizeof(format))) {
		running = basic_ok("the stream run", APR_SVC_ASM, STREAM_PORT, ASM_RUN, run, sizeof(run));
	}
	if (running && darwin) {
		/*
		 * Until the run ends: every period the DSP hands back goes out again, filled from the ring. The amplifier is on
		 * while there is sound and for three seconds after, then paused (configured, GLOBAL_EN off); an error bit, looked
		 * for every half second while it is on, powers it down for the rest of the run.
		 */
		uint32_t silent_ms = 0, look_ms = 0, report_ms = 0, starts = 0, mic_idle_ms = 0;
		bool wrote = true, dead = !amp_ready;

		darwin_mode = true;
		apr.done_periods = 0;
		apr.written_done = apr.write_errors = apr.underflows = 0;
		tone_sent = 0;
		for (uint32_t p = 0; p < PERIODS && wrote; p++) {
			wrote = tone_write(p, false);
		}
		hakios_audio_set_sink("the Pixel 4a's bottom speaker, through the audio DSP", darwin_sink);
		mic_make_node();
		say("adsp: sound: DARWIN'S SOUND GOES TO THE SPEAKER from here on (amplifier %d dB down%s)\n", -volume,
		    dead ? "; THE AMPLIFIER IS NOT READY: nothing will be heard" : "");
		while (wrote) {
			bool sound_now = false;

			sleep_ms(1);
			glink_poll();
			for (uint32_t p = 0; p < PERIODS && wrote; p++) {
				if (apr.done_periods & (1u << p)) {
					apr.done_periods &= ~(1u << p);
					wrote = tone_write(p, false);
					sound_now = sound_now || period_had_sound;
				}
			}
			/* the microphone: up while Darwin has /dev/hakios-mic open, down 3 s after it closed it */
			if (mic_wanted && !mic.running && !mic.failed) {
				(void)mic_up();
				mic_idle_ms = 0;
			} else if (mic.running) {
				mic_idle_ms = mic_wanted ? 0 : mic_idle_ms + 1;
				if (!mic_poll() || mic_idle_ms >= 3000) {
					mic_down();
				}
			} else if (!mic_wanted) {
				mic.failed = false;             /* (a failed start is tried again when Darwin opens it anew) */
			}
			silent_ms = sound_now ? 0 : silent_ms + 1;
			if (!dead && sound_now && !amp_on) {
				amp_on = hakios_amp_start();
				starts++;
				if (!amp_on) {
					dead = true;
					hakios_amp_stop();
					say("adsp: sound: the amplifier did not come on: off for the rest of the run\n");
				}
			} else if (amp_on && silent_ms >= 3000) {
				amp_on = false;
				if (!hakios_amp_pause()) {
					dead = true;
					hakios_amp_stop();
					say("adsp: sound: the amplifier did not pause: powered down for the rest of the run\n");
				}
			}
			if (amp_on && ++look_ms >= 500) {
				look_ms = 0;
				if (!hakios_amp_healthy()) {
					amp_on = false;
					dead = true;
					hakios_amp_stop();
					say("adsp: sound: THE AMPLIFIER REPORTS AN ERROR: powered down for the rest of the run\n");
				}
			}
			if (++report_ms >= 30000) {
				report_ms = 0;
				say("adsp: sound: %u periods played (%u errors, %u underflows), %u frames lost at the ring, amplifier %s, on %u times\n",
				    apr.written_done, apr.write_errors, apr.underflows, ring_lost, dead ? "dead" : amp_on ? "on" : "paused", starts);
			}
		}
		hakios_audio_set_sink("nothing", NULL);
		if (mic.running) {
			mic_down();
		}
		darwin_mode = false;
		say("adsp: sound: the stream stopped taking periods: over\n");
	} else if (running) {
		uint32_t waited = 0, amp_ms = 0;
		bool wrote = true;

		apr.done_periods = 0;
		apr.written_done = apr.write_errors = apr.underflows = 0;
		tone_sent = 0;
		for (uint32_t p = 0; p < PERIODS && wrote; p++) {
			wrote = tone_write(p, true);
		}
		/* until the tone's periods are played (and at most 6 s): a period that comes back is filled and written again */
		while (wrote && played < tone_periods && waited < 6000) {
			sleep_ms(1);
			waited++;
			glink_poll();
			for (uint32_t p = 0; p < PERIODS && wrote; p++) {
				if (apr.done_periods & (1u << p)) {
					apr.done_periods &= ~(1u << p);
					played++;
					if (tone_sent < tone_periods + PERIODS) {
						wrote = tone_write(p, tone_sent < tone_periods);
					}
				}
			}
			if (amp_ready && !amp_on && apr.written_done >= 1) {     /* the DSP takes the samples: the bit clock carries them */
				amp_on = hakios_amp_start();
				say("adsp: THE AMPLIFIER IS %s with the tone playing\n", amp_on ? "ON" : "NOT on");
				amp_ready = amp_on;
			}
			if (amp_on && ++amp_ms >= 3000) {
				break;                                  /* never on for more than 3 s */
			}
		}
		say("adsp: stream: %u periods written, %u played (%u with an error), %u underflows, in %u ms\n", tone_sent, apr.written_done,
		    apr.write_errors, apr.underflows, waited);
	}
	/* back: the amplifier first, then the stream, the path, the buffer */
	hakios_amp_stop();
	if (running) {
		(void)basic_ok("the stream paused", APR_SVC_ASM, STREAM_PORT, ASM_PAUSE, NULL, 0);
	}
	if (stream_open) {
		(void)basic_ok("the stream closed", APR_SVC_ASM, STREAM_PORT, ASM_CLOSE, NULL, 0);
	}
	if (path_open) {
		(void)basic_ok("the device path closed", APR_SVC_ADM, (uint16_t)path_id, ADM_CLOSE, NULL, 0);
	}
	if (mapped) {
		(void)basic_ok("the buffer taken back from the DSP", APR_SVC_ASM, 0, ASM_UNMAP, &tone_handle, sizeof(tone_handle));
	}
}

static void
step_port(uint32_t hold_seconds, bool with_amp, bool with_tone, bool darwin)
{
	/* the port's parameter: port and size (16 bits each), no address, no handle; module, parameter, size; the I2S format */
	struct {
		uint16_t port, size;
		uint32_t zero[3];
		uint32_t module, param;
		uint16_t param_size, reserved;
		uint32_t version;
		uint16_t bits, lane, mono_stereo, ws_source;
		uint32_t rate;
		uint16_t format, reserved2;
	} __attribute__((packed)) i2s = { AFE_PORT_SEC_MI2S_RX, 32, { 0, 0, 0 }, 0x0001020C, 0x0001020D, 20, 0, 1, 16,
		                          2 /* the data line SD1: on this phone the amplifiers' input (lanes swapped) */, 1 /* stereo */,
		                          1 /* word select from the DSP */, 48000, 0, 0 };
	static const uint32_t pins_want[4] = { 0xcc, 0xcc, 0xd0, 0xc8 };       /* gpio 8 clock, 9 word select, 10 SD0, 11 SD1; 8 mA, no pull */
	struct { uint32_t block; char name[8]; } vote = { 2, "hakios" };
	uint16_t start[2] = { AFE_PORT_SEC_MI2S_RX, 0 };
	uint32_t pins_before[4] = { 0 }, handle = 0;
	bool voted = false, clocked, started = false, pins_set = false;
	vm_offset_t lpi = 0;

	say("adsp: step 6: the secondary I2S port (AFE %#x) run for %u s; the amplifiers stay in reset, nothing is played\n",
	    AFE_PORT_SEC_MI2S_RX, hold_seconds);
	if (apr_call(APR_SVC_AFE, 0, AFE_HW_VOTE, &vote, sizeof(vote)) && apr.last_opcode == 0x000100F5u) {
		voted = true;
		handle = apr.last_words[0];
	}
	say("adsp: AFE: the vote for LPASS's core (block 2): %s (answer %#x: %#x %#x)\n", voted ? "a handle" : "none; going on without",
	    apr.last_opcode, apr.last_words[0], apr.last_words[1]);
	clocked = afe_clock(true);
	if (clocked && afe_do("the port's format: I2S, 48 kHz, 16 bits, stereo, data on SD1", AFE_PORT_SEC_MI2S_RX, AFE_PORT_SET_PARAM_V2,
	    &i2s, sizeof(i2s))) {
		started = afe_do("the port started", AFE_PORT_SEC_MI2S_RX, AFE_PORT_START, start, sizeof(start));
	}
	if (started) {
		/* LPASS's pin controller answers only while its core's clocks run: now, with the port started (a read before hangs the bus) */
		lpi = ml_io_map(LPI_PA + 0x8000, 0x4000);
		if (lpi != 0) {
			uint32_t toggles[2] = { 0, 0 }, last[2] = { 0, 0 };

			pins_set = true;
			for (unsigned i = 0; i < 4; i++) {
				volatile uint32_t *cfg = (volatile uint32_t *)(lpi + 0x1000ul * i);

				pins_before[i] = *cfg;
				*cfg = pins_want[i];
				__asm__ volatile ("dsb sy" ::: "memory");
				say("adsp: LPI gpio%u: configuration %#x -> %#x, reads back %#x\n", 8 + i, pins_before[i], pins_want[i], *cfg);
			}
			/* do the clock and the word select move? 20000 looks at each pin's input bit, a few microseconds apart */
			for (unsigned n = 0; n < 20000; n++) {
				for (unsigned i = 0; i < 2; i++) {
					uint32_t now = *(volatile uint32_t *)(lpi + 0x1000ul * i + 4) & 1;

					toggles[i] += n != 0 && now != last[i];
					last[i] = now;
				}
			}
			say("adsp: LPI: in 20000 looks the bit clock (gpio8) changed %u times, the word select (gpio9) %u times: %s\n",
			    toggles[0], toggles[1], toggles[0] > 100 && toggles[1] > 2 ? "THE PORT'S CLOCKS RUN AT THE PINS" :
			    "no clocks seen at the pins");
		}
		if (with_tone && pins_set) {
			step_tone(darwin);
		} else if (with_amp && pins_set) {
			/*
			 * Step 7: the bottom speaker's amplifier (0x41) brought up on this clock at its lowest volume (-102 dB) with
			 * nothing played into the port: does it take its configuration, does its PLL lock to the bit clock, does it
			 * say its power-up is done. Silent. Then it is powered down and put back into reset.
			 */
			say("adsp: step 7: the amplifier at 0x41 on the port's clock, at -102 dB, no samples: silent\n");
			if (hakios_amp_prepare(0x41, -102)) {
				bool up = hakios_amp_start();

				say("adsp: THE AMPLIFIER %s\n", up ? "POWERED UP ON THE PORT'S CLOCK (see its PLL_LOCK line above)" :
				    "did NOT power up");
				sleep_ms(1000);
				hakios_amp_stop();
			}
		}
		sleep_ms(hold_seconds * 1000);
		glink_poll();
	}
	/* back, in the opposite order; each as far as it was done */
	if (pins_set) {
		for (unsigned i = 0; i < 4; i++) {
			*(volatile uint32_t *)(lpi + 0x1000ul * i) = pins_before[i];
		}
		__asm__ volatile ("dsb sy" ::: "memory");
		say("adsp: LPI gpio 8 to 11 put back as they were\n");
	}
	if (started) {
		(void)afe_do("the port stopped", AFE_PORT_SEC_MI2S_RX, AFE_PORT_STOP, start, sizeof(start));
	}
	if (clocked) {
		(void)afe_clock(false);
	}
	if (voted) {
		uint32_t devote[2] = { 2, handle };

		(void)afe_do("the vote for LPASS's core given back", 0, AFE_HW_DEVOTE, devote, sizeof(devote));
	}
}

static void
step_watch(uint32_t seconds, uint32_t steps)
{
	struct glink_msg version = { .cmd = 0, .param1 = 1, .param2 = 1 };
	bool up = false, linked = false, port_done = false;
	uint32_t valid_before = ~0u, values[16] = { 0 };

	for (uint32_t tick = 0; tick < seconds * 20; tick++) {
		uint32_t size = 0;
		struct smp2p_smem_item *in = smem_get(SMP2P_IN, &size);

		sleep_ms(50);
		if (in != NULL && size >= sizeof(*in)) {
			if (!up) {
				up = true;
				say("adsp: SMP2P: THE DSP'S ITEM (%u) IS THERE after %u ms: version %u, hosts %u -> %u, features %#x\n",
				    SMP2P_IN, tick * 50, in->version, in->local_pid, in->remote_pid, (uint32_t)in->features);
				kick(KICK_SMP2P);
			}
			for (uint32_t v = 0; v < in->valid_entries && v < 16; v++) {
				if (in->valid_entries != valid_before || in->entries[v].value != values[v]) {
					char name[17] = { 0 };

					memcpy(name, in->entries[v].name, 16);
					values[v] = in->entries[v].value;
					say("adsp: SMP2P: \"%s\" = %#x\n", name, values[v]);
				}
			}
			valid_before = in->valid_entries;
		}
		if (!linked) {
			uint32_t rx_size = 0;

			rx_fifo = smem_get(480, &rx_size);
			if (rx_fifo != NULL && rx_size >= FIFO_SIZE) {
				linked = true;
				say("adsp: GLINK: the DSP's FIFO is there after %u ms; our version sent\n", tick * 50);
				(void)tx(&version, sizeof(version));
			}
		} else {
			glink_poll();
			apr_step(steps);
			if (steps >= 6 && !port_done && apr.received >= 2 && apr.intent_size != 0) {
				uint32_t hold = 3;

				PE_parse_boot_argn("adsp_hold", &hold, sizeof(hold));
				port_done = true;
				if (steps == 10) {
					uint32_t secs = 10;

					PE_parse_boot_argn("mic_secs", &secs, sizeof(secs));
					step_mic(MIN(secs, 120u));
				} else {
					step_port(MIN(hold, 30u), steps == 7, steps >= 8, steps >= 9);
				}
			}
		}
	}
	say("adsp: watched %u s: SMP2P %s, GLINK %s, %u channels opened by the DSP, %u APR packets received\n", seconds,
	    up ? "up" : "NOT up", linked ? "linked" : "NOT linked", channels, apr.received);
}

static void
adsp_thread(__unused void *parameter, __unused wait_result_t result)
{
	uint32_t steps = 0, delay = 45, watch = 30, keep = 0;

	PE_parse_boot_argn("adsp", &steps, sizeof(steps));
	PE_parse_boot_argn("adsp_delay", &delay, sizeof(delay));
	PE_parse_boot_argn("adsp_watch", &watch, sizeof(watch));
	PE_parse_boot_argn("adsp_keep", &keep, sizeof(keep));
	sleep_ms(delay * 1000);
	say("adsp: the audio DSP of the SM7150, steps 1 to %u\n", steps);
	if (step_look() && steps >= 2 && step_ours() && steps >= 3 && step_start() && steps >= 4) {
		step_watch(watch, steps);
	}
	if (!keep) {
		adsp_shutdown("the steps are over");
	}
	say("adsp: done\n");
	thread_terminate(current_thread());
}

/* Called by loader_return() on the way back to the loader: the amplifiers into reset, then the DSP shut down by TrustZone. */
void hakios_adsp_quiesce(void);
void
hakios_adsp_quiesce(void)
{
	uint64_t answer = 0;

	say_nothing = true;
	hakios_amp_quiesce();
	if (dsp_started) {
		(void)scm("shutdown at the run's end", SCM_64 | SCM_SIP | SCM_PAS_SHUTDOWN, SCM_ONE_VALUE, PAS_ADSP, 0, 0, &answer);
		dsp_started = false;
	}
}

static thread_call_t adsp_call;

static void
adsp_go(__unused thread_call_param_t p0, __unused thread_call_param_t p1)
{
	thread_t thread;
	uint32_t steps = 0;

	if (PE_parse_boot_argn("adsp", &steps, sizeof(steps)) && steps != 0 &&
	    kernel_thread_start(adsp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

__startup_func
static void
hakios_adsp_sm7150_init(void)
{
	uint64_t deadline;

	adsp_call = thread_call_allocate(adsp_go, NULL);
	clock_interval_to_deadline(3, NSEC_PER_SEC, &deadline);
	thread_call_enter_delayed(adsp_call, deadline);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_adsp_sm7150_init);
