/*
 * Darwin-on-hakios: the kernel's side of the Wi-Fi driver's object.
 *
 * The driver of the phone's radio is Linux's ath11k, compiled unmodified outside this tree on a
 * layer that gives it the kernel interfaces it expects, and tied into one object that the
 * kernel links when it is there (tools/mk-ath11k.sh and wifi/lx in the repository; the kernel
 * is built with that object when xnu-build.sh finds it). The object shows one name, lx_start(),
 * and asks for the calls of sys/hakios.lx.h, which are here: memory, time, threads, one lock
 * to sleep under, the kernel's options. What it asks of the radio itself -- its firmware's
 * files, its view of memory, the processor its firmware runs on -- is hakios.wifi.c's to
 * answer, which knows the phone.
 *
 * A kernel built without the object has the lx_start() at the end of this file, which says so.
 */
#include <kern/clock.h>
#include <kern/kalloc.h>
#include <kern/locks.h>
#include <kern/sched_prim.h>
#include <kern/thread.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/random.h>
#include <sys/hakios.input.h>
#include <sys/hakios.lx.h>
#include <sys/hakios.net.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map_unmappable(vm_offset_t phys_addr, vm_size_t size, unsigned int flags);
extern uint64_t kvtophys(vm_offset_t va);       /* pmap_paddr_t */
extern kern_return_t thread_terminate(thread_t thread);
extern void thread_set_thread_name(thread_t thread, const char *name);
extern bool umi_wifi_is_board(void);
extern bool umi_wifi_radio_running(void);
extern int umi_wifi_radio_boot(void);
extern void umi_wifi_radio_reset(void);

#define say hakios_wifi_say

static LCK_GRP_DECLARE(lx_lock_group, "hakios-lx");
static LCK_MTX_DECLARE(lx_lock, &lx_lock_group);

extern void hakios_tl(const char *, ...) __printflike(1, 2);

void
lx_os_ap_trace(const char *stage, unsigned subject, int value)
{
	/* Diagnostic cache only: collisions cause extra lines, never suppress a new
	 * signature. Atomics keep probes independent of the locks being diagnosed. */
	static uint64_t seen[512];
	uint64_t second = lx_os_nanoseconds() / 1000000000ull;
	uint64_t key = ((uintptr_t)stage >> 2) ^ ((uint64_t)subject * 0x9e3779b1u) ^
	    ((uint64_t)(unsigned)value * 0x85ebca6bu) ^ (second * 0xc2b2ae35u) ^
	    thread_tid(current_thread());
	if (os_atomic_xchg(&seen[key % 512], key, relaxed) != key)
		hakios_tl("ap-trace: tid=%llu %s subject=%u value=%d\n",
		    thread_tid(current_thread()), stage, subject, value);
}

/* ---- memory ------------------------------------------------------------------------------------------ */

void *
lx_os_alloc(unsigned long size)
{
	return kalloc_data(size, Z_WAITOK | Z_ZERO);
}

void
lx_os_free(void *memory, unsigned long size)
{
	kfree_data(memory, size);
}

/*
 * In one physical piece, so that it can be mapped a second time without this processor's cache
 * in between (as hakios.adreno.c does with what it shares with the GPU); the first, cached
 * view is written out and never used again.
 */
