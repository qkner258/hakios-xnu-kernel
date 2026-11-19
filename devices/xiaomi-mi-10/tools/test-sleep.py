#!/usr/bin/env python3
"""Host-only tests of real sleep policy/idle/GPU code and staging; no phone I/O."""
import importlib.util
import subprocess
import tempfile
from pathlib import Path

D = Path(__file__).resolve().parents[1]
R = D.parents[1]
X = D / 'overlays/xnu/files'
spec = importlib.util.spec_from_file_location('stage_sleep', D / 'tools/stage-sleep.py')
stage = importlib.util.module_from_spec(spec)
spec.loader.exec_module(stage)


def function(text, name):
    start = text.index('\n' + name + '(') + 1
    return text[start:text.index('\n}', start) + 2]


def run(source, directory, name):
    c = directory / (name + '.c')
    exe = directory / name
    c.write_text(source)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
                    '-I' + str(directory), '-I' + str(X / 'bsd'), str(c), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)


with tempfile.TemporaryDirectory() as temp:
    t = Path(temp)
    (t / 'kern').mkdir()
    (t / 'kern/locks.h').write_text('''
#define LCK_GRP_DECLARE(n,s) int n
#define LCK_MTX_DECLARE(n,g) int n
static void lck_mtx_lock(int *l) { assert(!*l); *l=1; }
static void lck_mtx_unlock(int *l) { assert(*l); *l=0; }
''')
    tracked = subprocess.run(['git', '-C', str(R), 'cat-file', '-e',
        'HEAD:devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/hakios.cpufreq.c'],
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0
    if tracked:
        original = subprocess.check_output(['git', '-C', str(R), 'show',
            'HEAD:devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/hakios.cpufreq.c'], text=True)
    else:
        # The UMI port may be staged in an uncommitted worktree. Use the
        # checked-in upstream template as the fixed-frequency fixture then.
        staged = subprocess.run(['git', '-C', str(R), 'show',
            ':devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/hakios.cpufreq.c'],
            capture_output=True, text=True)
        original = staged.stdout if staged.returncode == 0 else (X / 'bsd/dev/hakios.cpufreq.c').read_text()
    concurrent = (X / 'bsd/dev/hakios.cpufreq.c').read_text()
    for name, source in [('fixed', original), ('governor', concurrent)]:
        staged = stage.cpufreq_hooks(source)
        assert stage.cpufreq_hooks(staged) == staged
        has_gov = 'static bool cpugov_enabled;' in source
        separate_cap = has_gov and 'static int epss_sleep_cap[' in source
        pre = '''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define EPSS_DOMAINS 3
#define EPSS_PERF_STATE 0
#define MIN(a,b) ((a)<(b)?(a):(b))
#define tell(...) ((void)0)
static uint32_t registers[3] = {4,4,4};
static uintptr_t epss_base[3];
static int epss_top[3] = {4,4,4};
static uint32_t epss_khz[3][5] = {{300000,600000,900000,1200000,1500000},
 {300000,600000,900000,1200000,1500000},{300000,600000,900000,1200000,1500000}};
static bool dark;
uint32_t umi_pm_cpu_cap_khz(uint32_t d) { return dark && d > 0 && d < 3 ? 900000 : 0; }
'''
        if has_gov:
            pre += 'static int epss_cap[3]={4,4,4}, epss_sleep_cap[3]={4,4,4}, epss_row[3]={4,4,4};\nstatic bool cpugov_enabled;\n'
        pre += '#define UMI_SLEEP_HAS_CPUGOV ' + str(int(has_gov)) + '\n'
        pre += '#define UMI_SLEEP_HAS_EXTERNAL_CAP ' + str(int(separate_cap)) + '\n'
        pre += '#include "' + str(X / 'bsd/dev/umi_cpufreq_screenoff.h') + '"\nvoid\n'
        pre += function(staged, 'hakios_cpufreq_cap')
        if separate_cap:
            pre += '\nvoid\n' + function(staged, 'hakios_cpufreq_set_cap')
        main = '''
int main(void) {
 for(unsigned d=0; d<3; d++) epss_base[d]=(uintptr_t)&registers[d];
 hakios_cpufreq_cap(1,4); assert(registers[1]==4);
 dark=true; umi_cpufreq_refresh(); assert(registers[1]==2 && registers[2]==2 && registers[0]==4);
 hakios_cpufreq_cap(1,1); assert(registers[1]==1);
 dark=false; umi_cpufreq_refresh(); assert(registers[1]==1 && registers[2]==4); /* thermal survives wake */
 dark=true; hakios_cpufreq_cap(1,4); assert(registers[1]==2); /* thermal release cannot bypass screen cap */
 hakios_cpufreq_cap(0,4); assert(registers[0]==4);
 hakios_cpufreq_cap(1,-3); assert(registers[1]==0);
 hakios_cpufreq_cap(1,99); assert(registers[1]==2);
 hakios_cpufreq_cap(99,1); assert(registers[0]==4);
 dark=false; umi_cpufreq_refresh(); assert(registers[1]==4);
 assert(!umi_freq_lock);
'''
        if separate_cap:
            main += '''
 hakios_cpufreq_set_cap(1,600000); assert(registers[1]==1 && epss_cap[1]==4);
 umi_cpufreq_refresh(); assert(registers[1]==1); /* periodic refresh preserves separate cap */
 dark=true; hakios_cpufreq_set_cap(1,0); umi_cpufreq_refresh(); assert(registers[1]==2);
 dark=false; umi_cpufreq_refresh(); assert(registers[1]==4);
 dark=true; hakios_cpufreq_set_cap(1,1200000); assert(registers[1]==2); /* setter clamps immediately */
 hakios_cpufreq_cap(1,1); dark=false; hakios_cpufreq_set_cap(1,0);
 umi_cpufreq_refresh(); assert(registers[1]==1); /* releasing separate cap preserves thermal */
 hakios_cpufreq_cap(1,4); assert(registers[1]==4);
'''
        if has_gov:
            main += 'cpugov_enabled=true; dark=true; umi_cpufreq_refresh(); assert(registers[1]==4); assert(umi_freq_limit(1,4)==2);\n'
        run(pre + main + 'return 0; }', t, name)

    # The loader's real whitelist and validation, including complete supervised args.
    loader = (D / 'loader/loader.c').read_text()
    a = loader.index('static int\numi_bootargs_ok(')
    b = loader.index('\nstatic int\nchoose_window(', a)
    run('''
#include <assert.h>
#include <stdint.h>
#include <string.h>
#define print(...) ((void)0)
static int platform_umi; static int *PLAT=&platform_umi;
struct payload_info {uint64_t forced_base; const char *cmdline;};
static uint64_t str_len(const char *s) {return strlen(s);}
static int starts_with(const char *s,const char *p) {return strncmp(s,p,strlen(p))==0;}
''' + loader[a:b] + '''
int main(void) {
 struct payload_info p={0};
 const char *good[]={"idle=0 screenoff_pm=0", "idle=1 screenoff_pm=1 screenoff_cpu_mhz=900 screenoff_gpu=1 screenoff_wifi=1",
 "bootcpu=7 cpus=7 loader_return_after=180 umi_keys=1 umi_touch=1 gpu=1 metal=1 fb_scan=1 cpufreq=1 battery=1 umi_charge_diag=1 idle=1 screenoff_pm=1"};
 const char *new_good="bootcpu=0 cpus=8 cpufreq=1 cpugov=1 cpugov_log=1 amp=1 fb_scan=1 fb_diff=1 fb_dirty=1";
 const char *bad[]={"idle=2","idle=11","screenoff_pm=2","screenoff_pm=1x","screenoff_gpu=2","screenoff_wifi=11","syssleep=1","suspend=1", "cpugov=1", "fb_dirty=1 fb_scan=1"};
 for(unsigned i=0;i<sizeof(good)/sizeof(*good);i++){p.cmdline=good[i];assert(umi_bootargs_ok(&p));}
 p.cmdline=new_good; assert(umi_bootargs_ok(&p));
 for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);i++){p.cmdline=bad[i];assert(!umi_bootargs_ok(&p));}
 return 0;
}
''', t, 'loader')
    run('''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define HFI_NO_INDEX 0xff
#define HFI_GX_BW_PERF_VOTE 30
#define printf(...) ((void)0)
static bool umi_a650=true,gpu_kept=true,gmu_talks=true,ack=true;
static uint32_t umi_gpu_boot_index=5,umi_gpu_boot_khz=587000,core_khz=587000,calls,index_seen;
static bool hfi_send(const char *s,uint32_t id,uint32_t *v,uint32_t n) {
 (void)s; assert(id==30 && n==4 && v[1]==1 && v[3]==0xff); calls++;index_seen=v[2];return ack;
}
#include "''' + str(X / 'bsd/dev/umi_gpu_screenoff.h') + '''"
int main(void) {
 umi_gpu_screenoff_vote(false);assert(!calls);
 gpu_kept=false;umi_gpu_screenoff_vote(true);assert(!calls);gpu_kept=true;
 umi_gpu_screenoff_vote(true);assert(calls==1 && index_seen==1 && core_khz==305000);
 umi_gpu_screenoff_vote(true);assert(calls==1);
 umi_gpu_screenoff_vote(false);assert(calls==2 && index_seen==5 && core_khz==587000);
 ack=false;umi_gpu_screenoff_vote(true);assert(calls==3 && core_khz==587000);
 ack=true;umi_gpu_screenoff_vote(true);assert(calls==3);
 return 0;
}
''', t, 'gpu')
    (t / 'os').mkdir()
    (t / 'os/atomic_private.h').write_text("""
#define os_atomic_load(p,o) (*(p))
#define os_atomic_store(p,v,o) (*(p)=(v))
#define os_atomic_add(p,v,o) (*(p)+=(v))
#define os_atomic_inc(p,o) (++*(p))
""")
    run('''
#include <assert.h>
#include <stdint.h>
#include <stdbool.h>
#define MAX_CPUS 8
static unsigned bootarg, logical=2, physical=2, calls;
static uint64_t ticks;
static int PE_parse_boot_argn(const char *s,void *p,unsigned n) {(void)s;(void)n;*(unsigned*)p=bootarg;return 1;}
#define __builtin_arm_rsr64(s) ((uint64_t)physical<<8)
#define __builtin_arm_wfi() ((void)(calls++,ticks+=500))
static unsigned cpu_number(void) {return logical;}
static uint64_t mach_absolute_time(void) {return ticks;}
#include "''' + str(X / 'osfmk/arm64/umi_idle.h') + '''"
int main(void) {
 uint64_t e,t;
 bootarg=0;umi_idle_configure();umi_idle_wait();assert(calls==1 && !umi_idle_snapshot(2,&e,&t));
 bootarg=2;umi_idle_configure();umi_idle_wait();assert(calls==2 && !umi_idle_snapshot(2,&e,&t));
 bootarg=1;umi_idle_configure();umi_idle_wait();assert(umi_idle_snapshot(2,&e,&t)&&e==1&&t==500);
 physical=0;umi_idle_wait();assert(calls==4 && umi_idle_snapshot(2,&e,&t)&&e==1&&t==500);
 bootarg=0;umi_idle_configure();physical=2;umi_idle_set_measurement(true);
 umi_idle_wait();assert(calls==5 && umi_idle_snapshot(2,&e,&t)&&e==2&&t==1000);
 umi_idle_set_measurement(false);umi_idle_wait();assert(calls==6 && !umi_idle_snapshot(2,&e,&t));
 assert(!umi_idle_snapshot(8,&e,&t));return 0;
}
''', t, 'idle')
    # Staging drift must fail, rather than producing an uncapped kernel.
    try:
        stage.cpufreq_hooks(original.replace('hakios_cpufreq_cap(', 'changed_api('))
    except ValueError:
        pass
    else:
        raise AssertionError('missing anchor was accepted')
print('PASS: actual fixed/governor cap writers, thermal restoration, loader domain, GPU ACK/failure, WFI accounting/exclusion, loader gate, idempotent staging and drift rejection')
