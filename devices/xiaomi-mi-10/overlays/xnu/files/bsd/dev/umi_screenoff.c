/* Included by the display overlay to avoid changing shared build manifests. */
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <os/atomic_private.h>
#include <sys/umi_screenoff.h>

static unsigned pm_enabled, pm_off, pm_gpu, pm_wifi, pm_idle;
static uint32_t pm_cpu_mhz = 900;
static uint64_t pm_frames, pm_touch;
static bool pm_initialized;
static int pm_wait;

bool umi_pm_screen_off(void)
{
	return os_atomic_load(&pm_enabled, acquire) && os_atomic_load(&pm_off, acquire);
}
uint32_t umi_pm_cpu_cap_khz(uint32_t domain)
{
	return domain != 0 && domain < 3 && umi_pm_screen_off() ? pm_cpu_mhz * 1000 : 0;
}
int umi_pm_wifi_policy(void)
{
	return pm_wifi && os_atomic_load(&pm_enabled, acquire) ? (int)umi_pm_screen_off() : -1;
}
void umi_pm_frame_skipped(void) { os_atomic_inc(&pm_frames, relaxed); }
void umi_pm_touch_skipped(void) { os_atomic_inc(&pm_touch, relaxed); }

void
umi_pm_display(bool off, const char *source)
{
	if (!os_atomic_load(&pm_enabled, acquire)) return;
	unsigned old = os_atomic_xchg(&pm_off, (unsigned)off, acq_rel);
	if (old == (unsigned)off) return;
	hakios_tl("umi-pm: screen=%s source=%s cap_khz=%u loader-domain0=unchanged\n",
	    off ? "off" : "on", source, off ? pm_cpu_mhz * 1000 : 0);
	/* Only policy publication here: driver locks and firmware waits belong to
	 * their owners, never the backlight/sysctl or framebuffer callback. */
	thread_wakeup(&pm_wait);
}

static int
sysctl_screenoff(__unused struct sysctl_oid *oidp, __unused void *arg1,
    __unused int arg2, struct sysctl_req *req)
{
	int value = umi_pm_screen_off(), error;
	error = sysctl_io_number(req, value, sizeof(value), &value, NULL);
	if (error || !req->newptr) return error;
	if (!os_atomic_load(&pm_enabled, acquire)) return ENOTSUP;
	if (value != 0 && value != 1) return EINVAL;
	umi_pm_display(value != 0, "sysctl-test");
	return 0;
}
SYSCTL_PROC(_kern_hakios, OID_AUTO, screenoff, CTLTYPE_INT | CTLFLAG_RW | CTLFLAG_LOCKED,
    NULL, 0, sysctl_screenoff, "I", "UMI screen-off policy; requires screenoff_pm=1");

