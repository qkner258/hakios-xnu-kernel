#!/usr/bin/env python3
"""Run the actual submit gate and DPU hazard/flip code against host fake registers."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
xnu = root / 'devices/xiaomi-mi-10/overlays/xnu/files'
gpu = (xnu / 'iokit/Kernel/hakios.gpu.cpp').read_text()
display = (xnu / 'bsd/dev/hakios.display.c').read_text()


def function(source, name):
    start = source.index('\n' + name + '(') + 1
    return source[start:source.index('\n}', start) + 2]


start = gpu.index('\t{', gpu.index('* Submit wait v29:'))
end = gpu.index('\tif (gGPUReported || !hakios_gpu_ready())', start)
gate = gpu[start:end]
program = r'''
#include <cassert>
#include <cstdint>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#define NSEC_PER_MSEC 1000000ULL
#define NSEC_PER_USEC 1000ULL
#define MSM_SUBMIT_BO_WRITE 2
#define CTL_FLUSH 0x18
#define SSPP_SRC_YSTRIDE0 0x24
static uint64_t now;
static unsigned gpuLock, displayLock, gpuLockStorage, dpu_submit_mtx;
static unsigned *gGPULock=&gpuLockStorage;
static unsigned ringWrites, queries, errors, sleeps, latchLogs;
static uint64_t releaseAt;
static bool remapOnSleep;
static bool dpu_ready=true, dpu_scaled, clocks=true, command_mode=true, compose_active, compose_failed;
static void compose_single() {}
static void compose_restore() {}
static int dpu_shown=0, dpu_pending=1, dpu_latency_log;
static uint32_t dpu_latched;
static uint64_t dpu_programmed_at;
static unsigned dpu_count=3, dpu_ctl, dpu_flush=0x20041, dpu_pipe, mdp=1, intf=2;
static uint64_t dpu_phys[3]={0x1000,0x2000,0x3000};
static uint32_t dpu_stride[3], flush=0x20041;
static uint32_t phase=100, written=100, te_idle_start=1250;
static uint32_t phases[2], phaseAt, phaseCount;
static struct {bool ok;unsigned shown;} scan={true,2340};
static uint64_t mach_absolute_time() {return now;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns) {*ns=t;}
static void lck_mtx_lock(unsigned *) {assert(!displayLock);displayLock=1;}
static void lck_mtx_unlock(unsigned *) {assert(displayLock);displayLock=0;}
static void IOLockLock(unsigned *) {assert(!gpuLock);gpuLock=1;}
static void IOLockUnlock(unsigned *) {assert(gpuLock);gpuLock=0;}
static bool regs_alive() {return clocks;}
static bool hakios_display_command_mode() {return scan.ok&&command_mode;}
static unsigned hakios_display_line(void *) {
 assert(displayLock);
 return phaseCount ? phases[phaseAt++ % phaseCount] : phase;
}
static uint32_t rd(unsigned base,unsigned offset) {
 assert(displayLock);
 if (base==mdp) {assert(offset==dpu_ctl+CTL_FLUSH);queries++;return flush;}
 assert(base==intf&&offset==0x2b0);return written;
}
static void wr(unsigned,unsigned,uint32_t) {assert(displayLock);}
static void dpu_program(uint64_t) {assert(displayLock);flush=dpu_flush;}
static int dpu_wait(uint64_t) {return 1;}
static bool PE_parse_boot_argn(const char *,uint32_t *,unsigned) {return false;}
static void display_log(const char *,...) {latchLogs++;}
#define printf display_log
'''
for name, kind in [('dpu_check_locked', 'static int'), ('dpu_check', 'static int'),
                   ('hakios_display_dpu_write_busy', 'unsigned'),
                   ('hakios_display_dpu_flip', 'int')]:
    program += '\n' + kind + '\n' + function(display, name) + '\n'
program += r'''
#undef printf
struct MiniIOSurface {uint64_t phys;};
static MiniIOSurface surfaces[4]={{0x1000},{0x2000},{0x3000},{0}};
struct gpu_buffer {MiniIOSurface *surface;};
static gpu_buffer buffers[4]={{surfaces},{surfaces+1},{surfaces+2},{surfaces+3}};
static gpu_buffer *buffer(unsigned h) {assert(gpuLock);return h&&h<=4?buffers+h-1:nullptr;}
static uint64_t aliasOf(const MiniIOSurface *s) {assert(gpuLock);return s?s->phys:0;}
struct msm_gem_submit_bo {uint32_t flags,handle;uint64_t presumed;};
struct msm_gem_submit {uint32_t nr_bos;};
static struct {unsigned scanWaits,pendingWaits,scanTimeouts;uint64_t aliasWait;} gGPUStat;
static void hakios_tl(const char *,...) {errors++;}
static void delay_for_interval(unsigned n,uint64_t unit) {
 assert(!gpuLock&&!displayLock);sleeps++;now+=n*unit;
 if(remapOnSleep)surfaces[0].phys=0x3000;
 if(releaseAt&&now>=releaseAt) {
  /* Move both aliased frames off screen, as subsequent flips would. */
  dpu_pending=-1;dpu_shown=2;
 }
}
static const int kIOReturnTimeout=1;
static int submit(msm_gem_submit_bo *bos,unsigned count) {
 msm_gem_submit req={count},*request=&req;
 uint64_t mark=0,lockTime=0,aliasTime=0;
 int ret=0;
'''
program += gate
program += r'''
 assert(gpuLock&&!displayLock);
 ringWrites++;
 IOLockUnlock(gGPULock);
 out:
 return ret;
}
static std::string live, kernel;
static void loader_log_line(const char *s) {live+=s;}
static void IOLog(const char *fmt,...) {
 char line[1024];va_list v;va_start(v,fmt);vsnprintf(line,sizeof(line),fmt,v);va_end(v);kernel+=line;
}
static void
'''
program += function(gpu, 'gpuSummary')
program += r'''
static void reset() {
 assert(!gpuLock&&!displayLock);
 now=0;ringWrites=queries=errors=sleeps=0;releaseAt=0;remapOnSleep=false;
 dpu_ready=clocks=command_mode=true;scan.ok=true;scan.shown=2340;
 dpu_shown=0;dpu_pending=1;flush=dpu_flush;phase=100;written=100;phaseAt=phaseCount=0;
 surfaces[0].phys=0x1000;gGPUStat={};
}
int main() {
 reset();assert(hakios_display_dpu_write_busy(0x1000)==1);
 assert(hakios_display_dpu_write_busy(0x2000)==2);
 assert(!hakios_display_dpu_write_busy(0x3000));
 phase=1300;written=2340;
 assert(!hakios_display_dpu_write_busy(0x1000));
 assert(hakios_display_dpu_write_busy(0x2000)==2);
 written=100;assert(hakios_display_dpu_write_busy(0x1000)==1);
 written=2340;phaseCount=2;phases[0]=2300;phases[1]=20;
 assert(hakios_display_dpu_write_busy(0x1000)==1);
 phaseCount=0;command_mode=false;assert(hakios_display_dpu_write_busy(0x1000)==1);
 flush=0;assert(!hakios_display_dpu_write_busy(0x1000));
 assert(hakios_display_dpu_write_busy(0x2000)==2); // latched but not software-observed
 assert(dpu_check()&&dpu_shown==1&&dpu_pending==-1);
 assert(hakios_display_dpu_write_busy(0x2000)==1); // repeat fetch: no successor
 clocks=false;assert(hakios_display_dpu_write_busy(0x2000)==1);
 assert(hakios_display_dpu_flip(2,100000000)&&dpu_pending==2);
 assert(hakios_display_dpu_write_busy(0x3000)==2);
 puts("PASS: current/pending/free, DMA-done, TE wrap, busy DMA, video mode, latch transition and clock loss");
 msm_gem_submit_bo read[]={{1,1,0},{1,2,0}},write[]={{2,1,0}},both[]={{3,1,0}},two[]={{2,1,0},{2,2,0}};
 reset();assert(!submit(read,2)&&ringWrites==1&&!queries&&!sleeps&&!gGPUStat.aliasWait);
 reset();write[0].handle=3;assert(!submit(write,1)&&!sleeps);write[0].handle=1;
 reset();releaseAt=1000000;assert(!submit(write,1)&&now==releaseAt&&ringWrites==1);
 assert(gGPUStat.scanWaits==1&&!gGPUStat.pendingWaits&&gGPUStat.aliasWait==1000000);
 reset();releaseAt=500000;assert(!submit(both,1)&&gGPUStat.scanWaits==1);
 reset();releaseAt=1000000;assert(!submit(two,2)&&gGPUStat.scanWaits==1&&gGPUStat.pendingWaits==1);
 reset();remapOnSleep=true;assert(!submit(write,1)&&sleeps==1); // re-lookup alias after unlock
 reset();assert(submit(write,1)==kIOReturnTimeout&&!ringWrites&&now==50000000);
 assert(gGPUStat.scanTimeouts==1&&gGPUStat.scanWaits==1&&gGPUStat.aliasWait==50000000&&errors==1);
 assert(submit(write,1)==kIOReturnTimeout&&!ringWrites&&errors==1); // one log per window
 reset();write[0].handle=2;
 assert(submit(write,1)==kIOReturnTimeout&&!ringWrites&&gGPUStat.pendingWaits==1);
 puts("PASS: read-only bypass, free writes, read/write hazards, both reasons, remap during wait, timeout refuses ring writes");
 std::string detail(700,'x');gpuSummary("the GPU worked %s; END\n",detail.c_str());
 assert(live==kernel&&live.size()>700&&live.find("; END\n")!=std::string::npos);
 puts("PASS: complete long GPU summary reaches live ring and kernel log");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-gpu-submit-') as tmp:
    src, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
    src.write_text(program)
    subprocess.run(['c++', '-std=c++17', '-O2', '-Wall', '-Wextra', '-Werror',
                    str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