void *
lx_os_radio_memory(unsigned long size, unsigned long long *address)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;

	size = round_page(size);
	if (kmem_alloc_contig(kernel_map, &cached, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		say("lx: no %lu bytes of memory in one piece\n", size);
		return NULL;
	}
	physical = kvtophys(cached);
	bzero((void *)cached, size);
	for (vm_offset_t line = cached; line < cached + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	uncached = ml_io_map_unmappable(physical, size, VM_WIMG_WCOMB);
	if (uncached == 0 || !hakios_wifi_radio_map(physical, size, address)) {
		return NULL;
	}
	return (void *)uncached;
}

void *
lx_os_radio_memory_at(unsigned long size, unsigned long long address)
{
	vm_offset_t cached = 0, uncached;
	uint64_t physical;

	size = round_page(size);
	if (kmem_alloc_contig(kernel_map, &cached, size, 0, 0, 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) !=
	    KERN_SUCCESS) {
		say("lx: no %lu bytes of memory in one piece\n", size);
		return NULL;
	}
	physical = kvtophys(cached);
	bzero((void *)cached, size);
	for (vm_offset_t line = cached; line < cached + size; line += 64) {
		__asm__ volatile ("dc civac, %0" : : "r" (line));
	}
	__asm__ volatile ("dsb sy");
	uncached = ml_io_map_unmappable(physical, size, VM_WIMG_WCOMB);
	if (uncached == 0 || !hakios_wifi_radio_map_at(physical, size, address)) {
		return NULL;
	}
	return (void *)uncached;
}

void *
lx_os_map_registers(unsigned long long physical, unsigned long size)
{
	unsigned long long page = trunc_page(physical);
	vm_offset_t mapped = ml_io_map_unmappable(page, round_page(physical + size) - page, VM_WIMG_IO);

	return mapped == 0 ? NULL : (void *)(mapped + (physical - page));
}

/* ---- saying things ----------------------------------------------------------------------------------- */

void
lx_os_print(const char *text)
{
	say("%s", text);
}

void
lx_os_tell(const char *text)
{
	hakios_wifi_tell("%s", text);
}

void
lx_os_panic(const char *text)
{
	/*
	 * Said here, through the log the loader reads from another core, and before the panic: the
	 * kernel's own text goes to its message and panic buffers, which only reach that log on the
	 * way back to the loader -- and a run that stops inside the panic path never gets there.
	 */
	hakios_tl("lx: PANIC: %.150s\n", text);
	panic("%s", text);
}

/* ---- time, threads, sleeping --------------------------------------------------------------------------- */

unsigned long long
lx_os_nanoseconds(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds;
}

void *
lx_os_thread_self(void)
{
	return current_thread();
}

struct lx_thread {
	void (*function)(void *);
	void *argument;
};

static void
lx_thread_runs(void *parameter, __unused wait_result_t result)
{
	struct lx_thread start = *(struct lx_thread *)parameter;

	kfree_type(struct lx_thread, parameter);
	start.function(start.argument);
	thread_terminate(current_thread());
}

int
lx_os_thread(const char *name, void (*function)(void *), void *argument)
{
	struct lx_thread *start = kalloc_type(struct lx_thread, Z_WAITOK | Z_ZERO);
	thread_t thread;

	if (start == NULL) {
		return ENOMEM;
	}
	start->function = function;
	start->argument = argument;
	if (kernel_thread_start(lx_thread_runs, start, &thread) != KERN_SUCCESS) {
		kfree_type(struct lx_thread, start);
		return EAGAIN;
	}
	thread_set_thread_name(thread, name);
	thread_deallocate(thread);
	return 0;
}

void
lx_os_lock(void)
{
	lck_mtx_lock(&lx_lock);
}

void
lx_os_unlock(void)
{
	lck_mtx_unlock(&lx_lock);
}

int
lx_os_sleep(const void *address, unsigned long long nanoseconds)
{
	uint64_t deadline;

	if (nanoseconds == 0) {
		return lck_mtx_sleep(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT) == THREAD_TIMED_OUT;
	}
	nanoseconds_to_absolutetime(nanoseconds, &deadline);
	deadline += mach_absolute_time();
	return lck_mtx_sleep_deadline(&lx_lock, LCK_SLEEP_DEFAULT, (event_t)address, THREAD_UNINT, deadline) == THREAD_TIMED_OUT;
}

void
lx_os_wakeup(const void *address)
{
	thread_wakeup((event_t)address);
}

/* ---- the rest of the machine ------------------------------------------------------------------------- */

void
lx_os_random(void *buffer, unsigned long size)
{
	read_random(buffer, (u_int)size);
}

extern int umi_pm_wifi_policy(void);

/*
 * A switch on the data volume, read the way the governor's are (hakios.cpufreq.c: cpugov_switch()):
 * a regular file that is there means "on". 1: there, 0: not there, -1: there is no root file system
 * yet, which is a different answer and is said as such -- the driver's start comes early, and the
 * difference tells a reader of the log whether the file can have been looked at at all.
 */
static int
lx_os_dbfile(const char *name)
{
	char path[96];
	vnode_t node = NULLVP;
	int present;

	if (rootvnode == NULLVP) {
		return (-1);
	}
	snprintf(path, sizeof(path), "/private/var/db/%s", name);
	if (vnode_lookup(path, 0, &node, vfs_context_kernel()) != 0) {
		return (0);
	}
	present = vnode_isreg(node);
	vnode_put(node);
	return (present ? 1 : 0);
}

unsigned
lx_os_option(const char *name)
{
	if (umi_wifi_is_board()) {
		if (strcmp(name, "umi_screenoff_wifi_policy") == 0) return (unsigned)(umi_pm_wifi_policy() + 1);
		if (strcmp(name, "wifi_umi") == 0) return 1;
		if (strcmp(name, "wifi_lx") == 0) return 2;
		if (strcmp(name, "wifi_irq") == 0) return 0;
		if (strcmp(name, "wifi_imps") == 0) {
			/*
			 * Whether the firmware may take the radio into IMPS: the default is no, because
			 * nothing here can wake the chip again (work/v29f/INVESTIGATION-hotspot-hang2.md,
			 * H1a). /private/var/db/umi-wifi-imps-on restores the old behaviour for the
			 * experiment. Asked once, when the driver starts (lx_bus.c: lx_start()); the radio
			 * is not told again, so a change of the file takes a run. The line says what was
			 * found -- including that there was no file system to look in yet.
			 */
			int imps = lx_os_dbfile("umi-wifi-imps-on");

			hakios_tl("lx: IMPS: the firmware %s enter it (%s)\n", imps > 0 ? "may" : "may not",
			    imps > 0 ? "/private/var/db/umi-wifi-imps-on is there" : imps == 0 ?
			    "no /private/var/db/umi-wifi-imps-on" : "NO FILE SYSTEM YET to look for /private/var/db/umi-wifi-imps-on in");
			return (imps > 0 ? 1u : 0u);
		}
		if (strcmp(name, "wifi_scan") == 0)
			return umi_wifi_normal_session() ? hakios_wifi_option(name) : 1;
		if (strcmp(name, "wifi_station") == 0) return umi_wifi_normal_session() ? 1 : 0;
		if (strcmp(name, "wifi_ba") == 0 || strcmp(name, "wifi_wide") == 0) return 1;
		if (strcmp(name, "wifi_quiet") == 0) return 1;
	}
	return hakios_wifi_option(name);
}

int
lx_os_firmware(const char *name, const void **data, unsigned long *size)
{
	uint64_t length = 0;
	const uint8_t *file = hakios_wifi_driver_file(name, &length);

	if (file == NULL) {
		return ENOENT;
	}
	*data = file;
	*size = (unsigned long)length;
	return 0;
}

int
lx_os_rproc_boot(void)
{
	if (umi_wifi_is_board()) return umi_wifi_radio_boot();
	return hakios_wifi_dsp_is_up() ? 0 : ENXIO;
}

void
lx_os_rproc_shutdown(void)
{
	if (umi_wifi_is_board()) umi_wifi_radio_reset();
}

int
lx_os_radio_address(unsigned char *address)
{
	return hakios_wifi_radio_address(address);
}

int
lx_os_network(unsigned char *name, unsigned *name_length, unsigned char *passphrase, unsigned *passphrase_length)
{
	return hakios_wifi_network(name, name_length, passphrase, passphrase_length);
}

/* ---- frames: the radio as the link behind en0 (sys/hakios.net.h) ------------------------------------ */

/*
 * A frame from the network stack comes on whatever thread sends it, and the driver's side keeps
 * a record of every thread that enters it: so frames are put in a queue here, and one thread of
 * ours takes them to the driver. A queue that is full drops what comes (TCP sends it again).
 */
#define LX_QUEUE        256

static LCK_MTX_DECLARE(lx_mode_lock, &lx_lock_group);
static const struct lx_ap_ops *lx_ap_ops;
static unsigned lx_ap_generation;

struct lx_frame {
	uint16_t length;
	bool ap;
	unsigned generation;
	uint8_t bytes[HAKIOS_NET_FRAME_MAX];
};

static bool lx_link_attached;
static LCK_MTX_DECLARE(lx_queue_lock, &lx_lock_group);
static struct lx_frame *lx_queue;
static uint32_t lx_queue_in, lx_queue_out;      /* frames put in and taken out, counted from the start */
static uint64_t lx_queue_dropped;

enum { AP_QUEUE_IN, AP_QUEUE_FULL, AP_QUEUE_COPY, AP_QUEUE_STALE, AP_QUEUE_SENT, AP_QUEUE_REJECT, AP_QUEUE_COUNT };
static uint64_t lx_ap_queue_count[AP_QUEUE_COUNT];
static void
lx_ap_queue_event(unsigned event, int error)
{
	static const char *names[] = { "queued", "full", "copy", "stale", "submitted", "rejected" };
	uint64_t count = os_atomic_inc_orig(&lx_ap_queue_count[event], relaxed) + 1;
	if (count <= 8 || !(count & (count - 1)))
		hakios_wifi_tell("hotspot-ap: queue stage=%s count=%llu error=%d\n", names[event], count, error);
}

static void
lx_queue_runs(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		struct lx_frame *frame;

		lck_mtx_lock(&lx_queue_lock);
		while (lx_queue_in == lx_queue_out) {
			lck_mtx_sleep(&lx_queue_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_queue_in, THREAD_UNINT);
		}
		frame = &lx_queue[lx_queue_out % LX_QUEUE];     /* (stays ours until lx_queue_out moves) */
		lck_mtx_unlock(&lx_queue_lock);
		lck_mtx_lock(&lx_mode_lock);
		if (!frame->ap) {
			lx_transmit(frame->bytes, frame->length);
		} else if (lx_ap_ops != NULL && frame->generation == lx_ap_generation) {
			int error = lx_ap_ops->send(frame->bytes, frame->length);
			lx_ap_queue_event(error ? AP_QUEUE_REJECT : AP_QUEUE_SENT, error);
		} else lx_ap_queue_event(AP_QUEUE_STALE, ENETDOWN);
		lck_mtx_unlock(&lx_mode_lock);
		lck_mtx_lock(&lx_queue_lock);
		lx_queue_out++;
		lck_mtx_unlock(&lx_queue_lock);
	}
}