/* Read-only SMCCC queries, using the same conduit chosen by psci_smp_init(). */
extern uint32_t psci_smp_hvc;
static int32_t
umi_psci_query(uint64_t function, uint64_t argument)
{
	register uint64_t x0 __asm__("x0") = function;
	register uint64_t x1 __asm__("x1") = argument;
	register uint64_t x2 __asm__("x2") = 0;
	register uint64_t x3 __asm__("x3") = 0;
#define UMI_SMCCC_CLOBBERS "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory", "cc"
	if (psci_smp_hvc) {
		__asm__ volatile("hvc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : UMI_SMCCC_CLOBBERS);
	} else {
		__asm__ volatile("smc #0" : "+r"(x0), "+r"(x1), "+r"(x2), "+r"(x3) : : UMI_SMCCC_CLOBBERS);
	}
#undef UMI_SMCCC_CLOBBERS
	return (int32_t)x0;
}

static void
umi_pm_worker(__unused void *arg, __unused wait_result_t result)
{
	uint64_t prev_entries[MAX_CPUS] = {0}, prev_ticks[MAX_CPUS] = {0};
	unsigned round = 0;
	if (pm_idle) {
		int32_t version = umi_psci_query(0x84000000, 0);
		/* PSCI_FEATURES exists starting at PSCI 1.0. */
		int32_t cpu = -1, system = -1;
		if (version >= 0x10000) {
			cpu = umi_psci_query(0x8400000a, 0xc4000001);
			system = umi_psci_query(0x8400000a, 0xc400000e);
		}
		hakios_tl("umi-pm: PSCI conduit=%s version=%#x CPU_SUSPEND=%d SYSTEM_SUSPEND=%d; WFI-only\n",
		    psci_smp_hvc ? "hvc" : "smc", version, cpu, system);
	}
	for (;;) {
		uint64_t deadline;
		if (os_atomic_load(&pm_enabled, acquire)) {
			umi_cpufreq_refresh();
			if (pm_gpu) umi_gpu_pm_update(umi_pm_screen_off());
		}
		if (++round % 10 == 0) {
			hakios_tl("umi-pm: screen=%s skipped_frames=%llu skipped_touch_polls=%llu\n",
			    umi_pm_screen_off() ? "off" : "on", os_atomic_load(&pm_frames, relaxed), os_atomic_load(&pm_touch, relaxed));
			const ml_topology_info_t *topology = ml_get_topology_info();
			for (unsigned i = 0; pm_idle && i < topology->num_cpus; i++) {
				uint64_t entries, ticks, ns;
				unsigned cpu = topology->cpus[i].cpu_id;
				if (!umi_idle_snapshot(cpu, &entries, &ticks)) continue;
				absolutetime_to_nanoseconds(ticks - prev_ticks[cpu], &ns);
				hakios_tl("umi-idle: cpu=%u mpidr=%#x WFI_delta=%llu wait_us=%llu%s\n",
				    cpu, topology->cpus[i].phys_id, entries - prev_entries[cpu], ns / 1000,
				    topology->cpus[i].phys_id == 0 ? " loader-core-excluded" : "");
				prev_entries[cpu] = entries; prev_ticks[cpu] = ticks;
			}
		}
		/* A transition during work is picked up within this bounded second. */
		clock_interval_to_deadline(1, NSEC_PER_SEC, &deadline);
		assert_wait_deadline(&pm_wait, THREAD_UNINT, deadline);
		thread_block(THREAD_CONTINUE_NULL);
	}
}

void
umi_pm_init(void)
{
	DTEntry chosen; const void *board; unsigned size, enabled = 0;
	if (pm_initialized) return;
	pm_initialized = true; /* called once by framebuffer startup */
	if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
	    SecureDTGetProperty(chosen, "hakios-board", &board, &size) != kSuccess ||
	    size != 4 || memcmp(board, "umi", 4)) return;
	PE_parse_boot_argn("screenoff_pm", &enabled, sizeof(enabled));
	PE_parse_boot_argn("idle", &pm_idle, sizeof(pm_idle));
	pm_idle = pm_idle == 1;
	if (enabled != 1 && !pm_idle) return;
	PE_parse_boot_argn("screenoff_cpu_mhz", &pm_cpu_mhz, sizeof(pm_cpu_mhz));
	if (pm_cpu_mhz < 300 || pm_cpu_mhz > 2000) pm_cpu_mhz = 900;
	PE_parse_boot_argn("screenoff_gpu", &pm_gpu, sizeof(pm_gpu));
	PE_parse_boot_argn("screenoff_wifi", &pm_wifi, sizeof(pm_wifi));
	pm_gpu = pm_gpu == 1; pm_wifi = pm_wifi == 1;
	umi_idle_configure();
	thread_t thread;
	if (kernel_thread_start(umi_pm_worker, NULL, &thread) != KERN_SUCCESS) {
		hakios_tl("umi-pm: worker failed; screen-off policy disabled\n");
		return;
	}
	thread_deallocate(thread);
	os_atomic_store(&pm_enabled, enabled == 1, release);
	hakios_tl("umi-pm: enabled=%u idle=%u cap_mhz=%u gpu=%u wifi=%u; no power collapse\n",
	    enabled == 1, pm_idle, pm_cpu_mhz, pm_gpu, pm_wifi);
}
