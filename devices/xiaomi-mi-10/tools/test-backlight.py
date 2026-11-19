#!/usr/bin/env python3
"""Host-only tests of production brightness mapping and DSI transaction code."""
from pathlib import Path
import re, subprocess, tempfile
root = Path(__file__).resolve().parents[3]
driver = (root/'devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/hakios.display.c').read_text()
bl = (root/'devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_backlight.c').read_text()
def function(src, name):
    start = src.index(name+'(')
    return src[start:src.index('\n}', start)+2]
defs = '\n'.join(re.findall(r'^#define (?:DSI_\w+|UMI_BL_\w+|BL_\w+)\s+.*$', driver+'\n'+bl, re.M))
program = r'''
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#define NSEC_PER_USEC 1000
static bool umi=true, alive=true, command_mode=true, window=true, completion=true, stuck=false, fault=false;
static uintptr_t dsi0=1, intf=2;
static uint32_t autorefresh=0x80000001;
static uint32_t regs[1024], packet[32], words, writes, bl_logs;
static uint64_t now;
static int bl_mode=1,bl_lock;
static bool bl_ready=true,bl_failed,bl_pending,bl_known,bl_no_idle_dim,bl_no_fade,bl_measure;
static unsigned bl_base=600,bl_factor=1000,bl_min_mnits,bl_target,bl_from,bl_level,bl_duration_ms,bl_requests;
static uint64_t bl_started;
static void lck_mtx_lock(int *p) {(void)p;}
static void lck_mtx_unlock(int *p) {(void)p;}
static void thread_wakeup(bool *p) {(void)p;}
static void umi_pm_display(bool off,const char *why) {(void)off;(void)why;}

static int regs_alive(void) {return alive;}
static int hakios_display_command_mode(void) {return command_mode;}
static int hakios_display_wait_copy(uint64_t n,uint64_t limit) {(void)n;(void)limit;return window;}
static uint64_t mach_absolute_time(void) {return now;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns) {*ns=t;}
static void delay_for_interval(unsigned n,unsigned unit) {now+=(uint64_t)n*unit;}
static uint32_t rd(uintptr_t base,uint32_t off) {if(base==intf){assert(off==0x2b4);return autorefresh;}return regs[off/4];}
'''+defs+r'''
static void wr(uintptr_t base,uint32_t off,uint32_t value) {
 (void)base;writes++;
 if (off==DSI_INT_CTRL) {regs[off/4]=(regs[off/4]&BL_IRQ_STATUS&~value)|(value&~BL_IRQ_STATUS);return;}
 if (off==DSI_TEST_PATTERN_GEN_CMD_DMA_INIT_VAL) packet[words++]=value;
 if (off==DSI_CMD_MODE_DMA_SW_TRIGGER) {
  /* Model the vendor FIFO's 64-bit fetch requirement. Missing the
   * extra DWORD produces the observed DMA underflow / permanent busy. */
  unsigned needed=(regs[DSI_DMA_CMD_LENGTH/4]/4+1)&~1u;
  if(words!=needed){regs[0xc/4]|=1u<<10;regs[DSI_STATUS/4]|=2;}
  else if (completion) regs[DSI_INT_CTRL/4]|=1;
  if (stuck) regs[DSI_STATUS/4]|=2;
  if (fault) regs[0x68/4]|=0x20;
 }
 regs[off/4]=value;
}
'''
program += '\nstatic void\n'+function(driver,'umi_dsi_backlight_packet')
for name in ['bl_map','bl_interpolate']:program += '\nstatic unsigned\n'+function(bl,name)
for name in ['bl_dma', 'bl_dcs', 'bl_request']:program += '\nstatic int\n'+function(bl,name)
program += r'''
static void reset(void) {
 memset(regs,0,sizeof(regs));words=writes=0;now=0;
 autorefresh=0x80000001;alive=command_mode=window=completion=true;stuck=fault=false;
 regs[DSI_CTRL/4]=5;
 regs[DSI_COMMAND_MODE_DMA_CTRL/4]=0x14000000;
 regs[DSI_TRIG_CTRL/4]=0x80000020;
 regs[DSI_DMA_CMD_LENGTH/4]=128;
 regs[DSI_INT_CTRL/4]=(1<<8)|(1<<9)|(1<<24)|(1<<25)|3; /* old DONE + unrelated W1C */
}
int main(void) {
 assert(bl_map(0,1000,0)==16);assert(bl_map(1000,1000,0)==2047);
 assert(bl_map(600,0,500000)==0);assert(bl_map(0,1000,500000)==2047);
 for(unsigned b=0;b<=1000;b++) {
  assert(bl_map(b,1000,0)<=2047);
  if(b)assert(bl_map(b,1000,0)>=bl_map(b-1,1000,0));
 }
 unsigned last=2047;
 for(unsigned t=0;t<=400;t++) {unsigned l=bl_interpolate(2047,0,t,400);assert(l<=last);last=l;}
 assert(last==0);assert(bl_interpolate(0,2047,500,400)==2047);
 assert(bl_interpolate(100,0,0,0)==0);
 reset();alive=false;assert(bl_dcs(0x51,100)==ENXIO&&writes==0);
 reset();window=false;assert(bl_dcs(0x51,100)==EAGAIN&&writes==0);
 reset();regs[DSI_STATUS/4]=4;assert(bl_dcs(0x51,100)==EAGAIN&&writes==0);
 reset();assert(bl_dcs(0x53,0x20)==0);assert(words==2&&packet[0]==0x80152053&&packet[1]==0);
 assert(regs[DSI_DMA_CMD_LENGTH/4]==128);
 assert(regs[DSI_TRIG_CTRL/4]==0x80000020&&regs[DSI_DMA_CMD_LENGTH/4]==128);
 assert(regs[DSI_TEST_PATTERN_GEN_CTRL/4]==0);
 assert(regs[DSI_INT_CTRL/4]==((1<<8)|(1<<9)|(1<<24)|(1<<25)|2));
 reset();window=false;autorefresh=1;
 assert(bl_dcs(0x53,0x20)==0); /* paused transaction ignores stale scan window */
 reset();uint32_t timing[11]={0};timing[0]=0x4039001e;timing[10]=0x80150226;
 assert(!bl_dma(timing,11,0x2f,90)&&words==12&&packet[11]==0);
 assert(!memcmp(packet,timing,sizeof(timing)));
 reset();assert(bl_dcs(0x51,2047)==0);assert(words==2&&packet[0]==0xc0390003&&packet[1]==0xffff0751);
 reset();assert(bl_dcs(0x51,0)==0);assert(packet[1]==0xff000051);
 reset();completion=false;assert(bl_dcs(0x51,100)==ETIMEDOUT);assert(now>=40000000&&now<41000000);
 assert(regs[DSI_TEST_PATTERN_GEN_CTRL/4]==0);
 reset();completion=false;stuck=true;assert(bl_dcs(0x51,100)==ETIMEDOUT);
 assert(regs[DSI_TEST_PATTERN_GEN_CTRL/4]==0x30006); /* don't reset an active DMA */
 reset();fault=true;assert(bl_dcs(0x51,100)==EIO);
 bl_no_idle_dim=true;bl_no_fade=true;
 assert(!bl_request(2,50,500)&&bl_factor==1000&&bl_target==1235&&bl_duration_ms==0);
 assert(!bl_request(2,0,185)&&bl_factor==0&&bl_target==0);
 assert(!bl_request(1,200,120)&&bl_base==200&&bl_target==0);
 assert(!bl_request(2,1000,185)&&bl_target==422);
 bl_no_idle_dim=bl_no_fade=false;
 assert(!bl_request(2,50,500)&&bl_target==21&&bl_duration_ms==500);
 puts("PASS: session A/B keeps display off, slider state and on; default fades restored");
 puts("PASS: brightness/fade endpoints, TE refusal, FIFO packets, W1C preservation, register restore, odd-DWORD FIFO padding, paused-mode undo, bounded timeout and quarantine");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-backlight-') as d:
    src=Path(d)/'test.c'; exe=Path(d)/'test';src.write_text(program)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(src),'-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True)