static bool
lx_link_transmit_common(mbuf_t packet, size_t length, bool ap)
{
	struct lx_frame *frame;

	lck_mtx_lock(&lx_queue_lock);
	if (length > HAKIOS_NET_FRAME_MAX || lx_queue_in - lx_queue_out == LX_QUEUE) {
		if (ap) lx_ap_queue_event(AP_QUEUE_FULL, ENOBUFS);
		lx_queue_dropped++;
		lck_mtx_unlock(&lx_queue_lock);
		return false;
	}
	frame = &lx_queue[lx_queue_in % LX_QUEUE];
	if (mbuf_copydata(packet, 0, length, frame->bytes) != 0) {
		if (ap) lx_ap_queue_event(AP_QUEUE_COPY, EINVAL);
		lck_mtx_unlock(&lx_queue_lock);
		return false;
	}
	frame->length = (uint16_t)length;
	frame->ap = ap;
	frame->generation = os_atomic_load(&lx_ap_generation, relaxed);
	lx_queue_in++;
	if (ap) lx_ap_queue_event(AP_QUEUE_IN, 0);
	lck_mtx_unlock(&lx_queue_lock);
	thread_wakeup((event_t)&lx_queue_in);
	return true;
}

static bool
lx_link_transmit(mbuf_t packet, size_t length)
{
	return lx_link_transmit_common(packet, length, false);
}

