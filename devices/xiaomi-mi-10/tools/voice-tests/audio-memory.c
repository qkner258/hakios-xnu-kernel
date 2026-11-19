/* Hardware-free address/page-table model for the actual mailbox allocator. */
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
typedef uintptr_t vm_offset_t;
typedef uint32_t ppnum_t;
#define PAGE_MASK 16383
#define PAGE_SHIFT 14
#define KMA_KOBJECT 1
#define KMA_ZERO 2
#define KMA_DATA 4
#define VM_KERN_MEMORY_IOKIT 1
#define KERN_SUCCESS 0
#define AUDIO_ADDRESSES 0x10000000ull
#define AUDIO_STREAM_BITS (1ull<<32)
#define AUDIO_PTE 0xe43ull
#define SMMU_CB_TLBIASID 0x610
#define SMMU_CB_TLBSYNC 0x7f0
#define SMMU_CB_TLBSTATUS 0x7f4
static void *kernel_map;
static int hakios_smmu_lock;
static bool locked,shared_ok=true,allocation_ok=true,tlb_busy;
static unsigned allocations,memory_gets,pins,barriers;
static struct { bool passed; uintptr_t tables; unsigned context; } space;
static struct { uintptr_t base; } apps_smmu_context={0x1000};
static void lck_mtx_lock(int *p){assert(p==&hakios_smmu_lock&&!locked);locked=true;}
static void lck_mtx_unlock(int *p){assert(p==&hakios_smmu_lock&&locked);locked=false;}
static int kmem_alloc_contig(void *map,uintptr_t *ptr,unsigned bytes,uintptr_t mask,ppnum_t max,ppnum_t pmask,int flags,int tag)
{
    assert(locked&&!map&&bytes==128*1024&&mask==16383&&max==0x3ffff&&!pmask&&flags==7&&tag==1);
    allocations++;if(!allocation_ok)return ENOMEM;
    *ptr=(uintptr_t)calloc(1,bytes);return KERN_SUCCESS;
}
static bool shared_get(unsigned user){assert(locked&&user==2);memory_gets++;if(shared_ok)pins|=1u<<user;return shared_ok;}
static uint64_t kvtophys(uintptr_t va){assert(va);return 0xfffe0000;}
static void put(void *block,const char *name,unsigned offset,unsigned value)
{
    (void)name;assert(locked&&block==&apps_smmu_context);
    assert((offset==SMMU_CB_TLBIASID&&value==space.context)||(offset==SMMU_CB_TLBSYNC&&value==1));barriers++;
}
static unsigned rd(uintptr_t address){assert(locked&&address==apps_smmu_context.base+SMMU_CB_TLBSTATUS);return tlb_busy;}
static void sleep_ms(unsigned ms){assert(locked&&ms==1);}
/* Test runner removes ARM cache maintenance instructions from this copy. */
#include "umi_voice_audio_memory_host.inc"

int main(void)
{
    uint64_t pa,adsp;
    allocation_ok=false;assert(umi_voice_audio_memory(&pa,&adsp)==ENOMEM&&!pins);
    allocation_ok=true;shared_ok=false;assert(umi_voice_audio_memory(&pa,&adsp)==ENXIO&&!pins);
    shared_ok=true;
    space.tables=(uintptr_t)calloc(1,3*4096);space.context=7;
    uint64_t *third=(uint64_t *)(space.tables+8192);
    // Existing playback/capture and adjacent unmapped pages remain intact.
    for(unsigned i=0;i<512;i++)third[i]=0xdead0000+i;
    assert(!umi_voice_audio_memory(&pa,&adsp)&&pa==0xfffe0000&&adsp==0x110020000ull&&pins==4);
    assert(barriers==2);
    for(unsigned i=0;i<512;i++)assert(third[i]==(i>=32&&i<64?((pa+(i-32)*4096)|AUDIO_PTE):0xdead0000+i));
    unsigned before=memory_gets;assert(!umi_voice_audio_memory(&pa,&adsp)&&memory_gets==before&&barriers==2);
    // Bypass is supported only using the allocated physical range and SID bits.
    uva_memory_mapped=false;space.passed=true;
    assert(!umi_voice_audio_memory(&pa,&adsp)&&adsp==0x1fffe0000ull&&barriers==2);
    uva_memory_mapped=false;space.passed=false;tlb_busy=true;
    assert(umi_voice_audio_memory(&pa,&adsp)==ETIMEDOUT&&!uva_memory_mapped&&pins==4);
    assert(!locked&&allocations==2); // retained allocation, no retry allocation/leak
    free((void *)uva_memory);free((void *)space.tables);
    puts("PASS voice-audio memory: contiguous allocation below 4 GiB, separate 128 KiB ADSP map, SID encoding, media/mic isolation, lifetime pin, allocation/map/TLBSYNC failure");
}
