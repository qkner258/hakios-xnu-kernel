/*
 * The clock speed of the processor cores the kernel runs on, on the Xiaomi Mi 10 (UMI).
 * Only with boot-arg cpufreq=1: the addresses are this phone's.
 *
 * The boot loader leaves each group of cores at a modest frequency and expects the operating
 * system to ask for more. On this SoC that is one register write: the hardware ("EPSS") has
 * a table of frequencies per group and switches the clock source and the supply voltage by
 * itself when told which row is wanted. With cpufreq=1 the governor is enabled by
 * default; cpugov=0 selects fixed mode for diagnosis.
 *
 * Register layout: Linux drivers/cpufreq/qcom-cpufreq-hw.c ("epss" variant).
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <os/atomic_private.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <machine/machine_routines.h>
#include <pexpert/pexpert.h>
#include <sys/hakios.input.h>
#include <sys/hakios.board.h>
#include <sys/systm.h>
#include <sys/vnode.h>
#include <sys/vnode_internal.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);
extern vm_offset_t ml_io_map_wcomb(vm_offset_t phys_addr, vm_size_t size);
extern void loader_log_putc(char c);
extern void hakios_tl(const char *format, ...) __printflike(1, 2);
extern bool ml_cpu_idle_time(int cpu_id, uint64_t *idle);
extern void umi_idle_set_measurement(bool enabled);
extern bool umi_idle_snapshot(unsigned cpu, uint64_t *entries, uint64_t *ticks);

/*
 * The loader's log as well: this runs early, and the kernel's own log of a run keeps only its end. With
 * interrupts off -- on the way back to the loader (hakios_ddr_quiesce()) -- the loader's log only: the
 * kernel's console may not be written there, and printf was a panic ("Console I/O from interrupt-disabled
 * context") at the end of every run once the memory vote was taken away on the way back.
 */
static void
tell(const char *format, ...) __printflike(1, 2);

static void
tell(const char *format, ...)
{
	char line[256];
	va_list list;

	va_start(list, format);
	vsnprintf(line, sizeof(line), format, list);
	va_end(list);
	for (const char *c = line; *c != 0; c++) {
		loader_log_putc(*c);
	}
	if (ml_get_interrupts_enabled()) {
		printf("%s", line);
	}
}

#define EPSS_PHYS               hakios_board_addr("epss", 0x18591000UL)    /* three groups ("domains") of 0x1000: small cores, big, biggest */
#define EPSS_DOMAIN_SIZE        0x1000UL
#define EPSS_DOMAINS            3
#define EPSS_FREQ_TABLE         0x100           /* a word per row: [31:30] clock source, [7:0] multiple of 19.2 MHz */
#define EPSS_TABLE_ROWS         40
#define EPSS_PERF_STATE         0x320           /* the row that is wanted */
#define XO_KHZ                  19200
#define ALTERNATE_KHZ           300000          /* source 0: a fixed 300 MHz */
/*
 * The most an event boost holds, whatever the caller asks for (present and frame deadlines
 * ask for 24 ms, GPU submission 32, touch 40). A boost raises the row at once, before the
 * work starts, and the caller's group stays there for this long after its last event: long
 * enough for the frame or the touch it was for, short enough that a 40-50 fps stream of
 * events does not keep a whole group at its top row with no load to show for it.
 */
#define CPUGOV_BOOST_MAX_MS     8

/* How fast a simple loop runs, in thousands of iterations per millisecond. */
static uint32_t
loop_speed(void)
{
	volatile uint64_t sink = 0;
	uint64_t start = mach_absolute_time(), nanos;

	for (uint32_t i = 0; i < 2000000; i++) {
		sink += i;
	}
	absolutetime_to_nanoseconds(mach_absolute_time() - start, &nanos);
	return nanos ? (uint32_t)(2000000ULL * 1000 / nanos) : 0;
}

/* The group of the core with that number in its MPIDR (the device tree's "reg": the number is the second field). */
static uint32_t
domain_of(uint32_t phys_id)
{
	uint32_t core = (phys_id >> 8) & 0xff;

	return core < 4 ? 0 : core < 7 ? 1 : 2;
}

/* each group's registers and the highest row of its table, once raised: for the thermal service's caps */
static vm_offset_t epss_base[EPSS_DOMAINS];
static int epss_top[EPSS_DOMAINS] = { -1, -1, -1 };
static uint32_t epss_khz[EPSS_DOMAINS][EPSS_TABLE_ROWS];
static int epss_cap[EPSS_DOMAINS] = { -1, -1, -1 };
static int epss_sleep_cap[EPSS_DOMAINS] = { -1, -1, -1 };
static int epss_row[EPSS_DOMAINS] = { -1, -1, -1 };
static uint32_t cpugov_low_samples[EPSS_DOMAINS];
static uint64_t cpugov_changed[EPSS_DOMAINS];
static bool cpugov_enabled;
static uint64_t cpugov_boost_until[EPSS_DOMAINS];
static bool cpugov_local_boost = true;          /* the caller's group only: /private/var/db/umi-thermal-global-boost lifts this */
static uint64_t cpugov_boost_calls;
static int cpugov_wait;
static void ddr_switch_update(void);            /* (defined with the memory vote below; looked at from the one-second poll too) */

/* Read only by the sleeping governor worker, never in a submission/IRQ path.
 * Marker files live on persistent /var; no loader/boot-argument change. */