static struct hakios_net_link lx_link = {
	.name = "the phone's radio (ath11k)",
	.wireless = true,
	.transmit = lx_link_transmit,
};

void
lx_os_net_attach(const unsigned char *address)
{
	thread_t thread;

	if (lx_link_attached) {
		return;
	}
	lx_queue = kalloc_data(LX_QUEUE * sizeof(*lx_queue), Z_WAITOK | Z_ZERO);
	if (lx_queue == NULL || kernel_thread_start(lx_queue_runs, NULL, &thread) != KERN_SUCCESS) {
		say("lx: no memory or no thread for the frames to send: the radio's frames go nowhere\n");
		return;
	}
	thread_set_thread_name(thread, "wifi-send");
	thread_deallocate(thread);
	memcpy(lx_link.address, address, sizeof(lx_link.address));
	lx_link_attached = hakios_net_attach(&lx_link);
	say("lx: %s\n", lx_link_attached ? "the radio is the link behind en0" : "EN0 HAS A LINK ALREADY: the radio's frames go "
	    "nowhere");
}

/*
 * With wifi=1 the interface is made when the kernel starts, before the radio is up: iOS's Wi-Fi
 * side looks for it and for its registry entry as soon as it runs, once, and a radio that
 * turns up twelve seconds later is one it has already failed to open.
 */
