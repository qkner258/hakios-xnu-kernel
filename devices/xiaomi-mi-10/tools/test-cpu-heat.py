#!/usr/bin/env python3
"""Host-only execution of UMI governor and idle GPU policy with fake clocks/MMIO."""
from pathlib import Path
import subprocess
import tempfile

D = Path(__file__).resolve().parents[1]
X = D / 'overlays/xnu/files'
cpu = (X / 'bsd/dev/hakios.cpufreq.c').read_text()
gpu = (X / 'iokit/Kernel/hakios.gpu.cpp').read_text()


def function(text, name):
    start = text.index('\n' + name + '(') + 1
    return text[start:text.index('\n}', start) + 2]


prefix = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <setjmp.h>
#ifndef __unused
#define __unused __attribute__((unused))
#endif
#define MIN(a,b) ((a)<(b)?(a):(b))
#define MAX(a,b) ((a)>(b)?(a):(b))
#define MAX_CPUS 8
#define EPSS_DOMAINS 3
#define EPSS_PERF_STATE 0
#define CPUGOV_BOOST_MAX_MS 8
#define NSEC_PER_MSEC 1
#define NSEC_PER_SEC 1000
#define THREAD_UNINT 0
#define THREAD_CONTINUE_NULL 0
#define os_atomic_load(p,o) (*(p))
#define os_atomic_store(p,v,o) (*(p)=(v))
#define LCK_GRP_DECLARE(n,s) int n
#define LCK_MTX_DECLARE(n,g) int n
static void lck_mtx_lock(int *l) { assert(!*l); *l=1; }
static void lck_mtx_unlock(int *l) { assert(*l); *l=0; }
typedef int wait_result_t;
static int epss_top[3]={4,4,4},epss_cap[3]={4,4,4},epss_sleep_cap[3]={4,4,4},epss_row[3]={4,4,4};
static uint32_t epss_khz[3][5]={{300000,600000,900000,1200000,1500000},
 {300000,600000,900000,1200000,1500000},{300000,600000,900000,1200000,1500000}};