static bool
cpugov_switch(const char *path)
{
	vnode_t node = NULLVP;
	if (rootvnode == NULLVP || vnode_lookup(path, 0, &node, vfs_context_kernel()) != 0) return false;
	bool enabled = vnode_isreg(node);
	vnode_put(node);
	return enabled;
}

static void
raise_domain(uint32_t domain, uint32_t cores)
{
	uint32_t top = 0, top_khz = 0, was;
	vm_offset_t base;

	base = ml_io_map(EPSS_PHYS + domain * EPSS_DOMAIN_SIZE, EPSS_DOMAIN_SIZE);
	for (uint32_t row = 0; row < EPSS_TABLE_ROWS; row++) {
		uint32_t entry = *(volatile uint32_t *)(base + EPSS_FREQ_TABLE + 4 * row);
		uint32_t khz = (entry >> 30) ? XO_KHZ * (entry & 0xff) : ALTERNATE_KHZ;

		if (row > 0 && khz <= top_khz) {
			break;                  /* the table ends by repeating its last row */
		}
		top = row;
		top_khz = khz;
		epss_khz[domain][row] = khz;
	}
	epss_base[domain] = base;
	epss_top[domain] = (int)top;
	epss_cap[domain] = (int)top;
	epss_sleep_cap[domain] = (int)top;
	epss_row[domain] = (int)top;
	was = *(volatile uint32_t *)(base + EPSS_PERF_STATE);
	*(volatile uint32_t *)(base + EPSS_PERF_STATE) = top;
	delay_for_interval(20, NSEC_PER_MSEC);
	tell("cpufreq: group %u (%u of our cores): row %u -> %u of the frequency table, %u MHz\n", domain, cores, was, top,
	    top_khz / 1000);
}

/*
 * For the thermal service (bsd/dev/hakios.thermal.c): the highest row of a group's table (-1 when the
 * group was not raised -- none of our cores in it, or cpufreq=0), how fast a row is, and a cap: the
 * row the group is to run at, never above its top. One register write, as raising it was; EPSS moves
 * the clock and the voltage by itself.
 */
int
hakios_cpufreq_top(uint32_t domain)
{
	return domain < EPSS_DOMAINS ? epss_top[domain] : -1;
}

/*
 * Set a frequency ceiling in kHz. This is deliberately a small, exported hook:
 * the screen-off service can lower a domain without knowing EPSS row numbers,
 * and the governor takes the lower of the sleep and thermal ceilings.
 * khz == 0 restores the top row. The write is one EPSS performance-state write.
 */
#define UMI_SLEEP_HAS_CPUGOV 1
#define UMI_SLEEP_HAS_EXTERNAL_CAP 1
#include "umi_cpufreq_screenoff.h"

void
hakios_cpufreq_set_cap(uint32_t domain, uint32_t khz)
{
	int row = -1;

	if (domain >= EPSS_DOMAINS || epss_top[domain] < 0) {
		return;
	}
	if (khz == 0) {
		row = epss_top[domain];
	} else {
		for (int i = 0; i <= epss_top[domain]; i++) {
			if (epss_khz[domain][i] <= khz) {
				row = i;
			}
		}
		if (row < 0) {
			row = 0;
		}
	}
	lck_mtx_lock(&umi_freq_lock);
	epss_sleep_cap[domain] = row;
	int effective = umi_freq_limit(domain, MIN(epss_cap[domain], row));
	if (epss_row[domain] > effective) {
		epss_row[domain] = effective;
		*(volatile uint32_t *)(epss_base[domain] + EPSS_PERF_STATE) = (uint32_t)effective;
	}
	lck_mtx_unlock(&umi_freq_lock);
}

uint32_t
hakios_cpufreq_khz(uint32_t domain, int row)
{
	if (domain >= EPSS_DOMAINS || row < 0 || row > epss_top[domain]) {
		return 0;
	}
	return epss_khz[domain][row];
}

void
hakios_cpufreq_cap(uint32_t domain, int row)
{
	if (domain >= EPSS_DOMAINS || epss_top[domain] < 0) {
		return;
	}
	if (row < 0) {
		row = 0;
	}
	if (row > epss_top[domain]) {
		row = epss_top[domain];
	}
	lck_mtx_lock(&umi_freq_lock);
	epss_cap[domain] = row;
	row = umi_freq_limit(domain, row);
	/* Releasing a thermal ceiling must not raise an idle domain. */
	if (cpugov_enabled) row = MIN(row, epss_row[domain]);
	if (epss_row[domain] != row) {
		epss_row[domain] = row;
		*(volatile uint32_t *)(epss_base[domain] + EPSS_PERF_STATE) = (uint32_t)row;
	}
	lck_mtx_unlock(&umi_freq_lock);
}

void
hakios_cpufreq_boost(uint32_t milliseconds)
{
	uint64_t duration, now;

	if (!os_atomic_load(&cpugov_enabled, acquire) || milliseconds == 0) return;
	/* A touch/frame raises the row before its work starts. Repeated events
	 * only extend the hold, and never write an unchanged performance state. */
	nanoseconds_to_absolutetime((uint64_t)MIN(milliseconds, CPUGOV_BOOST_MAX_MS) * NSEC_PER_MSEC, &duration);
	lck_mtx_lock(&umi_freq_lock);
	now = mach_absolute_time();
	cpugov_boost_calls++;
	uint32_t caller = domain_of((uint32_t)__builtin_arm_rsr64("MPIDR_EL1"));
	for (uint32_t d = 0; d < EPSS_DOMAINS; d++) {
		if (epss_top[d] < 0) continue;
		/* Local (the default) raises the caller's own group only: an event on a
		 * fast core no longer drags the idle A55s up, nor a touch handler on an
		 * A55 the fast domains, and the other groups follow their measured load.
		 * umi-thermal-global-boost restores the old vote on all three. */
		if (cpugov_local_boost && d != caller) continue;
		cpugov_boost_until[d] = MAX(cpugov_boost_until[d], now + duration);
		int target = umi_freq_limit(d, epss_cap[d]);
		cpugov_low_samples[d] = 0;
		if (epss_row[d] < target) {
			epss_row[d] = target;
			*(volatile uint32_t *)(epss_base[d] + EPSS_PERF_STATE) = (uint32_t)target;
			cpugov_changed[d] = now;
		}
	}
	lck_mtx_unlock(&umi_freq_lock);
}

