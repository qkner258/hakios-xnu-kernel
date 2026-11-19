Sleep/DVFS integration contract (active concurrent work)

Sleep owns `umi_pm_cpu_cap_khz(domain)` in `bsd/dev/umi_screenoff.c`:
0 = no additional ceiling; otherwise an upper limit in kHz. No floor/boost.
Domain 0 always returns 0 because it shares the resident loader's core-0 clock.
The governor must clamp its final requested row by this ceiling AND its independent
thermal ceiling. Screen on must never reset the thermal cap. The current CPU
setter owns a separate `epss_sleep_cap[]` channel; our accessor leaves it intact.
The adapter clamps refresh, thermal/governor writes and that setter against all
applicable ceilings, and also supports the older shared-cap implementation.

`tools/stage-sleep.py` installs hooks in generated cpufreq.c only; the source
overlay is left to the CPU agent. The small `umi_cpufreq_screenoff.h` adapter is included after LUT discovery and
before final writers. Every EPSS performance-state write after initialization
passes through `umi_freq_limit(domain,row)`. `umi_cpufreq_refresh()` only revisits
the thermal request under the shared writer mutex in fixed-frequency mode; with cpugov enabled its tick owns
writes. Replace that refresh adapter with your governor's serialized recompute
if the governor representation changes. No second MMIO mapping or LUT is added.
Default screenoff_pm=0 => cap accessor is 0 and existing behavior remains.

The sleep agent also instruments the existing WFI instruction through generated-source anchors and a new osfmk/arm64/umi_idle.h; no scheduler topology/timer implementation
is changed. Its measurement excludes physical core 0 and never uses CPU_OFF,
CPU_SUSPEND or SYSTEM_SUSPEND. This file is for the concurrent CPU integration.
