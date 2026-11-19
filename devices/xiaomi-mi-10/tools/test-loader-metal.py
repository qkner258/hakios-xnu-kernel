#!/usr/bin/env python3
"""Exercise the actual UMI loader argument gate, without UEFI or MMIO."""
from pathlib import Path
import subprocess
import tempfile
root = Path(__file__).resolve().parents[3]
s = (root / 'devices/xiaomi-mi-10/loader/loader.c').read_text()
start = s.index('static int\numi_bootargs_ok(')
end = s.index('\nstatic int\nchoose_window(', start)
shim = r'''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define print(...) ((void)0)
static int platform_umi;
static int *PLAT = &platform_umi;
struct payload_info { uint64_t forced_base; const char *cmdline; };
static uint64_t str_len(const char *s) { return strlen(s); }
static int starts_with(const char *s, const char *p) { return strncmp(s, p, strlen(p)) == 0; }
'''
test = r'''
int main(void) {
 const char *good[] = { "bl=1", "al=1", "fb_scan=1 bl=1 al=1", "gpu=1 metal=1 metal_gmem_draws=0 metal_gmem_off=16 metal_sync=1 metal_shadow_refresh=1 metal_nocopy_refresh=1 metal_nospec", "gpu=1", "gpu=1 gpu_fc=1", "gpu=1 metal=1", "gpu=1 metal=2", "gpu=1 metal=3", "gpu=1 metal=1 metal_big=1", "gpu=1 metal=1 metal_all=1 ca_metallib=/redwood/share/ca-share/QuartzCore-default.metallib metal_defer=1 gpu_recover=1 anim_max=1 wallpaper_freeze=1 fbscale=2 fb_hz=120 fb_measure=1", "gpu=1 metal=1 metal_m2v=1 battery=1", "gpu=1 metal=1 metal_fps=1 metal_pass_wfi=1",
   "wifi=1 wifi_scan=1", "wifi=1 wifi_scan=1 wifi_mac=34:1c:f0:cf:a2:92",
   "gpu=1 glfix=0", "gpu=1 glfix=1", "cpufreq=1 cpugov=1", "cpufreq=1 cpugov=1 cpugov_log=1", "amp=1",
   "fb_scan=1 fb_diff=1 fb_dirty=1" };
 const char *bad[] = { "bl=0", "bl=11", "bl=1x", "al=2", "al=1x", "metal=3", "gpu=1 metal=0", "gpu=1 metal=4", "gpu=1 metal=33", "gpu=1 metal=x", "gpu=1 metal=3 metal=1",
   "wifi=1 wifi_scan=0", "wifi=1 wifi_scan=2", "wifi=1 wifi_scan=11", "wifi=1 wifi_scan=1x",
   "gpu=1 glfix=", "gpu=1 glfix=2", "gpu=1 glfix=10", "gpu=1 glfix=1x",
   "cpugov=1", "cpugov_log=1", "cpufreq=1 cpugov=1 cpugov_log=1 cpugov_log=1", "cpufreq=1 cpugov=1 cpugov_log=2",
   "cpufreq=1 cpugov=1 cpugov_log=1 cpugov=1", "cpufreq=1 cpugov=1 cpugov=1", "fb_dirty=1", "fb_scan=1 fb_dirty=1" }; struct payload_info p = {0};
 for (unsigned i = 0; i < sizeof(good)/sizeof(*good); i++) { p.cmdline=good[i]; assert(umi_bootargs_ok(&p)); }
 for (unsigned i = 0; i < sizeof(bad)/sizeof(*bad); i++) { p.cmdline=bad[i]; assert(!umi_bootargs_ok(&p)); }
 p.cmdline="gpu=1 metal=3"; p.forced_base=1; assert(!umi_bootargs_ok(&p));
 return 0;
}
'''
with tempfile.TemporaryDirectory() as d:
    c = Path(d)/'gate.c'; exe = Path(d)/'gate'
    c.write_text(shim+s[start:end]+test)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra',str(c),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
print('PASS: gpu_fc=1/metal_big=1, Metal levels 1..3, CA Metal arguments (metal_all/ca_metallib/defer/recovery/animation/wallpaper/fbscale/fb_hz), metal_m2v=1, wifi_scan=1, glfix=0/1, cpugov/cpugov_log/amp/fb_dirty gates; invalid arguments, missing GPU and forced windows rejected')
