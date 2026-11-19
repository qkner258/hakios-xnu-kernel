/* UMI stage-1 policy: no clock/rail power-off and no system suspend. */
#ifndef UMI_SCREENOFF_H
#define UMI_SCREENOFF_H
#include <stdint.h>
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif
void umi_pm_init(void);
void umi_pm_display(bool off, const char *source);
bool umi_pm_screen_off(void);
/* 0 means no additional cap. Domain 0 is shared with the resident loader. */
uint32_t umi_pm_cpu_cap_khz(uint32_t domain);
int umi_pm_wifi_policy(void); /* -1 disabled, 0 awake, 1 station PS requested */
void umi_pm_frame_skipped(void);
void umi_pm_touch_skipped(void);
void umi_cpufreq_refresh(void);
void umi_gpu_pm_update(bool off);
void umi_idle_configure(void);
bool umi_idle_snapshot(unsigned cpu, uint64_t *entries, uint64_t *ticks);
#ifdef __cplusplus
}
#endif
#endif
