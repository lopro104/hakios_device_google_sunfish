/*
 * The audio DSP (ADSP, a Hexagon inside LPASS) of the SM7150 in the Pixel 4a: started through TrustZone and linked.
 * Milestone 0 of sound and of the sensors (which this phone has behind the same DSP): the DSP runs, says so over SMP2P,
 * and opens its GLINK channels, whose names are written down. Nothing of audio is played yet.
 *
 * Status: COMPILED ONLY (2026-11-16). Not run on the phone.
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

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern void ml_io_unmap(vm_offset_t addr, vm_size_t sz);
extern int hakios_rpmh_crypto_vote(uint32_t vote);
extern int hakios_rpmh_arc_votes(const char *const *res, const uint32_t *level, unsigned n);
extern int hakios_rpmh_ddr_vote(uint32_t vote, bool send);
extern uint64_t kvtophys(vm_offset_t va);
extern void loader_log_putc(char c);
extern kern_return_t thread_terminate(thread_t);
extern bool hakios_sm7150_aop_send(const char *msg);            /* hakios.mpss_sm7150.c: the one mailbox, the one sender */

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

static void
say(const char *format, ...)
{
	char line[256];
	va_list arguments;

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
	uint8_t padded[64];

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

static void
glink_poll(void)
{
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
			say("adsp: GLINK: THE DSP OPENS CHANNEL \"%s\" (its id %u); acknowledged, not opened from here yet\n", name,
			    msg.param1);
			(void)tx(&ack, sizeof(ack));
		} else {
			/* anything else belongs to a channel we have not opened: said once, and the queue emptied */
			say("adsp: GLINK: command %u (%u, %#x) with %u bytes waiting: not handled in this step, queue dropped\n", msg.cmd,
			    msg.param1, msg.param2, have);
			rx_advance(have);
		}
	}
}

static void
step_watch(uint32_t seconds)
{
	struct glink_msg version = { .cmd = 0, .param1 = 1, .param2 = 1 };
	bool up = false, linked = false;
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
		}
	}
	say("adsp: watched %u s: SMP2P %s, GLINK %s, %u channels opened by the DSP\n", seconds, up ? "up" : "NOT up",
	    linked ? "linked" : "NOT linked", channels);
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
		step_watch(watch);
	}
	if (!keep) {
		adsp_shutdown("the steps are over");
	}
	say("adsp: done\n");
	thread_terminate(current_thread());
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