/* 20 ms load samples, immediate busy/foreground boost, two-sample downward
 * hysteresis. Target 60% occupancy; completely idle domains go straight to
 * row zero. Thermal and screen ceilings always win. Called under the lock. */
static void
cpugov_update(uint32_t d, uint32_t busy, uint64_t now, uint64_t interval)
{
	int row = epss_row[d], next = 0;
	int cap = umi_freq_limit(d, epss_cap[d]);

	if (now < cpugov_boost_until[d] || busy >= 70) {
		next = cap;
	} else if (busy > 15) {
		uint32_t khz = epss_khz[d][row] * busy / 60;
		while (next < cap && epss_khz[d][next] < khz) next++;
	}
	next = MIN(next, cap);
	if (row > cap) {
		next = cap;
	} else if (next < row) {
		if (++cpugov_low_samples[d] < 2 || now - cpugov_changed[d] < interval) return;
	} else {
		cpugov_low_samples[d] = 0;
		if (now - cpugov_changed[d] < interval) return;
	}
	if (next != row) {
		cpugov_low_samples[d] = 0;
		epss_row[d] = next;
		*(volatile uint32_t *)(epss_base[d] + EPSS_PERF_STATE) = (uint32_t)next;
		cpugov_changed[d] = now;
	}
}

/* One diagnostic line per ten seconds, including ten-second CPU averages.
 * Physical IDs make the sparse bootcpu=7 cpus=7 topology unambiguous. */