void
hakios_lx_net_early(void)
{
	uint8_t address[6];

	if (hakios_wifi_radio_address(address)) {
		lx_os_net_attach(address);
	}
}

void
lx_os_net_link(int up)
{
	if (lx_link_attached) {
		hakios_net_link_state(up != 0);
	}
}

void
lx_os_net_input(const unsigned char *frame, unsigned long length)
{
	if (lx_link_attached && length <= HAKIOS_NET_FRAME_MAX) {
		hakios_net_input(frame, length);
	}
}

#include "umi_wifi_ap.inc"

/* ---- the radio's interrupts ------------------------------------------------------------------------------- */

extern kern_return_t gic_v3_spi_attach(uint32_t intid, bool edge, void (*handler)(void *context, uint32_t intid), void *context);
extern void gic_v3_spi_mask(uint32_t intid);
extern void gic_v3_spi_unmask(uint32_t intid);
extern void gic_v3_spi_detach(uint32_t intid);
extern uint64_t gic_v3_spi_count(uint32_t intid);

#define LX_VECTORS      32

static struct {
	void (*function)(void *);
	void *argument;
	bool attached;
} lx_vector[LX_VECTORS];
static uint32_t lx_irq_first;                   /* the GIC's number of vector 0; 0: no interrupts */
static uint32_t lx_irq_pending;                 /* the vectors that have fired and are not dealt with yet */
static uint32_t lx_irq_running;                 /* the vectors whose function the thread is in */
static thread_t lx_irq_thread;
static LCK_MTX_DECLARE(lx_irq_lock, &lx_lock_group);

/* In interrupt context: the vector masked, and the thread told. */
static void
lx_irq_arrived(__unused void *context, uint32_t intid)
{
	gic_v3_spi_mask(intid);
	os_atomic_or(&lx_irq_pending, 1u << (intid - lx_irq_first), relaxed);
	thread_wakeup((event_t)&lx_irq_pending);
}

