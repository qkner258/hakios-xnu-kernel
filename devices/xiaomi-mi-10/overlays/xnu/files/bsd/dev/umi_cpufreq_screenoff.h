/* Included in generated cpufreq.c by stage-sleep.py. All runtime writers use
 * one lock. The optional governor still owns load selection and boost policy.
 */
#include <sys/umi_screenoff.h>
#include <kern/locks.h>
static LCK_GRP_DECLARE(umi_freq_group, "umi-screen-cap");
static LCK_MTX_DECLARE(umi_freq_lock, &umi_freq_group);
#if UMI_SLEEP_HAS_CPUGOV
#define umi_freq_request epss_cap
#define umi_freq_current epss_row
#else
static int umi_freq_request[EPSS_DOMAINS] = {-1, -1, -1};
static int umi_freq_current[EPSS_DOMAINS] = {-1, -1, -1};
#endif
static int
umi_freq_limit(uint32_t domain, int row)
{
	uint32_t cap = umi_pm_cpu_cap_khz(domain);
#if UMI_SLEEP_HAS_EXTERNAL_CAP
	if (epss_sleep_cap[domain] >= 0 && row > epss_sleep_cap[domain])
		row = epss_sleep_cap[domain];
#endif
	while (cap && row > 0 && epss_khz[domain][row] > cap) row--;
	return row;
}

void
umi_cpufreq_refresh(void)
{
#if UMI_SLEEP_HAS_CPUGOV
	/* Its 20 ms tick recomputes the final clamp, including touch boosts. */
	if (cpugov_enabled) return;
#endif
	lck_mtx_lock(&umi_freq_lock);
	for (uint32_t d = 1; d < EPSS_DOMAINS; d++) {
		if (epss_top[d] < 0) continue;
		int requested = umi_freq_request[d] >= 0 ? umi_freq_request[d] : epss_top[d];
		int row = umi_freq_limit(d, requested);
		if (umi_freq_current[d] == row) continue;
		/* Preserve the thermal request. Releasing screen-off cannot lift it. */
		umi_freq_current[d] = row;
		*(volatile uint32_t *)(epss_base[d] + EPSS_PERF_STATE) = (uint32_t)row;
		tell("umi-pm: cpu domain=%u row=%d khz=%u cap_khz=%u\n",
		    d, row, epss_khz[d][row], umi_pm_cpu_cap_khz(d));
	}
	lck_mtx_unlock(&umi_freq_lock);
}