static uint32_t registers[3]={4,4,4}, cpugov_low_samples[3];
static uintptr_t epss_base[3];
static bool cpugov_enabled=true,dark;
static uint64_t cpugov_boost_until[3],cpugov_changed[3],clock_now,cpugov_boost_calls;
static bool cpugov_local_boost,global_file;
static unsigned physical=7;
#define __builtin_arm_rsr64(s) ((uint64_t)physical<<8)
static bool cpugov_switch(const char *p) {return strstr(p,"global-boost") && global_file;}
static void ddr_switch_update(void) {}
static void umi_idle_set_measurement(bool b) {(void)b;}
static bool umi_idle_snapshot(unsigned c,uint64_t *e,uint64_t *t) {(void)c;(void)e;(void)t;return false;}
static int current_thread(void) {return 0;}
static void thread_set_thread_name(int t,const char *n) {(void)t;(void)n;}
static int cpugov_wait;
static uint64_t mach_absolute_time(void) {return clock_now;}
static void nanoseconds_to_absolutetime(uint64_t n,uint64_t *t) {*t=n;}
static void absolutetime_to_nanoseconds(uint64_t n,uint64_t *t) {*t=n;}
uint32_t umi_pm_cpu_cap_khz(uint32_t d) {return dark && d>0 ? 900000 : 0;}
static char last_line[256],last_heat[256];
static unsigned lines;
static void tell(const char *fmt,...) {
 va_list a;va_start(a,fmt);vsnprintf(last_line,sizeof(last_line),fmt,a);va_end(a);lines++;
 if(strncmp(last_line,"heat: ",6)==0)strcpy(last_heat,last_line);
}
#define UMI_SLEEP_HAS_CPUGOV 1
#define UMI_SLEEP_HAS_EXTERNAL_CAP 1
#include "umi_cpufreq_screenoff.h"
typedef struct {unsigned cpu_id,phys_id;} cpu_t;
typedef struct {unsigned num_cpus;cpu_t cpus[8];} ml_topology_info_t;
/* Logical IDs deliberately differ from array indexes; physical core 0 is absent. */
static ml_topology_info_t topo={3,{{5,0x700},{2,0x100},{7,0x400}}};
static const ml_topology_info_t *ml_get_topology_info(void) {return &topo;}
static unsigned load[8];
static bool ml_cpu_idle_time(int id,uint64_t *idle) {
 assert(id==5 || id==2 || id==7);*idle=clock_now*(100-load[id])/100;return true;
}
static jmp_buf finished;
static uint64_t stop_at,deadline;
static void clock_interval_to_deadline(unsigned n,unsigned scale,uint64_t *d) {*d=clock_now+n*scale;}
static void assert_wait_deadline(void *e,int w,uint64_t d) {(void)e;(void)w;deadline=d;}
static void thread_block(int c) {(void)c;if(deadline>stop_at)longjmp(finished,1);clock_now=deadline;}
'''
body = ''
for ret, name in [('uint32_t', 'domain_of'), ('void', 'hakios_cpufreq_set_cap'),
                  ('uint32_t', 'hakios_cpufreq_khz'), ('void', 'hakios_cpufreq_cap'),
                  ('void', 'hakios_cpufreq_boost'), ('void', 'cpugov_update'), ('void', 'cpugov_thread')]:
    body += '\n' + ret + '\n' + function(cpu, name) + '\n'
main = r'''
static void tick(unsigned busy,uint64_t at) {
 clock_now=at;lck_mtx_lock(&umi_freq_lock);cpugov_update(1,busy,at,20);lck_mtx_unlock(&umi_freq_lock);
 assert(registers[1]==(unsigned)epss_row[1]);
}
int main(void) {
 for(unsigned d=0;d<3;d++)epss_base[d]=(uintptr_t)&registers[d];
 cpugov_local_boost=false; /* the old all-group vote first, for the governor path tests */
 tick(0,20);assert(epss_row[1]==4);tick(0,40);assert(epss_row[1]==0);
 clock_now=41;hakios_cpufreq_boost(40);assert(epss_row[1]==4 && cpugov_boost_until[1]==49);
 tick(0,60);assert(epss_row[1]==4);tick(0,80);assert(epss_row[1]==0); /* 8 ms hold, then two idle samples */
 tick(0,100);assert(epss_row[1]==0);
 tick(100,105);assert(epss_row[1]==4); /* load boost jumps directly to top */
 clock_now=106;hakios_cpufreq_boost(24);assert(epss_row[1]==4); /* input bypasses the transition delay */
 hakios_cpufreq_cap(1,1);assert(epss_row[1]==1);
 clock_now=107;hakios_cpufreq_boost(32);assert(epss_row[1]==1);
 hakios_cpufreq_cap(1,4);assert(epss_row[1]==1); /* release cannot pin idle at max */
 dark=true;hakios_cpufreq_boost(32);assert(epss_row[1]==2);
 hakios_cpufreq_set_cap(1,600000);hakios_cpufreq_boost(32);assert(epss_row[1]==1);
 hakios_cpufreq_set_cap(1,0);assert(epss_row[1]==1); /* independent caps */
 dark=false;memset(cpugov_boost_until,0,sizeof(cpugov_boost_until));
 tick(30,160);tick(30,180);assert(epss_row[1]==0);
 tick(100,200);assert(epss_row[1]==4); /* load boost jumps directly to top */
 tick(30,220);tick(30,240);assert(epss_row[1]==2); /* proportional downshift */
 /* Local (the default) raises the caller's group and nothing else: an event on the
  * prime core leaves the A55s and the A77s alone, and one from an A55 raises only it. */
 cpugov_local_boost=true;
 epss_row[0]=registers[0]=0;cpugov_boost_until[0]=0;cpugov_boost_until[2]=0;
 clock_now=300;hakios_cpufreq_boost(24);
 assert(epss_row[0]==0 && cpugov_boost_until[0]==0 && cpugov_boost_until[1]==0);
 assert(epss_row[2]==4 && cpugov_boost_until[2]==308);
 physical=2;clock_now=301;hakios_cpufreq_boost(24);
 assert(epss_row[0]==4 && cpugov_boost_until[0]==309 && cpugov_boost_until[2]==308);
 hakios_cpufreq_cap(0,1);clock_now=302;hakios_cpufreq_boost(32);assert(epss_row[0]==1);
 epss_cap[0]=4;physical=7;cpugov_local_boost=false;
 clock_now=100000;lines=0;stop_at=clock_now+20000;
 load[5]=100;load[2]=0;load[7]=50;
 if(!setjmp(finished))cpugov_thread(NULL,0);
 /* The first one-second poll reports the scope change to the default (local) once. */
 assert(lines==5 && strstr(last_line,"samples=500") && strstr(last_line,"sampled requests"));
 assert(strstr(last_heat," 7:100") && strstr(last_heat," 1:0") && strstr(last_heat," 4:50"));
 /* Switch polling is outside the frequency mutex; a change of scope clears the
  * event holds, leaving independent thermal/load policy intact. */
 global_file=true;clock_now=200000;stop_at=clock_now+10000;
 if(!setjmp(finished))cpugov_thread(NULL,0);
 assert(!cpugov_local_boost && strstr(last_line,"local=0"));
 global_file=false;clock_now=300000;stop_at=clock_now+10000;
 if(!setjmp(finished))cpugov_thread(NULL,0);
 assert(cpugov_local_boost && strstr(last_line,"local=1"));
 assert(epss_row[0]==0 && epss_row[2]==4 && !umi_freq_lock);
 puts("PASS: idle decay, local/global/caller boosts, rate limit, thermal/screen ceilings, sparse IDs, 10-second telemetry cadence");
}
'''
gpu_test = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#define NSEC_PER_MSEC 1
static int lock;
static int *gGPULock=&lock;
static bool gGPUReported,ready=true,fault,done=true,voted;
static uint64_t gGPULastSubmit=10,now=109;
static unsigned calls;
static void IOLockLock(int *p) {assert(!*p);*p=1;}
static void IOLockUnlock(int *p) {assert(*p);*p=0;}
static void nanoseconds_to_absolutetime(uint64_t n,uint64_t *t) {*t=n;}
static uint64_t mach_absolute_time(void) {return now;}
static bool hakios_gpu_ready(void) {return ready;}
static bool hakios_gpu_faulted(void) {return fault;}
static bool hakios_gpu_done(unsigned f) {(void)f;return done;}
static unsigned hakios_gpu_last(void) {return 4;}
static void umi_gpu_screenoff_vote(bool off) {assert(lock);calls++;voted=off;}
''' + '\nvoid\n' + function(gpu, 'umi_gpu_pm_update') + r'''
int main(void) {
 umi_gpu_pm_update(false);assert(calls==1 && !voted);
 now=110;umi_gpu_pm_update(false);assert(calls==2 && voted);
 done=false;umi_gpu_pm_update(true);assert(calls==2);
 done=true;fault=true;umi_gpu_pm_update(true);assert(calls==2);
 fault=false;gGPUReported=true;umi_gpu_pm_update(true);assert(calls==2);
 gGPUReported=false;ready=false;umi_gpu_pm_update(true);assert(calls==2);
 ready=true;gGPULastSubmit=now;umi_gpu_pm_update(false);assert(calls==3 && !voted);
 assert(!lock);
}
'''
with tempfile.TemporaryDirectory() as temp:
    root=Path(temp)
    (root/'kern').mkdir()
    (root/'kern/locks.h').write_text('')
    for name, text in [('cpu', prefix+body+main), ('gpu', gpu_test)]:
        source=root/(name+'.c'); binary=root/name
        source.write_text(text)
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-unused-variable',
                        '-fsanitize=address,undefined','-I'+str(root),'-I'+str(X/'bsd'),
                        '-I'+str(X/'bsd/dev'),str(source),'-o',str(binary)],check=True)
        subprocess.run([str(binary)],check=True)
print('PASS: GPU inactivity hold, in-flight fences, fault/teardown exclusion and shared lock')