static void
cpugov_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t before[MAX_CPUS] = { 0 }, sample_at[MAX_CPUS] = { 0 };
	uint64_t used_sum[MAX_CPUS] = { 0 }, elapsed_sum[MAX_CPUS] = { 0 };
	bool valid[MAX_CPUS] = { false };
	uint64_t interval, log_interval, last_log = mach_absolute_time();
	uint64_t switch_interval, last_switch = last_log;
	uint64_t wfi_entries[MAX_CPUS] = { 0 }, wfi_ticks[MAX_CPUS] = { 0 };
	bool wfi_valid[MAX_CPUS] = { false };
	uint64_t samples = 0, top_samples[EPSS_DOMAINS] = { 0 }, boost_samples[EPSS_DOMAINS] = { 0 };
	uint64_t khz_sum[EPSS_DOMAINS] = { 0 };

	nanoseconds_to_absolutetime(20 * NSEC_PER_MSEC, &interval);
	nanoseconds_to_absolutetime(10 * NSEC_PER_SEC, &log_interval);
	nanoseconds_to_absolutetime(NSEC_PER_SEC, &switch_interval);
	thread_set_thread_name(current_thread(), "umi-cpugov");
	for (unsigned i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
		valid[i] = ml_cpu_idle_time(topology->cpus[i].cpu_id, &before[i]);
		sample_at[i] = mach_absolute_time();
	}
	for (;;) {
		uint32_t busy[EPSS_DOMAINS] = { 0 };
		bool seen[EPSS_DOMAINS] = { false };
		uint64_t deadline;

		/* Block explicitly: this observer must never spin to wait for a tick. */
		clock_interval_to_deadline(20, NSEC_PER_MSEC, &deadline);
		assert_wait_deadline(&cpugov_wait, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
		for (unsigned i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle, now;
			bool have = ml_cpu_idle_time(topology->cpus[i].cpu_id, &idle);
			now = mach_absolute_time();
			if (have && valid[i] && idle >= before[i] && now > sample_at[i]) {
				uint64_t elapsed = now - sample_at[i], delta = idle - before[i];
				uint64_t used = elapsed > delta ? elapsed - delta : 0;
				uint32_t d = domain_of(topology->cpus[i].phys_id);
				busy[d] = MAX(busy[d], (uint32_t)(100 * used / elapsed));
				seen[d] = true;
				used_sum[i] += used;
				elapsed_sum[i] += elapsed;
			}
			valid[i] = have;
			if (have) before[i] = idle;
			sample_at[i] = now;
		}
		char line[256], policy_line[256];
		uint64_t now = mach_absolute_time();
		if (now - last_switch >= switch_interval) {
			/* Boost scope: local (the caller's group) is the default;
			 * /private/var/db/umi-thermal-global-boost restores the vote on
			 * every group. The old umi-thermal-local-boost file, if it is
			 * still there, asks for what is now the default. */
			bool local = !cpugov_switch("/private/var/db/umi-thermal-global-boost");
			umi_idle_set_measurement(cpugov_switch("/private/var/db/umi-thermal-idle"));
			lck_mtx_lock(&umi_freq_lock);
			bool changed = cpugov_local_boost != local;
			cpugov_local_boost = local;
			/* A change of scope must not leave a hold taken under the old one. */
			if (changed) for (uint32_t d = 0; d < EPSS_DOMAINS; d++) cpugov_boost_until[d] = 0;
			lck_mtx_unlock(&umi_freq_lock);
			if (changed) tell("heat-policy: boost=%s\n",
			    local ? "local (the caller's group; the default)" : "global (every group: umi-thermal-global-boost)");
			last_switch = now;
			ddr_switch_update();
		}
		bool report = now - last_log >= log_interval;
		lck_mtx_lock(&umi_freq_lock);
		samples++;
		for (uint32_t d = 0; d < EPSS_DOMAINS; d++) {
			if (cpugov_enabled && seen[d] && epss_top[d] >= 0)
				cpugov_update(d, busy[d], now, interval);
			if (epss_top[d] >= 0) {
				top_samples[d] += epss_row[d] == epss_top[d];
				boost_samples[d] += now < cpugov_boost_until[d];
				khz_sum[d] += hakios_cpufreq_khz(d, epss_row[d]);
			}
		}
		if (report) {
			int at = snprintf(line, sizeof(line), "heat: rows %d/%d %d/%d %d/%d MHz %u/%u/%u busy%%(phys)=",
			    epss_row[0], umi_freq_limit(0, epss_cap[0]), epss_row[1], umi_freq_limit(1, epss_cap[1]),
			    epss_row[2], umi_freq_limit(2, epss_cap[2]), hakios_cpufreq_khz(0, epss_row[0]) / 1000,
			    hakios_cpufreq_khz(1, epss_row[1]) / 1000, hakios_cpufreq_khz(2, epss_row[2]) / 1000);
			for (unsigned i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
				if (elapsed_sum[i] && at < (int)sizeof(line) - 16)
					at += snprintf(line + at, sizeof(line) - at, " %u:%llu",
					    (topology->cpus[i].phys_id >> 8) & 0xff, 100 * used_sum[i] / elapsed_sum[i]);
				used_sum[i] = elapsed_sum[i] = 0;
			}
			snprintf(policy_line, sizeof(policy_line),
			    "heat-policy: local=%u calls=%llu samples=%llu boost%%=%llu/%llu/%llu top%%=%llu/%llu/%llu mean-MHz=%llu/%llu/%llu (sampled requests)\n",
			    cpugov_local_boost, cpugov_boost_calls, samples,
			    100 * boost_samples[0] / samples, 100 * boost_samples[1] / samples, 100 * boost_samples[2] / samples,
			    100 * top_samples[0] / samples, 100 * top_samples[1] / samples, 100 * top_samples[2] / samples,
			    khz_sum[0] / samples / 1000, khz_sum[1] / samples / 1000, khz_sum[2] / samples / 1000);
			cpugov_boost_calls = samples = 0;
			for (unsigned d = 0; d < EPSS_DOMAINS; d++) top_samples[d] = boost_samples[d] = khz_sum[d] = 0;
		}
		lck_mtx_unlock(&umi_freq_lock);
		if (report) {
			tell("%s\n", line);
			tell("%s", policy_line);
			uint64_t window_ns;
			absolutetime_to_nanoseconds(now - last_log, &window_ns);
			for (unsigned i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
				unsigned cpu = topology->cpus[i].cpu_id;
				uint64_t entries, ticks, ns;
				if (cpu >= MAX_CPUS || topology->cpus[i].phys_id == 0 ||
				    !umi_idle_snapshot(cpu, &entries, &ticks)) {
					wfi_valid[i] = false;
					continue;
				}
				absolutetime_to_nanoseconds(ticks - wfi_ticks[i], &ns);
				if (wfi_valid[i]) tell("heat-wfi: phys=%u entries=%llu wait_us=%llu window_us=%llu\n",
				    (topology->cpus[i].phys_id >> 8) & 0xff, entries - wfi_entries[i], ns / 1000, window_ns / 1000);
				wfi_entries[i] = entries; wfi_ticks[i] = ticks;
				wfi_valid[i] = true;
			}
			last_log = now;
		}
	}
}

