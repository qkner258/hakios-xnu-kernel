#!/usr/bin/env python3
"""Host tests of actual calibration + mode transaction; never accesses hardware.

Reference: MP dtsi SHA256 8acd59b8a3e9b5de527be34fb8570fba621b18c07f24a5ffedc0ce2f6143b92c,
timing-switch-command at lines 153 and 294. Expected payloads are transcribed
independently of the driver word packing.
"""
from pathlib import Path
import subprocess
import tempfile

device = Path(__file__).resolve().parents[1]
driver = (device / 'overlays/xnu/files/bsd/dev/hakios.display.c').read_text()
session = (device / 'overlays/xnu/files/bsd/dev/umi_display_session.h').read_text()

def function(source, name):
    a = source.index(name + '(')
    return source[a:source.index('\n}', a) + 2]

expected = {
    60: 'B4 CB BB BB AA 99 77 66 00 00 00 B4 D0 D0 D0 D0 D0 86 86 3C 3C F2 F2 A8 A8 36 36 36 36 0A',
    90: 'B4 98 88 88 77 66 55 44 00 00 00 60 B8 B8 B8 B8 B8 C8 C8 D8 D8 E8 E8 F8 F8 48 48 48 48 0A',
}
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define NSEC_PER_SEC 1000000000ULL
#define NSEC_PER_MSEC 1000000
#define NSEC_PER_USEC 1000
#define KERN_SUCCESS 0
typedef int thread_t;
struct hakios_display_scan {int ok; uint32_t lines,shown,millihz; uint64_t line_ns,frame_ns;};
static struct hakios_display_scan scan;
static bool umi=true,command_mode=true,bl_known,bl_pending,bl_failed,dpu_ready=true;
static bool bl_no_idle_dim,bl_no_fade,bl_measure,request90,requestdim,requestfade,requestcomposeoff;
/* (the header's switches, sampled once by hakios_display_session_prepare(); the two readers it defines are not
 * part of this test) */