static void
lx_irq_runs(__unused void *parameter, __unused wait_result_t result)
{
	for (;;) {
		uint32_t pending;

		for (;;) {
			assert_wait((event_t)&lx_irq_pending, THREAD_UNINT);
			if ((pending = os_atomic_xchg(&lx_irq_pending, 0, acquire)) != 0) {
				clear_wait(current_thread(), THREAD_AWAKENED);
				break;
			}
			thread_block(THREAD_CONTINUE_NULL);
		}
		for (unsigned v = 0; v < LX_VECTORS; v++) {
			if (!(pending & 1u << v)) {
				continue;
			}
			lck_mtx_lock(&lx_irq_lock);
			if (!lx_vector[v].attached) {
				lck_mtx_unlock(&lx_irq_lock);
				continue;
			}
			lx_irq_running |= 1u << v;
			lck_mtx_unlock(&lx_irq_lock);
			lx_vector[v].function(lx_vector[v].argument);
			lck_mtx_lock(&lx_irq_lock);
			lx_irq_running &= ~(1u << v);
			lck_mtx_unlock(&lx_irq_lock);
			thread_wakeup((event_t)&lx_irq_running);
		}
	}
}

int
lx_os_irq_msi(unsigned long long *address, unsigned *data)
{
	uint32_t first = 0;

	if (!hakios_wifi_radio_msi(address, data, &first)) {
		return 0;
	}
	lx_irq_first = first;
	return 1;
}

int
lx_os_irq_attach(unsigned vector, void (*function)(void *), void *argument)
{
	kern_return_t kr;

	if (vector >= LX_VECTORS || lx_irq_first == 0 || function == NULL) {
		return EINVAL;
	}
	if (lx_irq_thread == THREAD_NULL) {
		hakios_wifi_tell("lx: wifi_irq: THE GIC'S DISTRIBUTOR IS WRITTEN for the radio's interrupts: each vector's SPI (%u + v) edge-"
		    "triggered, group 1, routed to this processor, then enabled\n", lx_irq_first);
		if (kernel_thread_start(lx_irq_runs, NULL, &lx_irq_thread) != KERN_SUCCESS) {
			lx_irq_thread = THREAD_NULL;
			return ENOMEM;
		}
		thread_set_thread_name(lx_irq_thread, "wifi-irq");
	}
	lck_mtx_lock(&lx_irq_lock);
	lx_vector[vector].function = function;
	lx_vector[vector].argument = argument;
	lx_vector[vector].attached = true;
	lck_mtx_unlock(&lx_irq_lock);
	if ((kr = gic_v3_spi_attach(lx_irq_first + vector, true, lx_irq_arrived, NULL)) != KERN_SUCCESS) {
		lck_mtx_lock(&lx_irq_lock);
		lx_vector[vector].attached = false;
		lck_mtx_unlock(&lx_irq_lock);
		return kr == KERN_NAME_EXISTS ? EBUSY : EINVAL;
	}
	gic_v3_spi_mask(lx_irq_first + vector);
	return 0;
}

void
lx_os_irq_enable(unsigned vector)
{
	if (vector < LX_VECTORS && lx_vector[vector].attached) {
		gic_v3_spi_unmask(lx_irq_first + vector);
	}
}

void
lx_os_irq_disable(unsigned vector)
{
	if (vector >= LX_VECTORS || !lx_vector[vector].attached) {
		return;
	}
	gic_v3_spi_mask(lx_irq_first + vector);
	if (current_thread() == lx_irq_thread) {
		return;                         /* (from the vector's own function, or another's: nothing to wait for) */
	}
	lck_mtx_lock(&lx_irq_lock);
	while (lx_irq_running & 1u << vector) {
		lck_mtx_sleep(&lx_irq_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_irq_running, THREAD_UNINT);
	}
	lck_mtx_unlock(&lx_irq_lock);
}

void
lx_os_irq_detach_all(void)
{
	for (unsigned v = 0; v < LX_VECTORS; v++) {
		if (lx_vector[v].attached) {
			gic_v3_spi_detach(lx_irq_first + v);
		}
	}
	lck_mtx_lock(&lx_irq_lock);
	for (unsigned v = 0; v < LX_VECTORS; v++) {
		lx_vector[v].attached = false;
	}
	while (lx_irq_running != 0 && current_thread() != lx_irq_thread) {
		lck_mtx_sleep(&lx_irq_lock, LCK_SLEEP_DEFAULT, (event_t)&lx_irq_running, THREAD_UNINT);
	}
	lck_mtx_unlock(&lx_irq_lock);
	os_atomic_store(&lx_irq_pending, 0, relaxed);
}