static void
cpugov_start(void)
{
	thread_t thread;
	uint32_t on = 1;

	PE_parse_boot_argn("cpugov", &on, sizeof(on));
	os_atomic_store(&cpugov_enabled, on != 0, release);
	if (kernel_thread_start(cpugov_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
		tell("cpugov: %s; tick=20ms boost<=%ums (caller's group) decay=2 samples; heat=10s; idle=WFI\n",
		    on ? "enabled (default)" : "fixed rows", (unsigned)CPUGOV_BOOST_MAX_MS);
	} else {
		os_atomic_store(&cpugov_enabled, false, release);
		tell("cpugov: could not start governor thread\n");
	}
}

/*
 * Boot-arg membench=1: how fast this processor reads, writes and copies memory, in MB a second
 * (32 MB at a time, far more than any cache): what the memory's clock allows a processor, which
 * nobody here asks to be raised.
 */
static void
memory_bench(void)
{
	const vm_size_t size = 32u << 20;
	vm_offset_t a = 0, b = 0;
	uint64_t began, ns[3] = { 0, 0, 0 };
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		printf("membench: no memory\n");
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	for (int round = 0; round < 4; round++) {
		uint64_t t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		ns[2] += t;
	}
	tell("membench: on core %llu: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n",
	    (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff, 4ull * (size >> 20) * 1000000000ull / ns[0],
	    4ull * (size >> 20) * 1000000000ull / ns[1], 4ull * (size >> 20) * 1000000000ull / ns[2]);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

/*
 * Boot-arg membench=2: the same on every core of ours at once (a thread each, started together, 16 MB
 * at a time), 2 s after this starts, so that all the cores are up: the sum of what they get is what
 * the memory gives at the clock it has been left at.
 */
#define BENCH_THREADS   8
static struct {
	volatile uint32_t ready, go, done;
	uint64_t ns[BENCH_THREADS][3];
	uint32_t core[BENCH_THREADS];
} bench;

static void
bench_thread(void *arg, wait_result_t wr __unused)
{
	const vm_size_t size = 16u << 20;
	uintptr_t index = (uintptr_t)arg;
	vm_offset_t a = 0, b = 0;
	volatile uint64_t sum = 0;

	if (kmem_alloc(kernel_map, &a, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS ||
	    kmem_alloc(kernel_map, &b, size, KMA_KOBJECT | KMA_DATA, VM_KERN_MEMORY_DIAG) != KERN_SUCCESS) {
		os_atomic_inc(&bench.ready, relaxed);
		os_atomic_inc(&bench.done, relaxed);
		return;
	}
	memset((void *)a, 1, size);
	memset((void *)b, 2, size);
	os_atomic_inc(&bench.ready, relaxed);
	while (!os_atomic_load(&bench.go, acquire)) {
	}
	bench.core[index] = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	for (int round = 0; round < 4; round++) {
		uint64_t began, t;

		began = mach_absolute_time();
		memset((void *)a, round, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][0] += t;
		began = mach_absolute_time();
		for (const uint64_t *p = (const uint64_t *)b, *end = (const uint64_t *)(b + size); p < end; p += 8) {
			sum += p[0] + p[1] + p[2] + p[3] + p[4] + p[5] + p[6] + p[7];
		}
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][1] += t;
		began = mach_absolute_time();
		memcpy((void *)b, (const void *)a, size);
		absolutetime_to_nanoseconds(mach_absolute_time() - began, &t);
		bench.ns[index][2] += t;
	}
	os_atomic_inc(&bench.done, release);
	kmem_free(kernel_map, a, size);
	kmem_free(kernel_map, b, size);
}

static void
bench_all(void *arg __unused, wait_result_t wr __unused)
{
	const uint32_t threads = MIN(ml_get_topology_info()->num_cpus, BENCH_THREADS);
	const uint64_t mb = 4 * 16;                     /* per thread and test */
	uint64_t total[3] = { 0, 0, 0 };

	delay_for_interval(2000, NSEC_PER_MSEC);
	for (uintptr_t i = 0; i < threads; i++) {
		thread_t thread;

		if (kernel_thread_start(bench_thread, (void *)i, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		} else {
			os_atomic_inc(&bench.ready, relaxed);
			os_atomic_inc(&bench.done, relaxed);
		}
	}
	while (os_atomic_load(&bench.ready, relaxed) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	os_atomic_store(&bench.go, 1, release);
	while (os_atomic_load(&bench.done, acquire) < threads) {
		delay_for_interval(1, NSEC_PER_MSEC);
	}
	for (uint32_t i = 0; i < threads; i++) {
		tell("membench: all at once: core %u: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", bench.core[i],
		    bench.ns[i][0] ? mb * 1000000000ull / bench.ns[i][0] : 0, bench.ns[i][1] ? mb * 1000000000ull / bench.ns[i][1] : 0,
		    bench.ns[i][2] ? mb * 1000000000ull / bench.ns[i][2] : 0);
		for (int k = 0; k < 3; k++) {
			total[k] += bench.ns[i][k] ? mb * 1000000000ull / bench.ns[i][k] : 0;
		}
	}
	tell("membench: all %u at once, together: writing %llu MB/s, reading %llu MB/s, copying %llu MB/s\n", threads, total[0],
	    total[1], total[2]);
}

/*
 * Boot-arg cpuload=MS: every MS milliseconds, how busy each of our cores was meanwhile, in
 * percent, to the loader's log (tools/frames.py shows it per gesture):
 *   tl: u at US 7:NN 6:NN ... 1:NN      the core's number (as in its MPIDR), then its share
 * of the time it was not idle (interrupts included).
 */
static uint32_t load_every_ms;

static void
load_thread(void *arg __unused, wait_result_t wr __unused)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint64_t idle_before[MAX_CPUS] = { 0 }, then = mach_absolute_time();

	for (;;) {
		char line[192];
		int at;
		uint64_t now, us;

		delay_for_interval(load_every_ms, NSEC_PER_MSEC);
		now = mach_absolute_time();
		absolutetime_to_nanoseconds(now, &us);
		at = snprintf(line, sizeof(line), "tl: u at %llu", us / 1000);
		for (unsigned int i = 0; i < topology->num_cpus && i < MAX_CPUS; i++) {
			uint64_t idle = 0, busy;

			if (!ml_cpu_idle_time((int)i, &idle)) {
				continue;
			}
			busy = (now - then) > (idle - idle_before[i]) ? (now - then) - (idle - idle_before[i]) : 0;
			idle_before[i] = idle;
			if (at < (int)sizeof(line) - 12) {
				at += snprintf(line + at, sizeof(line) - at, " %u:%llu", (topology->cpus[i].phys_id >> 8) & 0xff,
				    100 * busy / (now - then));
			}
		}
		then = now;
		hakios_tl("%s\n", line);
	}
}

static void
load_start(void)
{
	thread_t thread;

	if (!PE_parse_boot_argn("cpuload", &load_every_ms, sizeof(load_every_ms)) || load_every_ms == 0) {
		return;
	}
	if (kernel_thread_start(load_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	}
}

/*
 * Boot-arg ddr=1: the memory's bus clock managers ("BCM") in the command database the boot loader
 * left, with what is needed to vote for bandwidth on them -- only read. (membench=2 says the cores
 * together get no more from memory than one alone: 12 GB/s; nobody here votes for more.)
 */
#define CMD_DB_PHYS             hakios_board_addr("cmd_db", 0x80860000UL)
#define CMD_DB_SIZE             0x20000UL
#define CMD_DB_BCM              5               /* the kind of resource in a slave's id: bus clock managers */

struct cmd_db_slave {
	uint16_t id, header_offset, data_offset, count, version, reserved[3];
};
struct cmd_db_header {
	uint32_t version;
	uint8_t magic[4];
	struct cmd_db_slave slave[8];
	uint32_t checksum, reserved;
	uint8_t data[];
};
struct cmd_db_entry {
	char id[8];
	uint32_t priority[2];
	uint32_t address;
	uint16_t length, offset;
};
struct cmd_db_bcm {                             /* a BCM's auxiliary data (Linux: struct bcm_db) */
	uint32_t unit;
	uint16_t width;
	uint8_t vcd, reserved;
};

/* (the same condition as the cores' clocks: the addresses are this phone's) */
static bool
cpufreq_wanted(void)
{
	uint32_t on = 0;

	return PE_parse_boot_argn("cpufreq", &on, sizeof(on)) && on != 0;
}

static void
ddr_look(void)
{
	/* (memory, not registers: mapped so that reads of any size and alignment are allowed) */
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		tell("ddr: no command database\n");
		return;
	}
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			struct cmd_db_bcm aux = { 0 };
			char name[9] = { 0 };

			memcpy(name, entry[i].id, 8);
			/* (the data starts after the header, so the header counts: this keeps the read inside the mapped database) */
			if (entry[i].length >= sizeof(aux) &&
			    sizeof(*db) + slave->data_offset + entry[i].offset + sizeof(aux) <= CMD_DB_SIZE) {
				memcpy(&aux, db->data + slave->data_offset + entry[i].offset, sizeof(aux));
			}
			tell("ddr: BCM %-8s at %#x: unit %u, width %u, vcd %u (aux %u bytes)\n", name, entry[i].address, aux.unit,
			    aux.width, aux.vcd, entry[i].length);
		}
	}
}

/*
 * ... and what the boot loader's firmware left in the application processor's RPMh command slots
 * ("DRV 2"; Linux: rpmh-rsc.c): 2 active, 3 sleep, 3 wake slot sets and a control one, of 16 commands
 * each (sc7280's qcom,tcs-config): every command with an address, and which of them go to the BCMs
 * above -- the bandwidth it asked for, and that is in force whenever the processors are awake.
 */
#define RSC_PHYS                hakios_board_addr("rsc", 0x18220000UL)
#define RSC_SIZE                0x10000UL
#define RSC_TCS(n, reg)         (0xd00 + 0x2a0 * (n) + (reg))
#define RSC_TCS_COUNT           9
#define RSC_CMDS                16

static void
ddr_slots(void)
{
	vm_offset_t rsc = ml_io_map(RSC_PHYS, RSC_SIZE);

	for (unsigned t = 0; t < RSC_TCS_COUNT; t++) {
		uint32_t enabled = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x1c));

		for (unsigned c = 0; c < RSC_CMDS; c++) {
			uint32_t msgid = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x30 + 20 * c));
			uint32_t address = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x34 + 20 * c));
			uint32_t data = *(volatile uint32_t *)(rsc + RSC_TCS(t, 0x38 + 20 * c));

			if (address == 0 && data == 0) {
				continue;
			}
			tell("ddr: slot set %u (%s) command %u%s: message %#x, address %#x, data %#x%s\n", t,
			    t < 2 ? "active" : t < 5 ? "sleep" : t < 8 ? "wake" : "control", c, enabled & (1u << c) ? " (on)" : "",
			    msgid, address, data, (address >> 16 & 0xf) == CMD_DB_BCM ? " <- a BCM" : "");
		}
	}
}