static bool bl_inplace_demote=true,bl_compose_copies=true;
static unsigned te_initial=2340,te_idle_start=1250,bl_mode=1,bl_lock;
static unsigned hz=60,dma_calls,undo_calls,fail_call;
static bool stalled,stuck90,undo_stalled,bad_config,fail90scan;
static uint64_t now;
static uintptr_t intf=1,dsi0=2;
static uint32_t config=0x180088,autorefresh=0x80000001;
static uint64_t mach_absolute_time(void) {return now;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns) {*ns=t;}
static void delay_for_interval(unsigned n,unsigned unit) {now+=(uint64_t)n*unit;}
static uint32_t rd(uintptr_t b,unsigned off) {
 if(b==dsi0){assert(off==DSI_STATUS);return 0;}
 uint64_t phase=stalled?0:now%(NSEC_PER_SEC/hz);
 switch(off) {
  case 0x284:return 1;
  case 0x288:return config;
  case 0x2b4:return bad_config?0x80000002:autorefresh;
  case 0x294:return 2340;
  case 0x298:return 2340+phase*19200000ULL/((config&0xffff)*NSEC_PER_SEC);
  case 0x2b0:if(fail90scan&&hz==90)return 2340;return phase>=8500000?2340:phase*2340/8500000;
 }
 assert(0);return 0;
}
static void wr(uintptr_t b,unsigned off,uint32_t value) {
 assert(b==intf);
 if(off==0x288)config=value;
 else {assert(off==0x2b4);autorefresh=value;}
}
static unsigned char expected60[30], expected90[30];
static int bl_dma(const uint32_t *packet,unsigned words,unsigned cmd,unsigned value) {
 assert(!(autorefresh&(1u<<31))&&!(config&(1u<<20)));
 dma_calls++;
 if(dma_calls==fail_call)return EIO;
 if(cmd==0xf0) {
  assert(words==3&&packet[0]==0xc0390006&&packet[1]==0x52aa55f0&&packet[2]==0xffff0008);
 } else {
  assert(cmd==0x2f&&words==11&&packet[0]==0x4039001e);
  assert(memcmp(packet+1,value==90?expected90:expected60,30)==0);
  assert(packet[8]>>16==0xffff);
  assert(packet[9]==(value==90?0x0015042f:0x0015012f));
  assert(packet[10]==(value==90?0x80150226:0x80150126));
  if(value==60){undo_calls++; if(undo_stalled)stalled=true;}
  hz=(value==90&&stuck90)?60:value;
 }
 return 0;
}
static bool display_switch_file(const char *name) {
 if(!strcmp(name,"90hz"))return request90;
 if(!strcmp(name,"no-idle-dim"))return requestdim;
 if(!strcmp(name,"no-fade"))return requestfade;
 if(!strcmp(name,"compose-off"))return requestcomposeoff;
 return false;
}
static void lck_mtx_lock(unsigned *p) {(void)p;}
static void lck_mtx_unlock(unsigned *p) {(void)p;}
static int hakios_display_command_mode(void) {return scan.ok&&command_mode;}
static int dpu_wait(uint64_t ns) {(void)ns;return 1;}
static bool PE_parse_boot_argn(const char *s,unsigned *p,unsigned n) {(void)s;(void)p;(void)n;return false;}
static void display_measure_worker(void) {}
static int kernel_thread_start(void(*f)(void),void *p,thread_t *t) {(void)f;(void)p;*t=1;return 0;}
static void thread_deallocate(thread_t t) {(void)t;}
'''
import re
program = '\n' + '\n'.join(re.findall(r'^#define DSI_\w+\s+.*$', driver, re.M)) + '\n' + program
for name, src in [('command_scan_start', driver), ('display_panel_timing', session),
                  ('hakios_display_session_prepare', session)]:
    program += '\nstatic int\n' + function(src, name)
program += '\nstatic void reset(void) {\n'
program += '''now=0;hz=60;dma_calls=undo_calls=fail_call=0;
config=0x180088;autorefresh=0x80000001;stalled=stuck90=undo_stalled=bad_config=fail90scan=false;
bl_known=bl_pending=bl_failed=false;dpu_ready=true;
request90=requestdim=requestfade=requestcomposeoff=false;
bl_inplace_demote=bl_compose_copies=true;
assert(command_scan_start());
}\nint main(void) {\n'''
for hz, payload in expected.items():
    values = ', '.join(hex(v) for v in bytes.fromhex(payload))
    program += f'{{const unsigned char p[]={{ {values} }};memcpy(expected{hz},p,30);}}\n'
program += r'''
 reset(); assert(scan.millihz>59000&&scan.millihz<61000);
 assert(bl_inplace_demote&&bl_compose_copies);   /* (the composition switches: on unless their file is there) */
 assert(!hakios_display_session_prepare());
 reset();requestcomposeoff=true;assert(!hakios_display_session_prepare()&&!bl_compose_copies&&bl_inplace_demote);
 reset(); requestdim=requestfade=true;assert(!hakios_display_session_prepare());
 assert(!dma_calls&&bl_no_idle_dim&&bl_no_fade);
 reset();request90=true;assert(!hakios_display_session_prepare());
 assert(hz==90&&dma_calls==2&&!undo_calls&&config==0x18005a&&autorefresh==0x80000001);
 assert(scan.millihz>89000&&scan.millihz<91000&&scan.lines>scan.shown);
 assert(te_idle_start<scan.lines&&scan.frame_ns>11000000&&scan.frame_ns<11200000);
 reset();request90=true;stuck90=true;assert(!hakios_display_session_prepare());
 assert(hz==60&&undo_calls==1&&scan.millihz>59000&&scan.millihz<61000);
 reset();request90=true;fail90scan=true;
 assert(!hakios_display_session_prepare());
 assert(hz==60&&undo_calls==1&&config==0x180088&&scan.millihz>59000&&scan.millihz<61000);
 reset();request90=true;fail_call=2;assert(!hakios_display_session_prepare());
 assert(hz==60&&undo_calls==1&&config==0x180088&&autorefresh==0x80000001);
 reset();request90=true;stuck90=true;undo_stalled=true;
 assert(!hakios_display_session_prepare()&&!bl_failed&&scan.ok);
 assert(config==0x180088&&autorefresh==0x80000001); /* keep framebuffer available */
 reset();request90=true;bl_known=true;assert(!hakios_display_session_prepare()&&!dma_calls&&hz==60);
 reset();request90=true;bad_config=true;assert(!hakios_display_session_prepare()&&!dma_calls&&hz==60);
 reset();struct hakios_display_scan saved=scan;stalled=true;
 assert(!command_scan_start()&&!memcmp(&scan,&saved,sizeof(scan)));
 puts("PASS: vendor LP packet bytes/batch boundaries, 60/90Hz calibration with distinct phase/image domains, switch gates, paused autorefresh, vendor vsync count, confirmed undo and framebuffer retained on calibration failure");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-display-session-') as tmp:
    src, exe = Path(tmp)/'test.c', Path(tmp)/'test'
    src.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