unsigned long long
lx_os_irq_count(unsigned vector)
{
	return vector < LX_VECTORS && lx_irq_first != 0 ? gic_v3_spi_count(lx_irq_first + vector) : 0;
}

/* ---- the station (sys/hakios.sta.h) --------------------------------------------------------------------- */

void
lx_os_station(const struct hakios_sta_ops *ops)
{
	hakios_sta_attach(ops);
}

void
lx_os_station_lost(int error)
{
	hakios_sta_lost(error);
}

/* ---- the radio's last accesses (sys/hakios.lx.h, struct lx_mmio_ring) ------------------------------------ */

/*
 * The driver's bus writes down its last accesses of the chip's registers (wifi/lx/src/lx_bus.c)
 * and offers the ring here once, when the driver starts. The kernel only passes the pointer on:
 * the loader reads it from another processor when this kernel has stopped answering, and prints
 * the entry still pending -- the access that never came back (osfmk/arm64/loader_return.c,
 * devices/xiaomi-mi-10/loader/loader.c).
 */
extern void loader_live_mmio(const void *ring);		/* osfmk/arm64/loader_return.c */

void
lx_os_mmio_ring(struct lx_mmio_ring *ring, unsigned long size)
{
	if (ring == NULL || size < sizeof(*ring) || ring->magic != LX_MMIO_MAGIC) {
		return;					/* (not the structure this kernel knows: say nothing) */
	}
	loader_live_mmio(ring);
}

/*
 * The bus is inside one access on this processor (lx_os_mmio_begin() in bsd/sys/hakios.lx.h):
 * wifi/lx/src/lx_bus.c sets it around every access it records, so that the loader's hang report
 * can say which processor an unfinished ring entry belongs to. The three captures live in
 * osfmk/arm64 (umi_cpu_pc.inc, which that side cannot include from here: bsd/ does not get
 * osfmk's private include path); they are declared here as loader_return.c's are.
 */
extern void umi_cpu_pc_mmio_begin(uint64_t seq, uint64_t when_ns);	/* osfmk/arm64/umi_cpu_pc.inc */
extern void umi_cpu_pc_mmio_end(uint64_t seq);

void
lx_os_mmio_begin(unsigned long long seq, unsigned long long when_ns)
{
	umi_cpu_pc_mmio_begin((uint64_t)seq, (uint64_t)when_ns);
}

void
lx_os_mmio_end(unsigned long long seq)
{
	umi_cpu_pc_mmio_end((uint64_t)seq);
}

/* ---- the driver ---------------------------------------------------------------------------------------- */

/* The object has its own; this is the one of a kernel built without it. */
__attribute__((weak)) int
lx_start(void)
{
	say("lx: THE DRIVER IS NOT BUILT IN: its object was not there when this kernel was built (tools/mk-ath11k.sh, then "
	    "build the kernel again)\n");
	return ENXIO;
}

__attribute__((weak)) void
lx_stop(void)
{
}

/* (a kernel without the object has no registers to refuse: nothing to do) */
__attribute__((weak)) void
lx_quiesce_registers(void)
{
}

__attribute__((weak)) int
lx_prepare(void)
{
	return 0;
}

__attribute__((weak)) int
lx_transmit(__unused const unsigned char *frame, __unused unsigned long length)
{
	return ENXIO;
}

int
hakios_lx_start(void)
{
	return lx_start();
}

void
hakios_lx_stop(void)
{
	lx_stop();
}

void
hakios_lx_quiesce_registers(void)
{
	lx_quiesce_registers();
}

int
hakios_lx_prepare(void)
{
	return lx_prepare();
}