/*
 * The memory's bandwidth (on by default; boot-arg ddr=0: not): a vote through RPMh -- the most there
 * is -- on the bus clock managers of the memory controller (MC0: the memory's clock) and of the
 * shared hub in front of it with the system cache (SH0), as Linux's interconnect driver casts for
 * a busy GPU (icc-bcm-voter.c: a BCM command is commit, valid, the average and the peak, 14 bits each,
 * in the BCM's units; the BCM takes the highest of its levels the vote reaches), and the memory kept
 * on for this processor (ACV, bit 3, which the boot loader's firmware had set already, the same).
 * All cores together got 12.4 GB/s writing without it (membench=2: what one core gets alone), 14.8
 * with it; frames over 16.7 ms 28% instead of 34-36%. ddr_vote=N: N instead of the default
 * (4096, the middle of what the firmware voted in our name); /private/var/db/umi-ddr-full
 * restores the most (16383), and is looked at again by the governor's one-second poll, since
 * the root file system is not mounted when this first votes.
 * When the kernel goes back to the loader (hakios_ddr_quiesce(), on every way back, a panic's too)
 * the vote goes down to ddr_rest=N (default 1024). What the firmware had voted cannot be read back,
 * but what the cores get from memory can be measured (membench=2, writing): with any vote from 4
 * to 4096 12.0 to 12.4 GB/s, as before the first vote (12.4); with the most, 15.4; with none at all
 * ("no request", valid clear: what Linux sends for nothing) 0.2 -- the memory at its floor, and the
 * loader's USB transfers at 8 MB/s instead of 30. So the firmware had a vote of its own in our name,
 * of that middle range, and one from the middle of it is left.
 */
