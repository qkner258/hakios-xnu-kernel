/* Included once by cpu.c, directly around the existing generic WFI instruction.
 * Kona rail-pc loses context and its local timer: never issue CPU_SUSPEND here.
 * Default off means exactly the original WFI, not a new busy-idle default.
 */
#ifndef UMI_IDLE_IMPL_H
#define UMI_IDLE_IMPL_H
#include <os/atomic_private.h>
static unsigned umi_idle_enabled;
static unsigned umi_idle_measurement;
static struct {
	uint64_t entries, ticks;
} umi_idle_stats[MAX_CPUS] __attribute__((aligned(64)));

void umi_idle_configure(void);
bool umi_idle_snapshot(unsigned cpu, uint64_t *entries, uint64_t *ticks);
void umi_idle_set_measurement(bool enabled);

void
umi_idle_set_measurement(bool enabled)
{
	os_atomic_store(&umi_idle_measurement, enabled, release);
}

void
umi_idle_configure(void)
{
	unsigned enabled = 0;
	PE_parse_boot_argn("idle", &enabled, sizeof(enabled));
	os_atomic_store(&umi_idle_enabled, enabled == 1, release);
}

bool
umi_idle_snapshot(unsigned cpu, uint64_t *entries, uint64_t *ticks)
{
	if ((!os_atomic_load(&umi_idle_enabled, acquire) &&
	    !os_atomic_load(&umi_idle_measurement, acquire)) || cpu >= MAX_CPUS) return false;
	*entries = os_atomic_load(&umi_idle_stats[cpu].entries, relaxed);
	*ticks = os_atomic_load(&umi_idle_stats[cpu].ticks, relaxed);
	return true;
}

static inline void
umi_idle_wait(void)
{
	/* Physical core 0 stays on the pre-existing path, even with idle=1. */
	if ((!os_atomic_load(&umi_idle_enabled, acquire) &&
	    !os_atomic_load(&umi_idle_measurement, acquire)) ||
	    ((__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff) == 0) {
		__builtin_arm_wfi();
		return;
	}
	unsigned cpu = (unsigned)cpu_number();
	uint64_t before = mach_absolute_time();
	__builtin_arm_wfi(); /* DAIF, GIC, caches and timer exactly as cpu_idle set them. */
	uint64_t elapsed = mach_absolute_time() - before;
	os_atomic_add(&umi_idle_stats[cpu].ticks, elapsed, relaxed);
	os_atomic_inc(&umi_idle_stats[cpu].entries, relaxed);
}
#endif
