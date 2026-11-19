#!/usr/bin/env python3
"""Compile production display composition against fake MMIO; never accesses a phone."""
from pathlib import Path
import re
import subprocess
import tempfile

root = Path(__file__).resolve().parents[3]
dev = root / 'devices/xiaomi-mi-10'
c = (dev / 'overlays/xnu/files/bsd/dev/hakios.display.c').read_text()
compose = (dev / 'overlays/xnu/files/bsd/dev/umi_dpu_compose.h').read_text()
contract = (dev / 'overlays/xnu/files/iokit/Kernel/umi_dpu_layer.h').read_text()
fb = (dev / 'overlays/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp').read_text()

def function(source, name):
    start = re.search(r'^' + re.escape(name) + r'\(', source, re.M).start()
    start = source.rfind('\n', 0, start) + 1
    end = source.index('\n}', start) + 2
    return source[start:end]

state = c[c.index('#define DPU_WIMG_IO'):c.index('static void compose_single(void);')]
program = r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
typedef uintptr_t vm_offset_t;
typedef uintptr_t vm_size_t;
#define LCK_GRP_DECLARE(a,b) int a
#define LCK_MTX_DECLARE(a,b) int a
static void lck_mtx_lock(int *p) { assert(!*p); *p=1; }
static void lck_mtx_unlock(int *p) { assert(*p); *p=0; }
#define KERN_SUCCESS 0
#define KMA_KOBJECT 0
#define KMA_ZERO 0
#define KMA_DATA 0
#define VM_KERN_MEMORY_IOKIT 0
#define NSEC_PER_USEC 1000
#define NSEC_PER_MSEC 1000000
#define PAGE_SIZE 16384
#define round_page(x) (((x)+16383)&~16383u)
#define kernel_map 0
static uint32_t mmio[0x4a000/4], smmu[0x20000/4];
static uint64_t now, latch_at;
static unsigned writes, mappings, allocations, flushes;
static bool requested, idle=true, hold_latch;
static int kmem_alloc(int map, vm_offset_t *out, size_t bytes, int flags, int tag) {
    (void)map; (void)flags; (void)tag;
    void *p=aligned_alloc(16384, round_page(bytes)); assert(p);
    memset(p, 0, bytes); *out=(uintptr_t)p; allocations++; return 0;
}
static void kmem_free(int map, vm_offset_t ptr, size_t bytes) { (void)map;(void)bytes;free((void*)ptr); }
static uint64_t kvtophys(vm_offset_t p) { return (p & 0xfffffff) + 0x100000000ull; }
static void flush_dcache64(uint64_t p, unsigned bytes, int phys) { (void)p;(void)bytes;(void)phys;flushes++; }
static vm_offset_t ml_io_map_unmappable(uint64_t p, size_t size, unsigned flags) {
    (void)p;(void)size;(void)flags;mappings++;return (uintptr_t)smmu;
}
static bool display_switch_file(const char *name) { assert(!strcmp(name,"dpu-compose"));return requested; }
static uint64_t mach_absolute_time(void) { return now; }
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns) { *ns=t; }
static void delay_for_interval(uint32_t us, unsigned unit) { now+=(uint64_t)us*unit; }
static bool regs_alive(void) { return true; }
static int hakios_display_wait_copy(uint64_t bytes, uint64_t limit) { (void)bytes;(void)limit;return idle; }
static uint32_t hakios_display_line(void *unused) { (void)unused; return 100; }
static bool hakios_bootlog_pause(void) { return true; }
static void dpu_bootlog_resume(void) { }        /* the boot log's frame: not under test here */
static int PE_parse_boot_argn(const char *name, uint32_t *out, size_t size) { (void)name;(void)out;(void)size;return 0; }
static void hakios_tl(const char *fmt, ...) { (void)fmt; }
static struct { bool ok; uint32_t lines; uint64_t line_ns; } scan;
static bool command_mode;
static uint32_t rd(vm_offset_t base,uint32_t offset) {
    if (base == (uintptr_t)mmio && offset == 0x1018 && now>=latch_at && !hold_latch) mmio[offset/4]=0;
    return *(uint32_t*)(base+offset);
}
static void wr(vm_offset_t base,uint32_t offset,uint32_t value) {
    writes++;
    if(base>=(uintptr_t)smmu && base<(uintptr_t)(smmu+0x20000/4) && offset==0x58) value=0;
    *(uint32_t*)(base+offset)=value;
    if (base==(uintptr_t)mmio && offset==0x1018) latch_at=now+16666667;
}
'''
program += contract + state
program += c[c.index('static const uint32_t qseed_regs'):c.index('static void\ndpu_geo_read_pipe')]

program += '\nstatic bool compose_active, compose_failed;\nstatic void compose_single(void);\nstatic void compose_restore(void);\n'
for name, typ in [('dpu_program','static void'), ('dpu_geo_read_pipe','static void'), ('dpu_geo_read','static void'),
                  ('dpu_geo_write_pipe','static void'), ('dpu_geo_write','static void'),
                  ('dpu_check_locked','static int'), ('dpu_check','static int'), ('dpu_wait','static int'),
                  ('hakios_display_dpu_flip','int'), ('dpu_restore_locked','static void'),
                  ('hakios_display_dpu_restore','void')]:
    program += '\n' + typ + '\n' + function(c, name) + '\n'
program += compose
program = re.sub(r'__asm__ volatile\([^;]+;', '(void)0;', program)
program += r'''
static struct umi_dpu_layer full(unsigned rgba) {
    struct umi_dpu_layer l={.width=1125,.height=2436,.stride=4608,.rgba=rgba,
      .src={0,0,1125,2436},.dst={0,0,1080,2340}};return l;
}
static void initial(void) {
    mdp=(uintptr_t)mmio; dpu_ready=dpu_scaled=dpu_vig=true;dpu_count=3;
    dpu_pipe=0x4000; dpu_ctl=0x1000;dpu_lm=0;dpu_flush=0x20041;
    dpu_srcs=1;dpu_src[0].reg=0x4014;dpu_shown=0;dpu_pending=-1;
    dpu_base=0x9c000000;dpu_phys[0]=dpu_base;dpu_phys[1]=0x9cac0000;dpu_phys[2]=0x9d580000;
    for(unsigned i=0;i<3;i++)dpu_stride[i]=4608;
    mmio[0x1000/4]=0x1000005;mmio[0x44004/4]=0x9240438;
    mmio[0x4000/4]=0x9840465;mmio[0x400c/4]=0x9240438;mmio[0x4024/4]=4608;
    mmio[0x4030/4]=0x237ff;mmio[0x4034/4]=0x03020001;mmio[0x4038/4]=0x80000000;
    /* ARM SMMUv2, 8 contexts at +0x8000, 4 SMRs. */
    smmu[0x20/4]=4;smmu[0x24/4]=0x20000008;
    smmu[0x800/4]=0x84020820;smmu[0xc00/4]=0x10000;
    smmu[0x804/4]=0x80000123;smmu[0xc04/4]=0; /* CB0 belongs to another stream */
    smmu[(0x8000+0x1000)/4]=1; /* CB1 enabled: don't touch it */
}
int main(void) {
    initial(); assert(!hakios_display_compose_start());assert(!mappings && !writes && !allocations);
    requested=true; mmio[0x1200/4]=0x8;
    assert(!hakios_display_compose_start());assert(!mappings && !writes);mmio[0x1200/4]=0;
    smmu[0x800/4]=0x87ff0820;assert(!hakios_display_compose_start());assert(!writes && !allocations);
    smmu[0x800/4]=0x84020820;
    assert(hakios_display_compose_start()); assert(allocations==9 && flushes==1);
    assert((smmu[0xc00/4]&0x300ff)==2); /* unused disabled CB2 */
    assert(smmu[0xc04/4]==0 && smmu[(0x8000+0x1000)/4]==1);
    uint64_t *root=(uint64_t*)compose_tables;
    assert(root[0]==(kvtophys(compose_tables+4096)|3));
    uint64_t *l2=(uint64_t*)(compose_tables+3*4096);
    assert(l2[(0x9c000000u>>21)&511]==(0x9c000000ull|0xec1));
    uint64_t *l3=(uint64_t*)(compose_tables+5*4096);
    assert(l3[0]==(kvtophys(compose_sources[0][0])|0xec3));
    l3=(uint64_t*)(compose_tables+11*4096);assert(l3[0]==0); /* guard after 12 MiB */
    puts("PASS: off does no MMIO; occupied pipe/broad stream refused; private CB and immutable bank page tables preserve splash/other streams");
    struct umi_dpu_layer l[4]={full(0),full(1),full(0),full(1)};
    struct dpu_geo g;
    assert(compose_geometry(l,&g));assert(g.src_size==0x9840465 && g.out_size==0x9240438);
    assert(g.qseed[1]==2184533 && g.qseed[2]==2183189); /* independent 1125/1080, 2436/2340 golden values */
    struct umi_dpu_layer bad=l[0];bad.dst[0]=-1;assert(!compose_geometry(&bad,&g));
    bad=l[0];bad.src[0]=1;assert(!compose_geometry(&bad,&g));
    bad=l[0];bad.stride=65536;assert(!compose_geometry(&bad,&g));
    bad=l[0];bad.rgba=2;assert(!compose_geometry(&bad,&g));
    bad=l[0];bad.dst[2]=1;assert(!compose_geometry(&bad,&g));
    assert(!hakios_display_compose_validate(l,1));assert(hakios_display_compose_validate(l,4));
    for(unsigned count=2;count<=4;count++) {
        int bank=hakios_display_compose_begin(100000000);assert(bank>=0);
        assert(hakios_display_compose(bank,l,count));
        /* CTL stage fields: BASE=1, blend stage0=2, stage1=3, stage2=4. */
        uint32_t stage=0x1000001|(2u<<3)|(count>=3 ? 3u<<6 : 0)|(count==4 ? 4u<<26 : 0);
        assert(mmio[0x1000/4]==stage);
        assert(mmio[0x4034/4]==0x03020001 && mmio[0x6034/4]==0x03010002);
        assert(mmio[0x44020/4]==0x600 && mmio[0x44024/4]==0xff0000);
        /* Premultiplied foreground coeff=1, background coeff=1-alpha.
         * Transparent, half-alpha, opaque sources preserve the CPU over contract. */
        unsigned alphas[3]={0,128,255};
        for(unsigned i=0;i<3;i++) {
            unsigned a=alphas[i];
            unsigned src=a*80/255,dst=170;
            uint32_t op=mmio[0x44020/4],alpha=mmio[0x44024/4];
            unsigned fg=(op&3)==0 ? (alpha>>16)&255 : 0;
            unsigned bg=((op>>8)&3)==2 ? a : alpha&255;
            if(op&(1u<<10))bg=255-bg;
            unsigned hw=src*fg/255+dst*bg/255;
            unsigned expected=src+dst*(255-a)/255;
            assert(hw==expected);
        }
        int next=hakios_display_compose_begin(100000000);
        assert(next==1-bank && dpu_shown==4+bank && dpu_pending==-1);
    }
    puts("PASS: 2/3/4 stages, BGRA/RGBA unpack, premultiplied alpha, QSEED scale and geometry limits");
    assert(hakios_display_compose(1,l,4));
    assert(hakios_display_compose_begin(100000000)==0);
    assert(hakios_display_dpu_flip(1,100000000));
    assert(!compose_active && mmio[0x1000/4]==0x1000005 && mmio[0x4024/4]==4608);
    assert(dpu_wait(100000000));assert(dpu_shown==1);
    assert(hakios_display_compose_begin(100000000)==0);
    assert(hakios_display_compose(0,l,2));hold_latch=true;
    assert(hakios_display_compose_begin(1000000)==-1);hold_latch=false;
    assert(!compose_ready && compose_failed && compose_routed);
    hakios_display_dpu_restore();
    assert(!compose_ready && !compose_routed && smmu[0xc00/4]==0x10000 && dpu_shown==0);
    puts("PASS: pending banks cannot be reused; hardware-to-single flip restores stage/stride; loader restore retires sources before revoking the SMMU route");
    for(unsigned b=0;b<2;b++)for(unsigned i=0;i<4;i++)free((void*)compose_sources[b][i]);
    free((void*)compose_tables);
}
'''
with tempfile.TemporaryDirectory(prefix='umi-compose-') as tmp:
    src = Path(tmp) / 'compose.c'
    binary = Path(tmp) / 'compose'
    src.write_text(program)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
                    '-Wno-unused-variable', str(src), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Compile the actual CPU alias reader and target selection block, including a
# source whose old backing store is intentionally stale. Replay >16 mixed swaps.
reader=function(fb,'IOMobileFramebuffer::sourcePixels').replace('IOMobileFramebuffer::','')
copy=function(fb,'carveCopy')
start=fb.index('\t\tfor (unsigned i = 0; i < fDpuBuffers && dpuTarget < 0;',fb.index('dpuTarget = zeroSlot'))
end=fb.index('\t\t/* (fb_dpu=4: only a frame no surface is aliased onto',start)
selection=fb[start:end]
program=r'''
#include <cassert>
#include <cstdint>
#include <cstring>
#include <cstdio>
struct MiniIOSurface { struct { unsigned bytesPerRow,height; } info; uint8_t *old; const uint8_t *pixels(){return old;} };
static unsigned fDpuBuffers=3;
static bool fZero=true,fGraphics=true,fSlotBlack[3];
static MiniIOSurface *fBound[3];
static uintptr_t fDpuVirt[3];
static uint8_t *fAliasRead;
static int current,pending_hw;
static int hakios_display_dpu_flip(unsigned p,uint64_t){current=p;pending_hw=-1;return 1;}
static int hakios_display_dpu_shown(){return current;}
static uint64_t fake_now;
static uint64_t mach_absolute_time(){return fake_now++;}
static void nanoseconds_to_absolutetime(uint64_t ns,uint64_t *t){*t=ns;}
static void absolutetime_to_nanoseconds(uint64_t t,uint64_t *ns){*ns=t;}
static bool demote_on=true, demote_wins=true;
static bool hakios_display_inplace_demote(){return demote_on;}      /* (the (a2) block: a frame given back) */
static unsigned demote_calls, demote_probes, demote_rounds;
static bool zeroDemote(unsigned i,uint64_t fence_ms,uint64_t budget_ms,bool probe){
    assert(i<3); assert(budget_ms>=2);                     /* (never asked for a demotion with no budget) */
    demote_calls++;
    if(probe) demote_probes++; else demote_rounds++;
    if(fence_ms!=0 && !probe) assert(fence_ms<=6);         /* (the bounded round: DEMOTE_FENCE_MS at most) */
    if(!demote_wins) return false;
    if(fBound[i]==nullptr) return false;
    fBound[i]=nullptr; return true;
}
#define NSEC_PER_MSEC 1000000
#define DEMOTE_BUDGET_MS 12
#define DEMOTE_FENCE_MS 6
#define DPU_FLIP_LIMIT_NS 100000000
#define DPU_INPLACE_LIMIT_NS 20000000
#define SWAP_SURFACE(x) (x)
#define MIN(a,b) ((a)<(b)?(a):(b))
static unsigned swapWord(const uint8_t *s,unsigned n){return s[n];}
'''
program+='static void\n'+copy+'\nconst uint8_t *\n'+reader
program+='\nstatic void choose(const uint8_t *swap,unsigned set,int pending,int scanned,int &dpuTarget,bool &zero,bool &inPlace){\n'+selection+'\n}\n'
program+=r'''
int main(){
 uint8_t old[64]={1},actual[64]={77},scratch[64]={0};
 MiniIOSurface surface={{16,4},old},other={{16,4},old};
 fBound[0]=&surface;fBound[2]=&other;fDpuVirt[0]=fDpuVirt[2]=(uintptr_t)actual;fAliasRead=scratch;
 assert(sourcePixels(&surface)==scratch && scratch[0]==77 && old[0]==1);
 assert(fBound[0]==&surface && sourcePixels(&other)==scratch);
 uint8_t swap[4]={9,8,7,6};
 /* Demotion off (/var/db/hakios-display/inplace-demote-off): the only frame free is the one shown. */
 demote_on=false;
 for(unsigned n=0;n<64;n++){
   int target=-1;bool zero=false,inplace=false;current=1;pending_hw=-1;
   choose(swap,15,-1,1,target,zero,inplace);
   assert(target==1 && inplace && !zero && fBound[0]==&surface && fBound[2]==&other);
 }
 assert(demote_calls==0);
 demote_on=true;demote_wins=true;demote_calls=demote_probes=demote_rounds=0;
 /* Demotion on: the first round gives a frame back with no fence waited for at all -- the composed frame goes
    into it and is flipped to (not written into the frame the panel fetches: inplace is false). */
 { int target=-1;bool zero=false,inplace=false;current=1;pending_hw=-1;
   fBound[0]=&surface;fBound[2]=&other;
   choose(swap,15,-1,1,target,zero,inplace);
   assert(target==0 && !inplace && !zero && fBound[0]==nullptr && fBound[2]==&other); }
 assert(demote_probes==1 && demote_rounds==0);          /* (a surface still drawn into is left alone first) */
 /* ... and a probe that finds the GPU still drawing into that surface has the bounded round after it. */
 { int target=-1;bool zero=false,inplace=false;current=1;pending_hw=-1;
   demote_wins=false;demote_calls=demote_probes=demote_rounds=0;
   fBound[0]=&surface;fBound[2]=&other;
   choose(swap,15,-1,1,target,zero,inplace);
   assert(target==1 && inplace);                        /* (nothing given back: the in-place path, as before) */
   assert(demote_probes>=1 && demote_rounds>=1);        /* (both rounds were tried) */ }
 demote_wins=true;
 demote_on=false;                                       /* (the two below are the (a2) block switched off) */
 int target=-1;bool zero=false,inplace=false;current=0;pending_hw=1;
 choose(swap,15,1,0,target,zero,inplace);assert(target==1 && inplace && current==1);
 fBound[0]=nullptr;target=-1;inplace=false;
 choose(swap,15,-1,1,target,zero,inplace);assert(target==0 && !inplace);
 puts("PASS: CPU composition reads current alias pixels without remapping; 64 copied swaps retain both aliases; pending copy slot retires before DMA-idle update; a demotable alias gives the composed frame a free frame in the non-blocking round");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-zero-') as tmp:
    src=Path(tmp)/'zero.cpp';binary=Path(tmp)/'zero'
    src.write_text(program)
    subprocess.run(['c++','-std=c++17','-Wall','-Wextra','-Werror',str(src),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