#define BCM_COMMIT              (1u << 30)
#define BCM_VALID               (1u << 29)
#define BCM_VOTE(x, y)          (BCM_COMMIT | BCM_VALID | ((uint32_t)(x) & 0x3fff) << 14 | ((uint32_t)(y) & 0x3fff))
#define TCS_REG(reg)            (ddr_rsc + 0xd00 + (reg))       /* command slot set 0, an "active" one */
#define BCM_ACV_APPS            8
#define DDR_REST                1024            /* the vote left for the loader (ddr_rest=N); see hakios_ddr_quiesce() */
#define DDR_VOTE_DEFAULT        4096            /* 12.0-12.4 GB/s, as the firmware's own vote: the default */
#define DDR_VOTE_FULL           0x3fff          /* 15.4 GB/s, the most: /private/var/db/umi-ddr-full */

static vm_offset_t ddr_rsc;
static uint32_t ddr_mc0, ddr_sh0, ddr_acv;
static bool ddr_voted;
static bool ddr_vote_managed;           /* no ddr_vote= boot argument: the switch file decides */
static bool ddr_vote_full;              /* the vote in force is DDR_VOTE_FULL */

static uint32_t
cmd_db_address(const char *name)
{
	const struct cmd_db_header *db = (const struct cmd_db_header *)ml_io_map_wcomb(CMD_DB_PHYS, CMD_DB_SIZE);
	char id[8] = { 0 };

	if (db->magic[0] != 0xdb || db->magic[1] != 0x30 || db->magic[2] != 0x03 || db->magic[3] != 0x0c) {
		return 0;
	}
	strlcpy(id, name, sizeof(id));
	for (unsigned s = 0; s < 8; s++) {
		const struct cmd_db_slave *slave = &db->slave[s];
		const struct cmd_db_entry *entry = (const struct cmd_db_entry *)(db->data + slave->header_offset);

		if (slave->id != CMD_DB_BCM || slave->header_offset >= CMD_DB_SIZE - 0x1000 || slave->count > 512) {
			continue;
		}
		for (unsigned i = 0; i < slave->count; i++) {
			if (memcmp(entry[i].id, id, sizeof(id)) == 0) {
				return entry[i].address;
			}
		}
	}
	return 0;
}

/* Up to three writes through command slot set 0, and the wait until RPMh has done them (as drivers/input/hakios.goodix.c) */
static bool
ddr_rpmh_write(const uint32_t *address, const uint32_t *data, unsigned count)
{
	uint32_t irq_enable = *(volatile uint32_t *)TCS_REG(0x00);
	bool done = false;

	for (int i = 0; *(volatile uint32_t *)TCS_REG(0x18) == 0; i++) {
		if (i > 2000) {
			tell("ddr: RPMh: the command slots stay busy\n");
			return false;
		}
		delay_for_interval(100, NSEC_PER_USEC);
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	for (unsigned i = 0; i < count; i++) {
		*(volatile uint32_t *)TCS_REG(0x30 + 20 * i) = 0x10008 | 0x100;        /* a 4-byte write, with a response */
		*(volatile uint32_t *)TCS_REG(0x34 + 20 * i) = address[i];
		*(volatile uint32_t *)TCS_REG(0x38 + 20 * i) = data[i];
	}
	*(volatile uint32_t *)TCS_REG(0x10) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x1c) = (1u << count) - 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable | 1;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16;
	*(volatile uint32_t *)TCS_REG(0x14) = 1u << 16 | 1u << 24;
	for (int i = 0; i < 2000 && !done; i++) {
		done = (*(volatile uint32_t *)TCS_REG(0x04) & 1) != 0;
		if (!done) {
			delay_for_interval(100, NSEC_PER_USEC);
		}
	}
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 24);
	*(volatile uint32_t *)TCS_REG(0x14) &= ~(1u << 16);
	*(volatile uint32_t *)TCS_REG(0x1c) = 0;
	*(volatile uint32_t *)TCS_REG(0x10) = 0;
	*(volatile uint32_t *)TCS_REG(0x08) = 1;
	*(volatile uint32_t *)TCS_REG(0x00) = irq_enable;
	if (!done) {
		tell("ddr: RPMh: no answer to the vote\n");
	}
	return done;
}

static bool
ddr_vote(uint32_t vote)
{
	uint32_t address[3] = { ddr_mc0, ddr_sh0, ddr_acv };
	uint32_t data[3] = { BCM_VOTE(vote, vote), BCM_VOTE(vote, vote), BCM_VOTE(0, BCM_ACV_APPS) };

	return ddr_rpmh_write(address, data, 3);
}

static void
ddr_raise(void)
{
	uint32_t vote = DDR_VOTE_DEFAULT;
	const char *why;

	/* ddr_vote=N keeps its meaning and wins over the switch; without it the vote is the
	 * middle of the firmware's own range unless /private/var/db/umi-ddr-full asks for more. */
	ddr_vote_managed = !PE_parse_boot_argn("ddr_vote", &vote, sizeof(vote));
	if (ddr_vote_managed && rootvnode != NULLVP && cpugov_switch("/private/var/db/umi-ddr-full")) {
		vote = DDR_VOTE_FULL;
	}
	ddr_mc0 = cmd_db_address("MC0");
	ddr_sh0 = cmd_db_address("SH0");
	ddr_acv = cmd_db_address("ACV");
	if (ddr_mc0 == 0 || ddr_sh0 == 0 || ddr_acv == 0 || vote == 0 || vote > 0x3fff) {
		tell("ddr: no vote (MC0 %#x, SH0 %#x, ACV %#x, vote %u)\n", ddr_mc0, ddr_sh0, ddr_acv, vote);
		return;
	}
	ddr_rsc = ml_io_map(RSC_PHYS, RSC_SIZE);
	ddr_voted = ddr_vote(vote);
	ddr_vote_full = ddr_voted && vote == DDR_VOTE_FULL;
	why = !ddr_vote_managed ? " (ddr_vote=)" : ddr_vote_full ? " (umi-ddr-full)" : "";
	tell("ddr: memory bandwidth vote %u on MC0 (%#x) and SH0 (%#x)%s%s\n", vote, ddr_mc0, ddr_sh0,
	    ddr_voted ? "" : ": FAILED", ddr_voted ? why : "");
}

/*
 * The firmware's own vote was taken in our name, in the middle of the range, before the kernel
 * began (see above): this leaves one of that middle range, which costs nothing measurable and
 * keeps the memory and the shared hub out of their top state all session. /private/var/db/umi-ddr-full
 * restores the most. The root file system is not mounted when the first vote is cast, so the file
 * is read from the governor's one-second poll (with the other switch files) until it can be, and
 * then whenever it appears or disappears. No ddr_vote= argument: that one wins.
 */
static void
ddr_switch_update(void)
{
	bool full;
	uint32_t vote;

	if (!ddr_vote_managed || !ddr_voted || rootvnode == NULLVP) {
		return;
	}
	full = cpugov_switch("/private/var/db/umi-ddr-full");
	if (full == ddr_vote_full) {
		return;
	}
	vote = full ? DDR_VOTE_FULL : DDR_VOTE_DEFAULT;
	ddr_voted = ddr_vote(vote);
	ddr_vote_full = ddr_voted && full;
	tell("ddr: memory bandwidth vote %u on MC0 (%#x) and SH0 (%#x)%s%s\n", vote, ddr_mc0, ddr_sh0,
	    ddr_voted ? "" : ": FAILED", ddr_vote_full ? " (umi-ddr-full)" : " (default)");
}

/* On the way back to the loader (osfmk/arm64/loader_return.c: loader_return(), whatever the reason; the other processors are stopped) */
void hakios_ddr_quiesce(void);

void
hakios_ddr_quiesce(void)
{
	uint32_t address[2] = { ddr_mc0, ddr_sh0 };
	uint32_t rest = DDR_REST, data[2];
	bool done;

	/* fb_dpu: the panel back on the GOP frame the loader and the panic console draw into (hakios.display.c) */
	extern void hakios_display_dpu_restore(void);
	hakios_display_dpu_restore();
	if (!ddr_voted) {
		return;
	}
	ddr_voted = false;
	PE_parse_boot_argn("ddr_rest", &rest, sizeof(rest));
	rest = MIN(rest, 0x3fff);
	data[0] = data[1] = rest != 0 ? BCM_VOTE(rest, rest) : BCM_COMMIT;     /* 0: no request */
	done = ddr_rpmh_write(address, data, 2);
	tell("ddr: memory bandwidth vote %u for the loader: %s\n", rest, done ? "done" : "FAILED");
}

void
hakios_cpufreq_init(void)
{
	const ml_topology_info_t *topology = ml_get_topology_info();
	uint32_t enabled = 0, membench = 0, cores[EPSS_DOMAINS] = { 0 }, before, here;

	load_start();
	enabled = 2;
	PE_parse_boot_argn("ddr", &enabled, sizeof(enabled));
	if (enabled == 1) {
		ddr_look();
		ddr_slots();
	} else if (enabled == 2 && cpufreq_wanted()) {
		ddr_raise();
	}
	enabled = 0;
	if (!PE_parse_boot_argn("cpufreq", &enabled, sizeof(enabled)) || !enabled) {
		return;
	}
	for (unsigned int i = 0; i < topology->num_cpus; i++) {
		cores[domain_of(topology->cpus[i].phys_id)]++;
		tell("cpufreq: cpu%u -> MPIDR %#x, group %u\n", topology->cpus[i].cpu_id,
		     topology->cpus[i].phys_id, domain_of(topology->cpus[i].phys_id));
	}
	/* (whichever core this thread happens to be on, and stays on or not: it only goes to the log) */
	here = (uint32_t)(__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	before = loop_speed();
	for (uint32_t domain = 0; domain < EPSS_DOMAINS; domain++) {
		if (cores[domain] != 0) {
			raise_domain(domain, cores[domain]);
		}
	}
	printf("cpufreq: on core %u a loop ran at %u, now at %u\n", here, before, loop_speed());
	/* membench has its own variable: when the boot-arg is absent, the cpufreq value (2 means on) must not start the benchmark */
	PE_parse_boot_argn("membench", &membench, sizeof(membench));
	if (membench == 1) {
		memory_bench();
	} else if (membench == 2) {
		thread_t thread;

		if (kernel_thread_start(bench_all, NULL, &thread) == KERN_SUCCESS) {
			thread_deallocate(thread);
		}
	}
	cpugov_start();
}
