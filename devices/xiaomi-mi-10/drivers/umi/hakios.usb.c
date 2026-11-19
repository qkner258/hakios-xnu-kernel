/* UMI/SM8250 USB2 peripheral bring-up, opt-in with usb=1.
 * Register sequences: Linux gcc-sm8250, dwc3-qcom, femto-v2, dwc3/ep0.
 * Stage 1e exposes EP0 diagnostics and a vendor bulk log snapshot transport.
 * The loader must acknowledge exclusive ownership before any USB MMIO write.
 */
#include <kern/clock.h>
#include <kern/locks.h>
#include <kern/thread.h>
#include <kern/sched_prim.h>
#include <libkern/libkern.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <pexpert/device_tree.h>
#include <sys/systm.h>
#include <sys/proc.h>
#include <sys/hakios.net.h>
#include <net/kpi_interface.h>
#include "umi_usb_ntb.h"
#include <sys/fcntl.h>
#include <sys/mount.h>
#include <sys/vnode.h>
#include <sys/vnode_if.h>
#include <sys/conf.h>
#include <sys/select.h>
#include <sys/uio.h>
#include <sys/file.h>
#include <sys/ioccom.h>
#include <sys/socketvar.h>
#include <miscfs/devfs/devfs.h>
#include <vm/memory_types.h>
#include <vm/vm_kern.h>

extern vm_offset_t ml_io_map(vm_offset_t, vm_size_t);
extern uint64_t kvtophys(vm_offset_t);
extern void IOSleep(unsigned int);
extern void thread_set_thread_name(thread_t, const char *);
extern kern_return_t thread_terminate(thread_t);
extern int umi_rpmh_usb_power(void);
extern lck_mtx_t hakios_smmu_lock;
extern void hakios_tl(const char *, ...) __printflike(1, 2);
extern void hakios_usbmux_host(int connected); /* hakios.usbmux.cpp: lockdownd's USB-host signal */
#define printf hakios_tl

#define BIT(n) (1U << (n))
#define USB_BASE 0x0a600000ULL
#define GCC_BASE 0x00100000ULL
#define PHY_BASE 0x088e3000ULL
#define SMMU_BASE 0x15000000ULL
#define CORE 0xc000
#define QSCRATCH 0xf8800
#define GCTL 0x110
#define GSNPSID 0x120
#define GHWPARAMS3 0x14c
#define GUSB2PHYCFG 0x200
#define GUSB3PIPECTL 0x2c0
#define GEVNTADRLO 0x400
#define GEVNTADRHI 0x404
#define GEVNTSIZ 0x408
#define GEVNTCOUNT 0x40c
#define DCFG 0x700
#define DCTL 0x704
#define DEVTEN 0x708
#define DSTS 0x70c
#define DALEPENA 0x720
#define DEP(n) (0x800 + 16 * (n))
#define PAR2 0
#define PAR1 4
#define PAR0 8
#define CMD 12
#define SETEPCFG 1
#define XFERRES 2
#define SETSTALL 4
#define CLEARSTALL 5
#define STARTXFER 6
#define ENDXFER 8
#define STARTCFG 9
#define ACTIVE BIT(10)
#define TRB_HWO BIT(0)
#define TRB_LST BIT(1)
#define TRB_CHN BIT(2)
#define TRB_IOC BIT(11)
#define TRB_ISP BIT(10)
#define TRB_SETUP (2U << 4)
#define TRB_STATUS2 (3U << 4)
#define TRB_STATUS3 (4U << 4)
#define TRB_DATA (5U << 4)
#define EVENT_SIZE 4096U
#define DMA_SIZE 0x18000U
#define DMA_MUX_OUT 0x10000U
#define DMA_MUX_IN 0x14000U
#define MUX_CHUNK 0x4000U
#define DMA_NCM_OUT 0x4000U
#define DMA_NCM_IN 0x8000U
#define DMA_NCM_NOTIFY 0xc000U
#define DMA_SETUP 0x1000U
#define DMA_TRB_OUT 0x2000U
#define DMA_TRB_IN 0x2080U
#define DMA_TRB_BULK_OUT 0x2100U
#define DMA_TRB_BULK_IN 0x2180U
#define DMA_BULK_OUT 0x2800U
#define DMA_BULK_IN 0x3000U
#define BULK_SIZE 4096U
#define DATA_SIZE 1024U
#define LOG_HEAD (256U * 1024)
#define LOG_RING (1024U * 1024)
#define HANDOFF_MAGIC 0x32425355494d55ULL /* "UMIUSB2": firmware IRQs quiesced before ack */

/* Prefix of the existing loader_live ABI in arm64/loader_return.c. */
extern struct {
    uint64_t magic, heartbeat;
    const char *log;
    const uint32_t *len, *ring_at;
    const bool *wrapped;
    uint64_t head, ring;
} loader_live;

struct usb_trb { uint32_t lo, hi, size, control; };
struct usb_setup { uint8_t type, request; uint16_t value, index, length; };
struct usb_handoff { uint64_t magic; volatile uint64_t request, ack; };
static volatile uint32_t *regs, *gcc, *phy, *wrapper;
static volatile struct usb_handoff *handoff;
static uint8_t *dma, *events;
static uint64_t dma_pa, event_pa;
static unsigned event_pos, resource[10], configuration;
static bool transfer_active[10];
static bool three_stage, need_zlp, setup_pending, failed, control_out;
static uint32_t event_total, last_event;
static unsigned data_len, requests;
static enum { SETUP, DATA, DATA_ZLP, STATUS_WAIT, STATUS, DATA_OUT } phase;
static uint8_t log_snapshot[LOG_HEAD + LOG_RING];
static uint32_t snapshot_len, snapshot_id;
static uint64_t snapshot_heartbeat;
static unsigned bulk_at, bulk_queued;
static bool bulk_busy, bulk_header, bulk_halted[2];
static bool ncm_enabled, ncm_attached, ncm_alt, ncm_link;
static unsigned ncm_input_size = NCM_NTB_MAX, ncm_filter, ncm_sequence, ncm_notify;
static unsigned ncm_out_length;
static bool ncm_halted[10]; /* by physical endpoint; 8/9 are the mux pair */
static uint64_t ncm_rx, ncm_tx, ncm_bad, ncm_drop;
static uint64_t ncm_queued, ncm_dequeued, ncm_submitted, ncm_completed, ncm_cancelled, ncm_flushed;
static unsigned ncm_tx_status;
static uint32_t ncm_tx_size, ncm_tx_control;
static uint64_t ncm_bad_reason[NCM_ERRORS], ncm_zlp, ncm_zero_block, ncm_pad;
static uint64_t ncm_unaligned, ncm_half_null, ncm_multi;
static unsigned ncm_bad_dumps;
static bool ncm_rx_reported;
#define NCM_QUEUE 64U
static struct { unsigned len; uint8_t data[NCM_FRAME_MAX]; } ncm_queue[NCM_QUEUE];
static unsigned ncm_head, ncm_count;
static LCK_GRP_DECLARE(ncm_group, "umi-usb-ncm");
static LCK_MTX_DECLARE(ncm_lock, &ncm_group);
static bool ncm_queue_up;
/* Apple usbmux interface 3 (FF/FE/02, EP 04/84). The kernel only moves bytes:
 * one OUT chunk and one IN packet (one write = one USB transfer) are owned at a
 * time; /redwood/bin/usbmux speaks the protocol through /dev/hakios-usbmux.
 * mux_reset tells the daemon once that the host session ended. */
static LCK_MTX_DECLARE(mux_lock, &ncm_group);
static unsigned mux_out_len, mux_out_at, mux_in_len;
static bool mux_up, mux_reset, mux_in_zlp;
static uint64_t mux_rx, mux_tx;
static struct selinfo mux_select_info;
static bool ncm_transmit(mbuf_t packet, size_t length)
{
    bool ok = false;
    lck_mtx_lock(&ncm_lock);
    if (ncm_queue_up && length >= 14 && length <= NCM_FRAME_MAX && ncm_count < NCM_QUEUE) {
        unsigned at = (ncm_head + ncm_count) % NCM_QUEUE;
        if (!mbuf_copydata(packet, 0, length, ncm_queue[at].data)) {
            ncm_queue[at].len = (unsigned)length; ++ncm_count; ++ncm_queued; ok = true;
        }
    }
    if (!ok) ++ncm_drop;
    lck_mtx_unlock(&ncm_lock);
    return ok;
}
static const struct hakios_net_link ncm_net = {
    .name = "usb-ncm", .unit = 2, .address = {2,0x42,0,0,0,1}, .transmit = ncm_transmit
};

static uint32_t rr(volatile uint32_t *base, unsigned off) { return base[off / 4]; }
static void wr(volatile uint32_t *base, unsigned off, uint32_t val)
{
    base[off / 4] = val;
    __asm__ volatile("dsb sy" ::: "memory");
}
static void rmw(volatile uint32_t *base, unsigned off, uint32_t mask, uint32_t val)
{
    wr(base, off, (rr(base, off) & ~mask) | (val & mask));
}
static bool wait_bits(volatile uint32_t *base, unsigned off, uint32_t mask, uint32_t val)
{
    for (unsigned i = 0; i < 10000; ++i) {
        if ((rr(base, off) & mask) == val) return true;
        delay(1);
    }
    printf("[usb] timeout off=%#x mask=%#x want=%#x got=%#x\n", off, mask, val, rr(base, off));
    return false;
}

/* Buffers are normal cached contiguous RAM; DWC3 is not DMA coherent.
 * Keep each CPU-written object on separate cache lines. Event lines are only
 * invalidated after their initial clean: the CPU never dirties an active ring.
 */
static void dma_sync(void *ptr, unsigned len, bool to_device)
{
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    unsigned line = 4U << ((ctr >> 16) & 15);
    uintptr_t end = (uintptr_t)ptr + len;
    for (uintptr_t p = (uintptr_t)ptr & ~(uintptr_t)(line - 1); p < end; p += line) {
        if (to_device) __asm__ volatile("dc cvac, %0" :: "r"(p) : "memory");
        else __asm__ volatile("dc ivac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}
static bool alloc_dma(uint8_t **buf, uint64_t *pa, unsigned size)
{
    vm_offset_t va = 0;
    if (kmem_alloc_contig(kernel_map, &va, size, 0, 0, 0,
        KMA_KOBJECT | KMA_ZERO | KMA_DATA, VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) return false;
    *buf = (uint8_t *)va;
    *pa = kvtophys(va);
    dma_sync(*buf, size, true);
    return true;
}

/* The UEFI normally maps SID0 and SID0x20 through a shared context. Replace
 * only the exact SID0 routing after Stop, using an otherwise unused context.
 * The shared lock also protects Wi-Fi/PCIe's context allocation. */
#define TABLE_BYTES (8U * 4096)
#define PTE_ADDR 0x0000fffffffff000ULL
static volatile uint32_t *smmu, *usb_cb;
static int usb_smr_index = -1;
static uint8_t *tables;
static uint64_t tables_pa;
static unsigned table_count = 1;
static uint64_t *next_table(uint64_t *entry)
{
    if (!*entry) {
        if (table_count == 8) return NULL;
        *entry = (tables_pa + 4096 * table_count++) | 3;
    }
    return (uint64_t *)(tables + ((*entry & PTE_ADDR) - tables_pa));
}
static bool map_buffer(uint64_t pa, unsigned len)
{
    if (pa & 4095 || pa + len > (1ULL << 36)) return false;
    for (uint64_t end = pa + len; pa < end; pa += 4096) {
        uint64_t *l1 = (uint64_t *)tables;
        uint64_t *l2 = next_table(&l1[(pa >> 30) & 511]);
        if (!l2) return false;
        uint64_t *l3 = next_table(&l2[(pa >> 21) & 511]);
        if (!l3) return false;
        /* Normal non-cacheable DMA, unprivileged RW, outer-shareable, NX. */
        l3[(pa >> 12) & 511] = pa | 3 | BIT(6) | (2U << 8) | BIT(10) | BIT(11) |
            (1ULL << 53) | (1ULL << 54);
    }
    return true;
}
static bool smmu_prepare(bool install)
{
    bool ok = false, bypass = false, used[256] = { false };
    int match = -1, free_match = -1;
    unsigned hits = 0;
    if (!smmu) smmu = (void *)ml_io_map(SMMU_BASE, 0x100000);
    if (!smmu) return false;
    volatile uint32_t *s = smmu;
    lck_mtx_lock(&hakios_smmu_lock);
    uint32_t scr = rr(s, 0), id0 = rr(s, 0x20), id1 = rr(s, 0x24);
    unsigned matches = id0 & 255, contexts = id1 & 255;
    unsigned page = id1 & BIT(31) ? 65536 : 4096;
    unsigned pages = 2U << ((id1 >> 28) & 7);
    printf("[usb] smmu sid=0 scr0=%#x idr0=%#x idr1=%#x install=%u\n", scr, id0, id1, install);
    if (scr == ~0U || !(id0 & BIT(27)) || (scr & BIT(3)) ||
        2 * pages * page != 0x100000 || !contexts) goto out;
    if (scr & BIT(0)) { ok = bypass = !(scr & BIT(10)); goto out; }
    for (unsigned i = 0; i < matches; ++i) {
        uint32_t smr = rr(s, 0x800 + 4 * i), s2cr = rr(s, 0xc00 + 4 * i);
        if (!(smr & BIT(31))) { if (free_match < 0) free_match = i; continue; }
        used[s2cr & 255] = true;
        if ((smr & 0x7fff) & ~((smr >> 16) & 0x7fff)) continue;
        ++hits; match = i;
        printf("[usb] smmu match=%u smr=%#x s2cr=%#x\n", i, smr, s2cr);
        unsigned type = (s2cr >> 16) & 3, cb = s2cr & 255;
        if (type == 1) bypass = true;
        else if (type == 0 && cb < contexts) {
            uint32_t cbar = rr(s, page + cb * 4), ctl = rr(s, (pages + cb) * page);
            printf("[usb] smmu inherited cb=%u cbar=%#x sctlr=%#x\n", cb, cbar, ctl);
            bypass = ((cbar >> 16) & 3) == 1 && !(ctl & BIT(0));
        }
    }
    if (hits > 1) goto out;
    usb_smr_index = match;
    if (!hits) bypass = !(scr & BIT(10));
    if (bypass) { ok = true; goto out; }
    if (match >= 0 && rr(s, 0x800 + 4 * match) != BIT(31)) {
        printf("[usb] shared/wildcard SID0 match; refusing replacement\n"); goto out;
    }
    if (match < 0) match = free_match;
    unsigned cb;
    for (cb = 0; cb < contexts; ++cb)
        if (!used[cb] && !(rr(s, (pages + cb) * page) & BIT(0))) break;
    if (match < 0 || cb == contexts) goto out;
    printf("[usb] smmu %s exact SID0 match=%d dedicated cb=%u\n", install ? "install" : "plan", match, cb);
    if (!install) { ok = true; goto out; }
    usb_cb = s + (pages + cb) * page / 4;
    uint64_t root = tables_pa | ((uint64_t)cb << 48);
    wr(usb_cb, 0, 0);
    wr(s, page + 0x800 + 4 * cb, 1); /* AArch64 tables */
    wr(s, page + 4 * cb, 0x1f300);   /* S1 translation, S2 bypass */
    wr(usb_cb, 0x10, 0x38011);      /* 36-bit PA, 16-bit ASID */
    wr(usb_cb, 0x30, 0x803519);     /* 39-bit IOVA, 4 KiB tables, WB walks */
    wr(usb_cb, 0x20, (uint32_t)root); wr(usb_cb, 0x24, root >> 32);
    wr(usb_cb, 0x28, 0); wr(usb_cb, 0x2c, 0);
    wr(usb_cb, 0x38, 0xf404ff44); wr(usb_cb, 0x3c, 0);
    wr(usb_cb, 0x58, 0xc00001fe);
    wr(usb_cb, 0x610, cb); wr(usb_cb, 0x7f0, 1);
    if (!wait_bits(usb_cb, 0x7f4, 1, 0)) goto out;
    wr(usb_cb, 0, 0x127); /* translation, fault recording, no IRQ/stall */
    if (!(rr(usb_cb, 0) & 1) || rr(usb_cb, 0x20) != (uint32_t)root ||
        rr(usb_cb, 0x24) != (uint32_t)(root >> 32) ||
        (rr(s, page + cb * 4) & 0x30000) != 0x10000) goto out;
    usb_smr_index = match;
    wr(s, 0xc00 + 4 * match, cb);
    wr(s, 0x800 + 4 * match, BIT(31));
    ok = rr(s, 0x800 + 4 * match) == BIT(31) && (rr(s, 0xc00 + 4 * match) & 0x300ff) == cb;
    if (ok) printf("[usb] smmu SID0 mapped event+DMA only; tables=%#llx cb=%u\n", tables_pa, cb);
out:
    lck_mtx_unlock(&hakios_smmu_lock);
    if (bypass && ok) printf("[usb] smmu bypass confirmed\n");
    if (!ok) printf("[usb] smmu refused; no controller DMA started\n");
    return ok;
}

static bool take_controller(void)
{
    DTEntry chosen;
    const void *value;
    unsigned size;
    uint64_t pa;
    if (SecureDTLookupEntry(NULL, "/chosen", &chosen) != kSuccess ||
        SecureDTGetProperty(chosen, "umi-usb-handoff", &value, &size) != kSuccess || size != 8) {
        printf("[usb] loader lacks ownership handshake; refusing takeover\n");
        return false;
    }
    memcpy(&pa, value, sizeof(pa));
    handoff = (void *)ml_io_map(pa, sizeof(*handoff));
    if (!handoff || handoff->magic != HANDOFF_MAGIC) {
        printf("[usb] loader ownership ABI2 required; refusing takeover\n"); return false;
    }
    printf("[usb] requesting loader release\n");
    handoff->request = 1;
    __asm__ volatile("dsb sy" ::: "memory");
    for (unsigned i = 0; i < 2000 && !handoff->ack; ++i) IOSleep(1);
    if (handoff->ack != 1) {
        printf("[usb] loader release failed ack=%llu\n", handoff->ack);
        return false;
    }
    printf("[usb] exclusive controller ownership ABI2 acknowledged; firmware IRQs quiesced\n");
    return true;
}

static bool rcg(unsigned cmd, uint32_t cfg)
{
    rmw(gcc, cmd + 4, 0x371f, cfg);
    rmw(gcc, cmd, BIT(0), BIT(0));
    return wait_bits(gcc, cmd, BIT(0), 0);
}
static bool platform_start(void)
{
    gcc = (void *)ml_io_map(GCC_BASE, 0x100000);
    phy = (void *)ml_io_map(PHY_BASE, 0x1000);
    wrapper = (void *)ml_io_map(USB_BASE, 0x100000);
    if (!gcc || !phy || !wrapper) return false;
    regs = wrapper + CORE / 4;
    /* GCC_USB30_PRIM_BCR, then the retained power domain. */
    rmw(gcc, 0xf000, 1, 1); delay(100);
    rmw(gcc, 0xf000, 1, 0);
    rmw(gcc, 0xf004, 7, 0);
    if (!wait_bits(gcc, 0xf004, BIT(31), BIT(31))) return false;
    /* GPLL0 is shared. Vote its existing 600 MHz configuration, never retune. */
    rmw(gcc, 0x52018, BIT(0), BIT(0));
    if (!wait_bits(gcc, 0x0, BIT(31), BIT(31))) return false;
    if (!rcg(0xf020, (1U << 8) | 5)) return false; /* GPLL0 / 3 = 200 MHz */
    /* RO UTMI post-divider must be /1 for XO=19.2 MHz. */
    if (rr(gcc, 0xf050) & 3) {
        printf("[usb] unexpected UTMI postdiv=%#x\n", rr(gcc, 0xf050)); return false;
    }
    if (!rcg(0xf038, 1)) return false; /* XO / 1 */
    static const unsigned clocks[] = { 0xf07c, 0xf010, 0xf080, 0xf018, 0xf01c, 0x8c010 };
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i) {
        rmw(gcc, clocks[i], 1, 1);
        if (!wait_bits(gcc, clocks[i], BIT(31), 0)) return false;
    }
    printf("[usb] gcc gdsc=%#x core-cfg=%#x utmi-cfg=%#x clocks on\n",
        rr(gcc, 0xf004), rr(gcc, 0xf024), rr(gcc, 0xf03c));
    /* ACTIVE votes also cover firmware that stopped these rails on USB Stop. */
    if (umi_rpmh_usb_power()) return false;
    delay(100);
    rmw(gcc, 0x12000, 1, 1); delay(100);
    rmw(gcc, 0x12000, 1, 0); delay(100);
    /* Femto 7nm, 19.2 MHz reference. No tuning overrides in the umi DTS. */
    rmw(phy, 0x94, BIT(1), BIT(1));
    rmw(phy, 0x50, BIT(1), BIT(1));
    rmw(phy, 0x54, 0x70, 0);
    rmw(phy, 0x58, BIT(5) | BIT(4), BIT(5) | BIT(4));
    rmw(phy, 0xa0, 3, 2);
    rmw(phy, 0x60, BIT(0), BIT(0));
    rmw(phy, 0x5c, BIT(0), BIT(0));
    rmw(phy, 0x64, BIT(2) | BIT(3), BIT(2) | BIT(3));
    rmw(phy, 0x3c, BIT(0), BIT(0));
    rmw(phy, 0x54, BIT(2), 0);
    rmw(phy, 0x50, BIT(1), 0);
    rmw(phy, 0x64, BIT(3), 0);
    rmw(phy, 0x94, BIT(1), 0);
    delay(100);
    printf("[usb] femto common0=%#x common1=%#x ref=%#x\n", rr(phy, 0x54), rr(phy, 0x58), rr(phy, 0xa0));
    /* UTMI-as-PIPE and forced peripheral session valid, as dwc3-qcom. */
    rmw(wrapper, QSCRATCH + 8, BIT(8), BIT(8)); delay(100);
    rmw(wrapper, QSCRATCH + 8, BIT(0) | BIT(3), BIT(0) | BIT(3)); delay(100);
    rmw(wrapper, QSCRATCH + 8, BIT(8), 0);
    rmw(wrapper, QSCRATCH + 0x10, BIT(20) | BIT(28), BIT(20) | BIT(28));
    uint32_t id = rr(regs, GSNPSID);
    printf("[usb] probe base=%#llx snpsid=%#x hwparam3=%#x\n", USB_BASE, id, rr(regs, GHWPARAMS3));
    bool usb31 = (id & 0xffff0000U) == 0x33310000U;
    if (!usb31 && (id & 0xffff0000U) != 0x55330000U) return false;
    uint32_t version = usb31 ? rr(regs, 0x1a0) : id;
    printf("[usb] DWC_usb%s version=%#x\n", usb31 ? "31" : "3", version);
    /* Peripheral role before the device soft reset. */
    rmw(regs, GCTL, (3U << 12) | (3U << 4) | BIT(11), 2U << 12);
    wr(regs, DCTL, BIT(30));
    /* usb31 >=1.90a needs >50 ms to synchronize clocks before clearing reset. */
    if (usb31) {
        for (unsigned i = 0; i < 10 && (rr(regs, DCTL) & BIT(30)); ++i) IOSleep(20);
    }
    if (!wait_bits(regs, DCTL, BIT(30), 0)) return false;
    if (usb31 && version <= 0x3138302a) IOSleep(50);
    /* No U2 suspend/ENBLSLPM, no USB3 or U1/U2 entry. Retain PHYIF. */
    rmw(regs, GUSB2PHYCFG, BIT(31) | BIT(6) | BIT(8), 0);
    rmw(regs, GUSB3PIPECTL, BIT(31) | BIT(17), BIT(17));
    wr(regs, DCTL, 0);
    return true;
}

/* One vendor interface with working bulk OUT/IN, 512-byte HS packets.
 * 18d1:1000 is a local bring-up identity, not an Apple/usbmux identity. */
static const uint8_t device_desc[] = {
    18,1,0,2,0,0,0,64,0xd1,0x18,0x00,0x10,1,1,1,2,3,1
};
static const uint8_t config_desc[] = {
    9,2,32,0,1,1,0,0x80,50, 9,4,0,0,2,0xff,0,0,0,
    7,5,0x01,2,0,2,0, 7,5,0x81,2,0,2,0
};
/* Interface 0 keeps the diagnostic protocol; IAD groups NCM interfaces 1/2;
 * interface 3 is Apple's usbmux. Host MAC string4=024200000002; device en2
 * MAC=02:42:00:00:00:01. Apple's usbmuxd claims any IOUSBInterface with
 * idVendor 05ac and FF/FE/02, whatever the product. 12c0 is outside
 * AppleUSBHostiOSDevice (05ac:1290-12bf) and usbmuxd's own product list, so
 * macOS keeps the generic CDC composite driver that binds NCM, and nothing
 * re-selects the configuration. iSerialNumber becomes the usbmuxd UDID. */
static const uint8_t ncm_device_desc[] = {
    18,1,0,2,0xef,2,1,64,0xac,0x05,0xc0,0x12,2,1,1,2,3,1
};
static const uint8_t ncm_config_desc[] = {
    9,2,140,0,4,1,0,0x80,50,
    9,4,0,0,2,0xff,0,0,0, 7,5,1,2,0,2,0, 7,5,0x81,2,0,2,0,
    8,11,1,2,2,0x0d,0,0,
    9,4,1,0,1,2,0x0d,0,0,
    5,0x24,0,0x20,1, /* CDC1.20 */
    5,0x24,6,1,2,    /* union */
    13,0x24,0x0f,4,0,0,0,0,0xea,5,0,0,0, /* Ethernet max segment1514 */
    6,0x24,0x1a,0,1,1, /* NCM1.00; packet filter supported, NTB16 only */
    7,5,0x82,3,16,0,9,
    9,4,2,0,0,0x0a,0,1,0,
    9,4,2,1,2,0x0a,0,1,0, 7,5,3,2,0,2,0, 7,5,0x83,2,0,2,0,
    9,4,3,0,2,0xff,0xfe,2,0, 7,5,4,2,0,2,0, 7,5,0x84,2,0,2,0
};
static const uint8_t ncm_mac_desc[] = {26,3,'0',0,'2',0,'4',0,'2',0,'0',0,'0',0,'0',0,'0',0,'0',0,'0',0,'0',0,'2',0};
static const uint8_t lang_desc[] = { 4,3,9,4 };
static const uint8_t mfr_desc[] = { 14,3,'H',0,'a',0,'k',0,'i',0,'O',0,'S',0 };
static const uint8_t prod_desc[] = { 22,3,'U',0,'M',0,'I',0,' ',0,'U',0,'S',0,'B',0,' ',0,'g',0,'1',0 };
static const uint8_t serial_desc[] = { 18,3,'u',0,'m',0,'i',0,'-',0,'x',0,'n',0,'u',0,'1',0 };
/* NCM mode: usbmuxd's UDID is iSerialNumber and must equal lockdownd's
 * UniqueDeviceID: the legacy SHA1 of the loader's serial, ECID (decimal), Wi-Fi
 * and BT MACs (loader.c, iPhone10,3 identity). Check: mg_test UniqueDeviceID. */
static const uint8_t udid_desc[] = { 82,3,'2',0,'7',0,'d',0,'a',0,'0',0,'0',0,'a',0,'1',0,'1',0,'1',0,'8',0,'5',0,'3',0,'0',0,'4',0,'a',0,'a',0,'7',0,'a',0,'e',0,'b',0,'a',0,'c',0,'7',0,'d',0,'e',0,'8',0,'1',0,'5',0,'4',0,'2',0,'1',0,'2',0,'f',0,'b',0,'6',0,'7',0,'7',0,'9',0,'6',0 };

static bool dep_cmd(unsigned ep, unsigned cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
    unsigned base = DEP(ep);
    wr(regs, base + PAR0, p0); wr(regs, base + PAR1, p1); wr(regs, base + PAR2, p2);
    wr(regs, base + CMD, cmd | ACTIVE);
    /* usb31 cannot poll ENDTRANSFER completion. No IOC, then a 1 ms drain,
     * as dwc3_send_gadget_ep_cmd; the buffers remain allocated throughout. */
    if ((cmd & 15) == ENDXFER) { delay(1000); return true; }
    if (!wait_bits(regs, base + CMD, ACTIVE, 0)) { failed = true; return false; }
    uint32_t r = rr(regs, base + CMD);
    if (r & 0xf000) {
        printf("[usb] depcmd ep=%u cmd=%#x status=%#x\n", ep, cmd, r);
        failed = true; return false;
    }
    if ((cmd & 15) == STARTXFER) {
        resource[ep] = (r >> 16) & 127;
        transfer_active[ep] = true; /* resource index zero is valid (EP0 OUT) */
    }
    return true;
}
static bool start_xfer(unsigned ep, unsigned offset, unsigned len, unsigned type)
{
    static const unsigned trb_off[] = { DMA_TRB_OUT, DMA_TRB_IN, DMA_TRB_BULK_OUT, DMA_TRB_BULK_IN, 0x2200, 0x2280, 0x2300, 0x2380, 0x2400, 0x2480 };
    unsigned off = trb_off[ep];
    struct usb_trb *t = (void *)(dma + off);
    uint64_t pa = dma_pa + offset, tp = dma_pa + off;
    *t = (struct usb_trb){ (uint32_t)pa, (uint32_t)(pa >> 32), len,
        type | TRB_HWO | TRB_IOC | TRB_ISP | TRB_LST };
    dma_sync(dma + offset, len, true);
    dma_sync(t, sizeof(*t), true);
    return dep_cmd(ep, STARTXFER, tp >> 32, (uint32_t)tp, 0);
}
static void end_xfers(void)
{
    for (unsigned ep = 0; ep < 2; ++ep) {
        if (transfer_active[ep]) dep_cmd(ep, ENDXFER | BIT(11) | (resource[ep] << 16), 0, 0, 0);
        resource[ep] = 0;
        transfer_active[ep] = false;
    }
}
static bool ep0_config(bool modify)
{
    if (!modify && !dep_cmd(0, STARTCFG, 0, 0, 0)) return false;
    for (unsigned ep = 0; ep < 2; ++ep) {
        uint32_t p0 = (64U << 3) | (modify ? 2U << 30 : 0); /* EP0 shares TX FIFO0 */
        uint32_t p1 = (ep << 25) | BIT(8) | BIT(10);
        if (!dep_cmd(ep, SETEPCFG, p0, p1, 0)) return false;
        if (!modify && !dep_cmd(ep, XFERRES, 1, 0, 0)) return false;
    }
    rmw(regs, DALEPENA, 3, 3);
    return true;
}
static void arm_setup(void)
{
    phase = SETUP;
    setup_pending = false;
    start_xfer(0, DMA_SETUP, 8, TRB_SETUP);
}
static void stall(void)
{
    /* DWC3 clears the EP0 protocol stall on the next SETUP. */
    dep_cmd(0, SETSTALL, 0, 0, 0);
    arm_setup();
}
static unsigned descriptor(const struct usb_setup *s, const uint8_t **src)
{
    unsigned type = s->value >> 8, index = s->value & 255;
    if (type == 1 && !index) { *src = ncm_enabled ? ncm_device_desc : device_desc; return sizeof(device_desc); }
    if (type == 2 && !index) { *src = ncm_enabled ? ncm_config_desc : config_desc; return ncm_enabled ? sizeof(ncm_config_desc) : sizeof(config_desc); }
    if (type != 3) return 0; /* HS-only: STALL device qualifier/other-speed/BOS. */
    switch (index) {
    case 0: *src = lang_desc; return sizeof(lang_desc);
    case 1: *src = mfr_desc; return sizeof(mfr_desc);
    case 2: *src = prod_desc; return sizeof(prod_desc);
    case 3: *src = ncm_enabled ? udid_desc : serial_desc; return ncm_enabled ? sizeof(udid_desc) : sizeof(serial_desc);
    case 4: if (ncm_enabled) { *src = ncm_mac_desc; return sizeof(ncm_mac_desc); } return 0;
    default: return 0;
    }
}
static unsigned copy_log(uint8_t *dst)
{
    /* Bounded best-effort copy of the existing RAM head + tail ring. */
    unsigned len = *loader_live.len, at = *loader_live.ring_at;
    bool wrapped = *loader_live.wrapped;
    if (loader_live.head != LOG_HEAD || loader_live.ring != LOG_RING || at >= LOG_RING) return 0;
    if (wrapped) {
        memcpy(dst, loader_live.log, LOG_HEAD);
        memcpy(dst + LOG_HEAD, loader_live.log + LOG_HEAD + at, LOG_RING - at);
        memcpy(dst + LOG_HEAD + LOG_RING - at, loader_live.log + LOG_HEAD, at);
        return LOG_HEAD + LOG_RING;
    }
    if (len > LOG_HEAD + LOG_RING) len = LOG_HEAD + LOG_RING;
    memcpy(dst, loader_live.log, len);
    return len;
}
static void snapshot_log(void)
{
    snapshot_len = copy_log(log_snapshot);
    snapshot_heartbeat = loader_live.heartbeat;
    ++snapshot_id;
}
/* LOG1 command on OUT01 -> LOG1 24-byte metadata on IN81, then the
 * exact snapshot length in <=4096-byte transfers. One request at a time;
 * rearm OUT only after the final IN completes. EP0 shares the frozen snapshot
 * and refuses snapshot requests while bulk owns it. No unsolicited traffic. */
static void bulk_cancel(void)
{
    for (unsigned ep = 2; ep < 4; ++ep) {
        if (transfer_active[ep]) dep_cmd(ep, ENDXFER | BIT(11) | (resource[ep] << 16), 0, 0, 0);
        resource[ep] = 0; transfer_active[ep] = false;
    }
    bulk_busy = false;
}
static void bulk_stop(void)
{
    bulk_cancel();
    rmw(regs, DALEPENA, 12, 0);
    bulk_busy = false; bulk_halted[0] = bulk_halted[1] = false;
}
static void bulk_arm(void)
{
    if (configuration && !bulk_busy && !bulk_halted[0] && !bulk_halted[1] && !transfer_active[2])
        start_xfer(2, DMA_BULK_OUT, 512, 1U << 4); /* NORMAL, short command ends TRB */
}
static void ncm_link_update(void)
{
    bool up = configuration && ncm_alt && ncm_filter;
    lck_mtx_lock(&ncm_lock);
    ncm_queue_up = up && ncm_attached;
    if (!up) { ncm_flushed += ncm_count; ncm_head = ncm_count = 0; }
    lck_mtx_unlock(&ncm_lock);
    if (ncm_attached && up != ncm_link) {
        ncm_link = up; hakios_net_link_state_unit(2, up);
    }
}
static void ncm_end(unsigned ep)
{
    if (ep == 7 && transfer_active[ep]) ++ncm_cancelled;
    if (transfer_active[ep]) dep_cmd(ep, ENDXFER | BIT(11) | (resource[ep] << 16), 0, 0, 0);
    transfer_active[ep] = false; resource[ep] = 0;
}
static void mux_wake(void)
{
    wakeup(&mux_out_len); wakeup(&mux_in_len);
    lck_mtx_lock(&mux_lock); selwakeup(&mux_select_info); lck_mtx_unlock(&mux_lock); /* kqueue knotes attach under mux_lock */
}
/* Any mux halt change, SET_INTERFACE or deconfiguration ends the host session:
 * both transfers end, owned bytes are discarded and the daemon resets. */
static void mux_drop(void)
{
    ncm_end(8); ncm_end(9);
    lck_mtx_lock(&mux_lock);
    mux_out_len = mux_out_at = mux_in_len = 0; mux_in_zlp = false; mux_reset = true;
    lck_mtx_unlock(&mux_lock);
    mux_wake();
}
static void mux_poll(void)
{
    if (!mux_up) return;
    lck_mtx_lock(&mux_lock);
    bool out = !mux_out_len && !transfer_active[8] && !ncm_halted[8];
    unsigned in = transfer_active[9] || ncm_halted[9] ? 0 : mux_in_len;
    lck_mtx_unlock(&mux_lock);
    if (out) start_xfer(8, DMA_MUX_OUT, MUX_CHUNK, 1U << 4);
    if (in) start_xfer(9, DMA_MUX_IN, in, 1U << 4);
}
/* OUT chunks form a byte stream (the daemon splits packets by their length).
 * Each IN packet is one transfer, so a 512-multiple is ended by a ZLP. */
static void mux_event(unsigned ep, unsigned status)
{
    struct usb_trb *t = (void *)(dma + (ep == 8 ? 0x2400 : 0x2480));
    dma_sync(t, sizeof(*t), false);
    transfer_active[ep] = false; resource[ep] = 0;
    unsigned remain = t->size & 0xffffff;
    if ((status & 1) || (t->size >> 28) || remain > MUX_CHUNK || (ep == 9 && remain)) {
        printf("[usb-mux] DMA error ep=%u status=%#x trb=%#x\n", ep, status, t->size);
        failed = true; return;
    }
    if (ep == 8) {
        unsigned received = MUX_CHUNK - remain;
        if (!received) return;
        dma_sync(dma + DMA_MUX_OUT, received, false);
        lck_mtx_lock(&mux_lock); mux_out_len = received; mux_out_at = 0; lck_mtx_unlock(&mux_lock);
        mux_rx += received;
    } else {
        lck_mtx_lock(&mux_lock);
        bool zlp = !mux_in_zlp && !(mux_in_len % 512);
        mux_in_zlp = zlp;
        if (!zlp) { mux_in_len = 0; ++mux_tx; }
        lck_mtx_unlock(&mux_lock);
        if (zlp) { start_xfer(9, DMA_MUX_IN, 0, 1U << 4); return; }
    }
    mux_wake();
}
/* Interface 3 and endpoints 04/84. usbmuxd clears both halts after opening
 * the interface and before its version packet. */
static bool mux_control(const struct usb_setup *s, unsigned *n)
{
    if (!ncm_enabled || !configuration || s->value) return false;
    unsigned ep = s->index == 4 ? 8 : s->index == 0x84 ? 9 : 0;
    if (s->index == 3 && s->type == 0x81 && !s->request && s->length == 2) { *n = 2; return true; }
    if (s->index == 3 && s->type == 0x81 && s->request == 10 && s->length == 1) { *n = 1; return true; }
    if (s->index == 3 && s->type == 1 && s->request == 11 && !s->length) {
        mux_drop(); ncm_halted[8] = ncm_halted[9] = false;
        printf("[usb-mux] interface 3 alt 0; host session reset\n");
        return dep_cmd(8, CLEARSTALL, 0, 0, 0) && dep_cmd(9, CLEARSTALL, 0, 0, 0);
    }
    if (!ep) return false;
    if (s->type == 0x82 && !s->request && s->length == 2) { dma[0] = ncm_halted[ep]; *n = 2; return true; }
    if (s->type != 2 || (s->request != 1 && s->request != 3) || s->length) return false;
    mux_drop(); ncm_halted[ep] = s->request == 3;
    printf("[usb-mux] %s halt ep=%#x; host session reset\n", ncm_halted[ep] ? "set" : "clear", s->index);
    return dep_cmd(ep, ncm_halted[ep] ? SETSTALL : CLEARSTALL, 0, 0, 0);
}
static void ncm_stop(void)
{
    if (!ncm_enabled) return;
    ncm_alt = false; ncm_filter = 0; ncm_notify = 0;
    bzero(ncm_halted, sizeof(ncm_halted));
    for (unsigned ep = 5; ep < 8; ++ep) ncm_end(ep);
    mux_up = false; mux_drop();
    rmw(regs, DALEPENA, 0x3e0, 0);
    ncm_input_size = NCM_NTB_MAX; ncm_sequence = 0;
    ncm_link_update();
}
static bool ncm_ep_config(unsigned ep)
{
    uint32_t p0 = (ep == 5 ? 3U << 1 | 16U << 3 : 2U << 1 | 512U << 3);
    if (ep & 1) p0 |= (ep >> 1) << 17;
    return dep_cmd(ep, SETEPCFG, p0, (ep << 25) | BIT(8) | (ep == 5 ? 8U << 16 : 0), 0);
}
static bool ncm_config(void)
{
    if (!ncm_enabled) return true;
    for (unsigned ep = 5; ep < 10; ++ep)
        if (!ncm_ep_config(ep) || !dep_cmd(ep, XFERRES, 1, 0, 0)) return false;
    rmw(regs, DALEPENA, 0x320, 0x320);
    mux_up = true;
    printf("[usb-ncm] configured control=1 data=2 endpoints=82/03/83; usbmux interface=3 endpoints=04/84 fifo4=%#x\n",
        rr(regs, 0x310));
    return true;
}
static bool ncm_set_alt(unsigned alt)
{
    ncm_alt = false;
    ncm_halted[6] = ncm_halted[7] = false;
    ncm_end(6); ncm_end(7);
    rmw(regs, DALEPENA, 0xc0, 0);
    ncm_link_update();
    if (alt) {
        if (!ncm_ep_config(6) || !ncm_ep_config(7)) return false;
        rmw(regs, DALEPENA, 0xc0, 0xc0);
        ncm_alt = true;
        start_xfer(6, DMA_NCM_OUT, NCM_NTB_MAX, 1U << 4);
    }
    ncm_notify = 1; ncm_link_update();
    printf("[usb-ncm] data alt=%u filter=%#x input=%u\n", alt, ncm_filter, ncm_input_size);
    return !failed;
}
static void ncm_poll(void)
{
    if (!ncm_enabled) return;
    if (!ncm_attached) {
        /* USB starts before BSD networking. Require lo0 and PID1: loopattach
         * alone precedes ether_family_init. Keep EP0 live while waiting. */
        ifnet_t lo = NULL;
        if (!ifnet_find_by_name("lo0", &lo)) {
            ifnet_release(lo);
            proc_t init = proc_find(1);
            if (!init) return;
            proc_rele(init);
            ncm_attached = hakios_net_attach(&ncm_net);
            ncm_link_update();
        }
    }
    if (!configuration) return;
    mux_poll();
    if (ncm_notify && !ncm_halted[5] && !transfer_active[5]) {
        uint8_t *p = dma + DMA_NCM_NOTIFY;
        bzero(p, 16); p[0] = 0xa1; ncm_put16(p + 4, 1);
        if (ncm_notify == 1) {
            p[1] = 0; ncm_put16(p + 2, ncm_alt ? 1 : 0);
            start_xfer(5, DMA_NCM_NOTIFY, 8, 1U << 4);
        } else {
            p[1] = 0x2a; ncm_put16(p + 6, 8);
            ncm_put32(p + 8, 100000000); ncm_put32(p + 12, 100000000);
            start_xfer(5, DMA_NCM_NOTIFY, 16, 1U << 4);
        }
        ncm_notify = ncm_notify == 1 && ncm_alt ? 2 : 0;
    }
    if (!ncm_alt || !ncm_filter || ncm_halted[7] || transfer_active[7]) return;
    unsigned bytes = 0;
    lck_mtx_lock(&ncm_lock);
    if (ncm_count) {
        bytes = ncm_encode(dma + DMA_NCM_IN, ncm_queue[ncm_head].data,
                           ncm_queue[ncm_head].len, ncm_sequence++);
        ncm_head = (ncm_head + 1) % NCM_QUEUE; --ncm_count; ++ncm_dequeued;
    }
    lck_mtx_unlock(&ncm_lock);
    if (bytes && start_xfer(7, DMA_NCM_IN, bytes, 1U << 4)) ++ncm_submitted;
}
/* Only four malformed OUT buffers per boot: enough to see host framing
 * without flooding the live-log transport. All dump lengths are DMA bounded. */
static void ncm_reject(unsigned reason, unsigned received, unsigned status, uint32_t trb)
{
    ++ncm_bad; ++ncm_bad_reason[reason];
    if (ncm_bad_dumps == 4) return;
    ++ncm_bad_dumps;
    char hex[64 * 3 + 1]; unsigned n = received < 64 ? received : 64;
    static const char digits[] = "0123456789abcdef";
    for (unsigned i = 0; i < n; ++i) {
        uint8_t v = dma[DMA_NCM_OUT + i];
        hex[3*i] = digits[v >> 4]; hex[3*i+1] = digits[v & 15]; hex[3*i+2] = ' ';
    }
    hex[3*n] = 0;
    printf("[usb-ncm] bad-ntb #%u reason=%s OUT=%u status=%#x trb=%#x first%u=%s\n",
        ncm_bad_dumps, ncm_error_name[reason], received, status, trb, n, hex);
}
static void ncm_event(unsigned ep, unsigned status)
{
    static const unsigned offsets[] = {0x2280, 0x2300, 0x2380};
    struct usb_trb *t = (void *)(dma + offsets[ep - 5]);
    dma_sync(t, sizeof(*t), false);
    transfer_active[ep] = false; resource[ep] = 0;
    unsigned remain = t->size & 0xffffff;
    unsigned received = remain <= NCM_NTB_MAX ? NCM_NTB_MAX - remain : 0;
    if (ep == 7) {
        ++ncm_completed;
        ncm_tx_status = status; ncm_tx_size = t->size; ncm_tx_control = t->control;
    }
    if (ep == 6) dma_sync(dma + DMA_NCM_OUT, received, false);
    if ((status & 1) || (t->size >> 28)) {
        if (ep == 6) ncm_reject(NCM_DMA, received, status, t->size);
        printf("[usb-ncm] DMA error ep=%u status=%#x trb=%#x\n", ep, status, t->size);
        failed = true; return;
    }
    if (ep == 7) {
        /* As for log IN, a completion with residue is not a sent NTB. */
        if (remain) {
            printf("[usb-ncm] IN incomplete remain=%u status=%#x trb=%#x\n", remain, status, t->size);
            failed = true; return;
        }
        ++ncm_tx; return;
    }
    if (ep != 6 || !ncm_alt) return;
    if (remain > NCM_NTB_MAX) ncm_reject(NCM_RESIDUE, 0, status, t->size);
    else if (!received) ++ncm_zlp; /* A standalone USB ZLP contains no NTB. */
    else {
        struct ncm_datagram frames[NCM_DATAGRAMS];
        struct ncm_decode_info info;
        int count = ncm_decode(dma + DMA_NCM_OUT, received, frames, &info);
        if (count < 0) ncm_reject((unsigned)-count, received, status, t->size);
        else {
            ncm_zero_block += info.zero_block; ncm_pad += info.pad;
            ncm_unaligned += info.unaligned; ncm_half_null += info.half_null;
            ncm_multi += info.blocks > 1;
            if (!ncm_rx_reported && count) {
                ncm_rx_reported = true;
                printf("[usb-ncm] first RX OUT=%u block=%u ndp=%u frames=%d first=%u/%u\n",
                    received, ncm_le16(dma + DMA_NCM_OUT + 8), ncm_le16(dma + DMA_NCM_OUT + 10),
                    count, frames[0].offset, frames[0].length);
            }
            if (ncm_attached && ncm_filter) for (int i = 0; i < count; ++i) {
                hakios_net_input_unit(2, dma + DMA_NCM_OUT + frames[i].offset, frames[i].length); ++ncm_rx;
            }
        }
    }
    start_xfer(6, DMA_NCM_OUT, NCM_NTB_MAX, 1U << 4);
}
/* Class requests address only NCM control interface1. Capabilities advertise
 * packet filter, not CRC/NTB32/max-datagram negotiation. */
static bool ncm_control(const struct usb_setup *s, unsigned *n)
{
    if (!ncm_enabled || !configuration || s->index != 1) return false;
    if (s->type == 0xa1 && s->request == 0x80 && !s->value) {
        static const uint8_t parameters[] = {
            28,0, 1,0, 0,0x40,0,0, 4,0, 0,0, 4,0, 0,0,
            0,0x40,0,0, 4,0, 0,0, 4,0, 16,0
        };
        memcpy(dma, parameters, sizeof(parameters)); *n = sizeof(parameters); return true;
    }
    if (s->type == 0xa1 && s->request == 0x83 && !s->value) { *n = 2; return true; }
    if (s->type == 0xa1 && s->request == 0x85 && !s->value) {
        ncm_put32(dma, ncm_input_size); *n = 4; return true;
    }
    if (s->type == 0x21 && s->request == 0x84 && !s->value && !s->length) return true;
    if (s->type == 0x21 && s->request == 0x43 && !s->length && !(s->value & ~31U)) {
        ncm_filter = s->value; ncm_link_update(); return true;
    }
    return false;
}
static bool bulk_config(unsigned value)
{
    bulk_stop(); ncm_stop(); configuration = 0;
    if (!value) return true;
    if (!dep_cmd(0, STARTCFG | (2U << 16), 0, 0, 0)) return false;
    for (unsigned ep = 2; ep < 4; ++ep) {
        uint32_t p0 = (2U << 1) | (512U << 3) | (ep == 3 ? 1U << 17 : 0);
        if (!dep_cmd(ep, SETEPCFG, p0, (ep << 25) | BIT(8), 0) ||
            !dep_cmd(ep, XFERRES, 1, 0, 0)) return false;
    }
    rmw(regs, DALEPENA, 12, 12);
    if (!ncm_config()) return false;
    configuration = 1; bulk_arm();
    printf("[usb] bulk ready OUT01/IN81 mps=512 chunk=4096 fifo1=%#x\n", rr(regs, 0x304));
    return !failed;
}
static void bulk_next(void)
{
    if (bulk_at == snapshot_len) {
        bulk_busy = false; bulk_arm(); return;
    }
    bulk_queued = snapshot_len - bulk_at;
    if (bulk_queued > BULK_SIZE) bulk_queued = BULK_SIZE;
    memcpy(dma + DMA_BULK_IN, log_snapshot + bulk_at, bulk_queued);
    start_xfer(3, DMA_BULK_IN, bulk_queued, 1U << 4);
}
static void bulk_event(unsigned ep, unsigned status)
{
    unsigned off = ep == 2 ? DMA_TRB_BULK_OUT : DMA_TRB_BULK_IN;
    struct usb_trb *t = (void *)(dma + off);
    dma_sync(t, sizeof(*t), false);
    transfer_active[ep] = false; resource[ep] = 0;
    if (!configuration) return;
    unsigned remain = t->size & 0xffffff;
    if ((status & BIT(0)) || (t->size >> 28)) {
        printf("[usb] bulk DMA error ep=%u status=%#x trb=%#x\n", ep, status, t->size);
        failed = true; return;
    }
    if (ep == 2) {
        dma_sync(dma + DMA_BULK_OUT, 512, false);
        if (remain != 508 || memcmp(dma + DMA_BULK_OUT, "LOG1", 4)) { bulk_arm(); return; }
        snapshot_log(); bulk_at = 0; bulk_busy = bulk_header = true;
        uint32_t meta[] = { 0x31474f4c, snapshot_id, snapshot_len, BULK_SIZE,
            (uint32_t)snapshot_heartbeat, (uint32_t)(snapshot_heartbeat >> 32) };
        memcpy(dma + DMA_BULK_IN, meta, sizeof(meta));
        start_xfer(3, DMA_BULK_IN, sizeof(meta), 1U << 4);
    } else {
        if (!bulk_busy) return;
        if (remain) { printf("[usb] bulk IN incomplete remain=%u\n", remain); failed = true; return; }
        if (bulk_header) bulk_header = false;
        else bulk_at += bulk_queued;
        bulk_next();
    }
}
static void setup_packet(void)
{
    struct usb_setup s;
    const uint8_t *src = NULL;
    unsigned n = 0;
    bool valid = false;
    dma_sync(dma + DMA_SETUP, 8, false);
    memcpy(&s, dma + DMA_SETUP, sizeof(s));
    bzero(dma, DATA_SIZE);
    control_out = !(s.type & 0x80) && s.length;
    if (++requests <= 24 || (ncm_enabled && (s.type & 0x60) == 0x20)) printf("[usb] setup %02x/%02x v=%04x i=%04x n=%u\n", s.type, s.request, s.value, s.index, s.length);
    if (ncm_enabled && configuration && s.type == 0x21 && s.request == 0x86 &&
        !s.value && s.index == 1 && (s.length == 4 || s.length == 8)) {
        ncm_out_length = s.length; three_stage = true; phase = DATA_OUT;
        start_xfer(0, 0, 64, TRB_DATA); return;
    }
    if (s.type == 0x80 && s.request == 6) {
        n = descriptor(&s, &src); valid = n != 0;
    } else if (s.type == 0 && s.request == 5 && !s.index && !s.length && s.value < 128) {
        /* Synopsys programs the address before queuing STATUS2 (Linux ep0). */
        rmw(regs, DCFG, 127U << 3, s.value << 3); valid = true;
    } else if (s.type == 0 && s.request == 9 && !s.index && !s.length && s.value <= 1) {
        valid = bulk_config(s.value);
        printf("[usb] configuration=%u\n", configuration);
    } else if (s.type == 0x80 && s.request == 8 && !s.index && !s.value && s.length == 1) {
        dma[0] = configuration; n = 1; valid = true;
    } else if ((s.type == 0x80 || (s.type == 0x81 && configuration && (!s.index || (ncm_enabled && s.index <= 2))) ||
        (s.type == 0x82 && (s.index == 0 || s.index == 0x80 ||
        (configuration && (s.index == 1 || s.index == 0x81 || (ncm_enabled && (s.index == 3 || s.index == 0x83 || s.index == 0x82))))))) && s.request == 0 && !s.value && s.length == 2) {
        n = 2; valid = true;
        if (s.type == 0x82 && (s.index & 15) == 1) dma[0] = bulk_halted[s.index >> 7];
        else if (s.type == 0x82 && ncm_enabled) dma[0] = ncm_halted[2 * (s.index & 15) + (s.index >> 7)];
    } else if (ncm_enabled && configuration && s.type == 2 && (s.request == 1 || s.request == 3) &&
        !s.value && !s.length && (s.index == 3 || s.index == 0x83 || s.index == 0x82)) {
        unsigned ep = 2 * (s.index & 15) + (s.index >> 7);
        ncm_end(ep); ncm_halted[ep] = s.request == 3;
        valid = dep_cmd(ep, ncm_halted[ep] ? SETSTALL : CLEARSTALL, 0, 0, 0);
        if (!ncm_halted[ep] && ep == 6 && ncm_alt) start_xfer(6, DMA_NCM_OUT, NCM_NTB_MAX, 1U << 4);
    } else if (s.type == 2 && (s.request == 1 || s.request == 3) && !s.value && !s.length &&
        configuration && (s.index == 1 || s.index == 0x81)) {
        unsigned dir = s.index >> 7, ep = 2 + dir;
        bulk_cancel();
        bulk_halted[dir] = s.request == 3;
        valid = dep_cmd(ep, bulk_halted[dir] ? SETSTALL : CLEARSTALL, 0, 0, 0);
        bulk_arm();
    } else if (ncm_enabled && configuration && s.type == 0x81 && s.request == 10 &&
        !s.value && (s.index == 1 || s.index == 2) && s.length == 1) {
        dma[0] = s.index == 2 && ncm_alt; n = 1; valid = true;
    } else if (ncm_enabled && configuration && s.type == 1 && s.request == 11 && !s.length &&
        ((s.index == 2 && s.value <= 1) || (s.index == 1 && !s.value))) {
        valid = s.index == 1 || ncm_set_alt(s.value);
    } else if (s.type == 0x81 && s.request == 10 && configuration && !s.value && !s.index && s.length == 1) {
        n = 1; valid = true;
    } else if (s.type == 1 && s.request == 11 && configuration && !s.value && !s.index && !s.length) {
        /* Reset only the log interface: NCM endpoints/resources remain live. */
        bulk_stop();
        valid = true;
        for (unsigned ep = 2; ep < 4; ++ep) {
            uint32_t p0 = (2U << 1) | (512U << 3) | (ep == 3 ? 1U << 17 : 0);
            valid &= dep_cmd(ep, SETEPCFG, p0, (ep << 25) | BIT(8), 0);
        }
        rmw(regs, DALEPENA, 12, 12); bulk_arm();
    } else if (s.type == 0xc0 && s.request == 0x5a && !bulk_busy && !s.value && !s.index && s.length == 24) {
        snapshot_log();
        uint32_t meta[] = { 0x31474f4c, snapshot_id, snapshot_len, 1024,
            (uint32_t)snapshot_heartbeat, (uint32_t)(snapshot_heartbeat >> 32) };
        memcpy(dma, meta, sizeof(meta)); n = sizeof(meta); valid = true;
    } else if (s.type == 0xc0 && s.request == 0x5b && !bulk_busy && s.length && s.length <= DATA_SIZE) {
        uint32_t off = ((uint32_t)s.index << 16) | s.value;
        if (off <= snapshot_len && s.length <= snapshot_len - off) {
            memcpy(dma, log_snapshot + off, s.length); n = s.length; valid = true;
        }
    }
    if (!valid) valid = ncm_control(&s, &n);
    if (!valid) valid = mux_control(&s, &n);
    if (!valid) { stall(); return; }
    if (n > s.length) n = s.length;
    if (src) memcpy(dma, src, n);
    three_stage = s.length != 0;
    data_len = n;
    need_zlp = n && !(n % 64) && n < s.length;
    if (three_stage) {
        if (!(s.type & 0x80)) { stall(); return; }
        phase = DATA;
        start_xfer(1, 0, n, TRB_DATA); /* Do not wait for XferNotReady(DATA). */
    } else phase = STATUS_WAIT;
}
static void event_one(uint32_t e)
{
    ++event_total; last_event = e;
    if (event_total <= 128) printf("[usb] event #%u raw=%#x pos=%u phase=%u active=%u/%u\n",
        event_total, e, event_pos, phase, transfer_active[0], transfer_active[1]);
    if (!(e & 1)) {
        unsigned ep = (e >> 1) & 31, type = (e >> 6) & 15, status = (e >> 12) & 15;
        if (ep > 1) {
            if (ep < 4 && type == 1) bulk_event(ep, status);
            else if (ncm_enabled && ep >= 5 && ep <= 7 && type == 1) ncm_event(ep, status);
            else if (ncm_enabled && (ep == 8 || ep == 9) && type == 1) mux_event(ep, status);
            return;
        }
        if (type == 1) { /* XferComplete */
            resource[ep] = 0;
            transfer_active[ep] = false;
            if (phase == SETUP && ep == 0) setup_packet();
            else if (phase == DATA_OUT && ep == 0) {
                struct usb_trb *t = (void *)(dma + DMA_TRB_OUT);
                dma_sync(t, sizeof(*t), false); dma_sync(dma, 64, false);
                unsigned size = ncm_le32(dma), remain = t->size & 0xffffff;
                if ((t->size >> 28) || remain != 64 - ncm_out_length || size < 2048 || size > NCM_NTB_MAX ||
                    (ncm_out_length == 8 && (!ncm_le16(dma + 4) || ncm_le16(dma + 6)))) { stall(); return; }
                ncm_input_size = size; phase = STATUS_WAIT;
                /* This control write has IN status, unlike descriptor reads. */
                printf("[usb-ncm] host NTB input=%u\n", size);
            }
            else if ((phase == DATA || phase == DATA_ZLP) && ep == 1) {
                struct usb_trb *t = (void *)(dma + DMA_TRB_IN);
                dma_sync(t, sizeof(*t), false);
                setup_pending = (t->size >> 28) == 2;
                if (setup_pending) {
                    printf("[usb] EP0 data aborted by new SETUP trb=%#x\n", t->size);
                    phase = STATUS_WAIT;
                } else if (need_zlp && phase == DATA) {
                    phase = DATA_ZLP; start_xfer(1, 0, 0, TRB_DATA);
                } else phase = STATUS_WAIT;
            } else if (phase == STATUS && ep == (three_stage && !control_out ? 0U : 1U)) arm_setup();
        } else if (type == 3 && (status & 3) == 1 &&
            (phase == DATA || phase == DATA_ZLP) && ep != 1) {
            printf("[usb] EP0 unexpected data direction event=%#x\n", e);
            end_xfers(); stall();
        } else if (type == 3 && (status & 3) == 2 && phase == STATUS_WAIT) {
            if (setup_pending) { end_xfers(); stall(); return; }
            if (ep == (three_stage && !control_out ? 0U : 1U)) {
                phase = STATUS;
                start_xfer(ep, 0, 0, three_stage ? TRB_STATUS3 : TRB_STATUS2);
            }
        }
        return;
    }
    switch ((e >> 8) & 15) {
    case 0: /* Disconnect */
        bulk_stop(); ncm_stop(); configuration = 0;
        if (phase != SETUP) { end_xfers(); stall(); }
        printf("[usb] disconnect\n"); break;
    case 1: /* Reset */
        bulk_stop(); ncm_stop(); configuration = 0;
        /* Preserve an already queued SETUP across reset (Linux ep0_reset_state). */
        if (phase != SETUP) { end_xfers(); stall(); }
        rmw(regs, DCFG, 127U << 3, 0); printf("[usb] reset\n"); break;
    case 2: /* ConnectDone: MODIFY EP0, never STARTCFG or re-arm live SETUP. */
        ep0_config(true); printf("[usb] connect speed=%u (0=HS)\n", rr(regs, DSTS) & 7); break;
    case 3: case 4: case 6: case 9:
        printf("[usb] link/wake/suspend/error event=%#x dsts=%#x dctl=%#x\n",
            e, rr(regs, DSTS), rr(regs, DCTL)); break;
    case 11:
        printf("[usb] event overflow\n"); failed = true; break;
    default: break;
    }
}
/* Only the USB worker reads controller MMIO. Check GCC before touching a
 * possibly gated controller; never repair an unexplained lost vote silently. */
static bool controller_powered(void)
{
    if (!gcc || !(rr(gcc, 0xf004) & BIT(31))) return false;
    static const unsigned clocks[] = { 0xf07c, 0xf010, 0xf080, 0xf018, 0xf01c, 0x8c010 };
    for (unsigned i = 0; i < sizeof(clocks) / sizeof(clocks[0]); ++i)
        if (rr(gcc, clocks[i]) & BIT(31)) return false;
    return true;
}
static void usb_diagnostic(void)
{
    if (ncm_enabled) printf("[usb-ncm] attached=%u alt=%u filter=%#x rx=%llu tx=%llu bad=%llu drop=%llu active=%u/%u/%u\n",
        ncm_attached, ncm_alt, ncm_filter, ncm_rx, ncm_tx, ncm_bad, ncm_drop,
        transfer_active[5], transfer_active[6], transfer_active[7]);
    if (ncm_enabled) {
        lck_mtx_lock(&ncm_lock);
        uint64_t queued = ncm_queued, drop = ncm_drop;
        unsigned pending = ncm_count;
        lck_mtx_unlock(&ncm_lock);
        uint32_t size = 0, control = 0;
        if (dma && transfer_active[7]) {
            struct usb_trb *t = (void *)(dma + 0x2380);
            dma_sync(t, sizeof(*t), false); size = t->size; control = t->control;
        }
        printf("[usb-ncm-tx] queued=%llu dequeued=%llu submitted=%llu completed=%llu pending=%u in-flight=%u drop=%llu flushed=%llu cancelled=%llu last-status=%#x last-trb=%#x/%#x live-trb=%#x/%#x\n",
            queued, ncm_dequeued, ncm_submitted, ncm_completed, pending, transfer_active[7],
            drop, ncm_flushed, ncm_cancelled, ncm_tx_status, ncm_tx_size, ncm_tx_control, size, control);
        hakios_net_usb_diagnostic();
        printf("[usb-ncm] rx-compat zero-block=%llu pad=%llu unaligned=%llu half-null=%llu multi=%llu zlp=%llu\n",
            ncm_zero_block, ncm_pad, ncm_unaligned, ncm_half_null, ncm_multi, ncm_zlp);
        for (unsigned i = 1; i < NCM_ERRORS; ++i) if (ncm_bad_reason[i])
            printf("[usb-ncm] reject %s=%llu\n", ncm_error_name[i], ncm_bad_reason[i]);
        printf("[usb-mux] up=%u rx=%llu tx=%llu out=%u/%u in=%u active=%u/%u halt=%u/%u\n",
            mux_up, mux_rx, mux_tx, mux_out_at, mux_out_len, mux_in_len,
            transfer_active[8], transfer_active[9], ncm_halted[8], ncm_halted[9]);
    }
    printf("[usb] diag events=%u last=%#x requests=%u phase=%u res=%u/%u failed=%u hb=%llu\n",
        event_total, last_event, requests, phase, resource[0], resource[1], failed, loader_live.heartbeat);
    if (!gcc) return;
    printf("[usb] gcc bcr=%#x gdsc=%#x cfg/core/iface/sleep/utmi/xo=%#x/%#x/%#x/%#x/%#x/%#x rcg=%#x/%#x\n",
        rr(gcc, 0xf000), rr(gcc, 0xf004), rr(gcc, 0xf07c), rr(gcc, 0xf010),
        rr(gcc, 0xf080), rr(gcc, 0xf018), rr(gcc, 0xf01c), rr(gcc, 0x8c010), rr(gcc, 0xf024), rr(gcc, 0xf03c));
    if (!regs || !controller_powered()) { printf("[usb] controller gated; skipping core MMIO\n"); return; }
    printf("[usb] ring adr=%08x%08x size=%#x pos=%u devten=%#x active=%u/%u\n",
        rr(regs, GEVNTADRHI), rr(regs, GEVNTADRLO), rr(regs, GEVNTSIZ), event_pos,
        rr(regs, DEVTEN), transfer_active[0], transfer_active[1]);
    printf("[usb] core dctl=%#x dsts=%#x dcfg=%#x gctl=%#x phy2=%#x scratch=%#x count=%#x cmd=%#x/%#x\n",
        rr(regs, DCTL), rr(regs, DSTS), rr(regs, DCFG), rr(regs, GCTL), rr(regs, GUSB2PHYCFG),
        rr(wrapper, QSCRATCH + 0x10), rr(regs, GEVNTCOUNT), rr(regs, DEP(0)+CMD), rr(regs, DEP(1)+CMD));
    if (configuration) {
        struct usb_trb *bo = (void *)(dma + DMA_TRB_BULK_OUT), *bi = (void *)(dma + DMA_TRB_BULK_IN);
        dma_sync(bo, sizeof(*bo), false); dma_sync(bi, sizeof(*bi), false);
        printf("[usb] bulk snapshot=%u busy=%u at=%u/%u active=%u/%u trb=%#x/%#x:%#x/%#x cmd=%#x/%#x\n",
            snapshot_id, bulk_busy, bulk_at, snapshot_len, transfer_active[2], transfer_active[3],
            bo->size, bo->control, bi->size, bi->control, rr(regs, DEP(2)+CMD), rr(regs, DEP(3)+CMD));
    }
    if (smmu && usb_smr_index >= 0) printf("[usb] SID0 smr[%d]=%#x s2cr=%#x\n",
        usb_smr_index, rr(smmu, 0x800 + 4 * usb_smr_index), rr(smmu, 0xc00 + 4 * usb_smr_index));
    if (dma) {
        struct usb_trb *out = (void *)(dma + DMA_TRB_OUT), *in = (void *)(dma + DMA_TRB_IN);
        dma_sync(out, sizeof(*out), false); dma_sync(in, sizeof(*in), false);
        printf("[usb] trb out size/ctrl=%#x/%#x in=%#x/%#x\n", out->size, out->control, in->size, in->control);
    }
    if (usb_cb) printf("[usb] smmu sctlr=%#x fsr=%#x far=%08x%08x fsynr=%#x\n",
        rr(usb_cb, 0), rr(usb_cb, 0x58), rr(usb_cb, 0x64), rr(usb_cb, 0x60), rr(usb_cb, 0x68));
}
static void poll_events(void)
{
    if (!controller_powered()) {
        printf("[usb] power/clock lost; stopping controller MMIO\n"); failed = true; return;
    }
    /* Stop if an external owner has reset/reprogrammed the controller. The
     * stage1b capture showed DCTL/DCF[G]/QSCRATCH reverting while GCC stayed on. */
    if (!(rr(regs, DCTL) & BIT(31)) || (rr(regs, DCFG) & 7) != 0 ||
        rr(regs, GEVNTSIZ) != (EVENT_SIZE | BIT(31)) ||
        rr(regs, GEVNTADRLO) != (uint32_t)event_pa || rr(regs, GEVNTADRHI) != event_pa >> 32) {
        printf("[usb] controller ownership lost: core/ring configuration changed externally\n");
        failed = true; return;
    }
    unsigned count = rr(regs, GEVNTCOUNT) & 0xfffc;
    if (count > EVENT_SIZE) { printf("[usb] bad event count=%u\n", count); failed = true; return; }
    while (count && !failed) {
        uint32_t e;
        dma_sync(events + event_pos, 4, false);
        memcpy(&e, events + event_pos, 4);
        event_pos = (event_pos + 4) % EVENT_SIZE;
        event_one(e);
        wr(regs, GEVNTCOUNT, 4); count -= 4;
    }
}
static void mux_node(void);
static void usb_thread(void *arg, wait_result_t result)
{
    (void)arg; (void)result;
    thread_set_thread_name(current_thread(), "umi-usb");
    printf("[usb] stage1 begin; default-off, HS-only, EP0+bulk log ABI1 bcdDevice=0101\n");
    if (!alloc_dma(&events, &event_pa, EVENT_SIZE) || !alloc_dma(&dma, &dma_pa, DMA_SIZE)) goto done;
    if (!alloc_dma(&tables, &tables_pa, TABLE_BYTES) || tables_pa >= (1ULL << 36) ||
        !map_buffer(event_pa, EVENT_SIZE) || !map_buffer(dma_pa, DMA_SIZE)) goto done;
    dma_sync(tables, TABLE_BYTES, true);
    if (!smmu_prepare(false)) goto done; /* loader USB still live on failure */
    if (!take_controller()) goto done;
    if (!platform_start()) goto done;
    /* Recheck after firmware Stop + controller reset, before publishing DMA. */
    if (!smmu_prepare(true)) goto done;
    wr(regs, DEVTEN, 0); wr(regs, DALEPENA, 0);
    wr(regs, GEVNTADRLO, (uint32_t)event_pa); wr(regs, GEVNTADRHI, event_pa >> 32);
    wr(regs, GEVNTSIZ, EVENT_SIZE | BIT(31)); /* poll, mask IRQ133 */
    unsigned pending = rr(regs, GEVNTCOUNT) & 0xfffc;
    if (pending) wr(regs, GEVNTCOUNT, pending);
    wr(regs, DCFG, 0); /* high speed, address zero, event buffer zero */
    if (!ep0_config(false)) goto done;
    arm_setup();
    if (failed) goto done;
    wr(regs, DEVTEN, BIT(0) | BIT(1) | BIT(2) | BIT(3) | BIT(4) | BIT(6) | BIT(9) | BIT(11));
    wr(regs, DCTL, BIT(31));
    if (!wait_bits(regs, DSTS, BIT(22), 0)) goto done;
    printf("[usb] handover ok event-pa=%#llx dma-pa=%#llx; waiting for enumeration\n", event_pa, dma_pa);
    unsigned ticks = 0;
    bool mux_made = false, mux_host = false;
    while (!failed) {
        poll_events();
        ncm_poll();
        if (ncm_attached && !mux_made) { mux_made = true; mux_node(); }
        if (mux_up != mux_host) hakios_usbmux_host(mux_host = mux_up);
        if (!(ticks++ % 1000)) usb_diagnostic();
        IOSleep(1);
    }
done:
    /* Do not free active DMA on a failed controller. The loader's independent
     * deadline resets the SoC; buffers remain owned for this boot. */
    usb_diagnostic();
    printf("[usb] worker stopped; RAM/HFS logger and loader deadline remain armed\n");
    thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}
/* /dev/hakios-usbmux for /redwood/bin/usbmux. read() returns OUT stream bytes,
 * or ENXIO once after the host session ended; write() sends exactly one mux
 * packet (1..16384 bytes) as one IN transfer, waiting while the last is owned. */
static int mux_open(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
    lck_mtx_lock(&mux_lock); mux_reset = false; lck_mtx_unlock(&mux_lock);
    return 0;
}
static int mux_close(__unused dev_t dev, __unused int flags, __unused int type, __unused struct proc *p)
{
    return 0;
}
static int mux_read(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
    int err = 0;
    lck_mtx_lock(&mux_lock);
    while (!err && !mux_reset && mux_out_at == mux_out_len)
        err = msleep(&mux_out_len, &mux_lock, PZERO | PCATCH, "usbmux-read", NULL);
    if (!err && mux_reset) { mux_reset = false; err = ENXIO; }
    else if (!err) {
        unsigned n = (unsigned)MIN((user_ssize_t)(mux_out_len - mux_out_at), uio_resid(uio));
        err = uiomove((const char *)dma + DMA_MUX_OUT + mux_out_at, (int)n, uio);
        if (!err && (mux_out_at += n) == mux_out_len) mux_out_len = mux_out_at = 0;
    }
    lck_mtx_unlock(&mux_lock);
    return err;
}
static int mux_write(__unused dev_t dev, struct uio *uio, __unused int ioflag)
{
    user_ssize_t len = uio_resid(uio);
    int err = 0;
    if (len <= 0 || len > MUX_CHUNK) return EMSGSIZE;
    lck_mtx_lock(&mux_lock);
    while (!err && mux_up && !mux_reset && mux_in_len)
        err = msleep(&mux_in_len, &mux_lock, PZERO | PCATCH, "usbmux-write", NULL);
    if (!err && (!mux_up || mux_reset)) err = ENXIO;
    if (!err && !(err = uiomove((const char *)dma + DMA_MUX_IN, (int)len, uio))) mux_in_len = (unsigned)len;
    lck_mtx_unlock(&mux_lock);
    return err;
}
/* lockdownd accepts a loopback peer only when TCP_PEER_PID is 0: a socket the
 * kernel made, as AppleUSBDeviceMux's are. The daemon marks each socket so
 * before connect(); so_update_last_owner_locked() keeps a zero last_pid. */
#define HAKIOS_USBMUX_KERNEL_PEER _IOW('u', 1, int) /* also in tools/usbmux.c */
static int mux_ioctl(__unused dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
    struct socket *so;
    int fd = *(int *)data, err;
    if (cmd != HAKIOS_USBMUX_KERNEL_PEER) return ENOTTY;
    if ((err = file_socket(fd, &so))) return err;
    socket_lock(so, 1);
    so->last_pid = 0; so->last_upid = 0;
    socket_unlock(so, 1);
    file_drop(fd);
    return 0;
}
static int mux_select(__unused dev_t dev, int which, void *wql, struct proc *p)
{
    lck_mtx_lock(&mux_lock);
    int ready = which != FREAD || mux_reset || mux_out_at < mux_out_len;
    if (!ready) selrecord(p, &mux_select_info, wql);
    lck_mtx_unlock(&mux_lock);
    return ready;
}
static const struct cdevsw mux_cdevsw = {
    .d_open = mux_open, .d_close = mux_close, .d_read = mux_read, .d_write = mux_write,
    .d_ioctl = mux_ioctl, .d_stop = eno_stop, .d_reset = eno_reset, .d_select = mux_select,
    .d_mmap = eno_mmap, .d_strategy = eno_strat, .d_type = 0
};
/* The daemon poll()s this node with its sockets. XNU's poll() is kqueue, and a
 * cdev without CDEVSW_SELECT_KQUEUE gets POLLNVAL at once: the daemon then sat
 * in read() and lockdownd's replies waited for the next host packet. */
static void mux_node(void)
{
    int major = cdevsw_add(-1, &mux_cdevsw);
    bool ok = major >= 0 && !cdevsw_setkqueueok(major, &mux_cdevsw, 0) &&
        devfs_make_node(makedev(major, 0), DEVFS_CHAR, UID_ROOT, GID_WHEEL, 0600, "hakios-usbmux");
    printf("[usb-mux] /dev/hakios-usbmux %s\n", ok ? "ready (kqueue)" : "cannot be made");
}
/* Independent of the USB worker: no VFS operation in the event polling loop.
 * Each slot is created exclusively on the named HFS data filesystem. We never
 * open a block device and never write the historical raw var_store slots.
 * Two slots alternate so a reset during one write leaves the other snapshot.
 */
static uint8_t disk_snapshot[LOG_HEAD + LOG_RING];
/* The slots keep the iocount vnode_open() gives them for the whole run, and a forced unmount
 * drains every iocount (vnode_drain, no timeout): the power-off path must close them first. */
enum { HFS_IDLE, HFS_OPEN, HFS_STOPPING, HFS_STOPPED };
static uint32_t hfs_state = HFS_IDLE;
struct disk_header { uint32_t magic, version, epoch, sequence, length, checksum; };
static bool log_volume(vnode_t vp)
{
    struct vfsstatfs *fs = vfs_statfs(vnode_mount(vp));
    return fs && !strcmp(fs->f_fstypename, "hfs") &&
        !strcmp(fs->f_mntfromname, "/dev/ufs_darwin_data");
}
static int log_write(vnode_t vp, void *data, unsigned len, off_t off, vfs_context_t ctx)
{
    int resid = 0;
    int err = vn_rdwr(UIO_WRITE, vp, data, len, off, UIO_SYSSPACE, IO_SYNC,
        vfs_context_ucred(ctx), &resid, vfs_context_proc(ctx));
    return err ? err : (resid ? EIO : 0);
}
static void usb_file_thread(void *arg, wait_result_t result)
{
    (void)arg; (void)result;
    thread_set_thread_name(current_thread(), "umi-usb-log");
    char volume[64] = {0};
    unsigned writable = 0, epoch = 0;
    PE_parse_boot_argn("ufs_write", &writable, sizeof(writable));
    PE_parse_boot_argn("var_hfs", volume, sizeof(volume));
    PE_parse_boot_argn("epoch", &epoch, sizeof(epoch));
    if (writable != 1 || strcmp(volume, "ufs_darwin_data")) {
        printf("[usb] HFS logger requires ufs_write=1 var_hfs=ufs_darwin_data\n"); goto done;
    }
    vfs_context_t ctx = vfs_context_kernel();
    vnode_t slots[2] = { NULL, NULL };
    /* init clears /var/tmp after mounting, so use persistent /var/db.
     * Wait for the real data mount; never create files on the boot ramdisk. */
    for (;;) {
        vnode_t dir = NULL;
        if (__atomic_load_n(&hfs_state, __ATOMIC_ACQUIRE) != HFS_IDLE) goto done;
        if (vfs_rootvnode() && !vnode_lookup("/private/var/db", 0, &dir, ctx)) {
            bool ready = vnode_isdir(dir) && log_volume(dir);
            vnode_put(dir);
            if (ready) break;
        }
        IOSleep(1000);
    }
    uint32_t idle = HFS_IDLE;
    if (!__atomic_compare_exchange_n(&hfs_state, &idle, HFS_OPEN, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) goto done;
    /* A standalone boot gets its epoch from the loader's firmware clock through
     * GenericARMPE. Daily USB debugging continues to use its supplied epoch. */
    extern bool loader_is_production(void);
    if (!epoch && loader_is_production()) {
        clock_sec_t secs;
        clock_usec_t usecs;
        clock_get_calendar_microtime(&secs, &usecs);
        epoch = (unsigned)secs;
    }
    for (unsigned i = 0; i < 2; ++i) {
        char path[128];
        snprintf(path, sizeof(path), "/private/var/db/hakios-usb-%u-%u.bin", epoch, i);
        int err = vnode_open(path, FWRITE | O_CREAT | O_EXCL | O_NOFOLLOW, 0600, 0, &slots[i], ctx);
        if (err) { printf("[usb] HFS open errno=%d path=%s\n", err, path); goto close; }
        if (!vnode_isreg(slots[i]) || !log_volume(slots[i])) {
            printf("[usb] HFS file volume/type mismatch; refusing write\n"); goto close;
        }
        printf("[usb] HFS log slot=%s\n", path);
    }
    for (unsigned seq = 1; ; ++seq) {
        unsigned len = copy_log(disk_snapshot);
        uint32_t hash = 2166136261U;
        for (unsigned i = 0; i < len; ++i) hash = (hash ^ disk_snapshot[i]) * 16777619U;
        struct disk_header hdr = { 0x32474f4c, 1, epoch, seq, len, hash };
        vnode_t vp = slots[seq & 1];
        int err = log_write(vp, &hdr, sizeof(hdr), 0, ctx);
        if (!err) err = log_write(vp, disk_snapshot, len, sizeof(hdr), ctx);
        if (!err) err = log_write(vp, &hdr, sizeof(hdr), sizeof(hdr) + len, ctx);
        if (!err) err = VNOP_FSYNC(vp, MNT_WAIT, ctx);
        if (err) { printf("[usb] HFS snapshot seq=%u errno=%d\n", seq, err); goto close; }
        if (seq == 1 || !(seq % 15)) printf("[usb] HFS snapshot committed seq=%u bytes=%u\n", seq, len);
        if (__atomic_load_n(&hfs_state, __ATOMIC_ACQUIRE) == HFS_STOPPING) {
            printf("[usb] HFS log closed for power-off after seq=%u\n", seq);
            break;
        }
        /* Keep the debug cadence; production limits flash writes to one snapshot/minute.
         * Shutdown still wakes this loop within 100 ms for a final snapshot. */
        unsigned ticks = loader_is_production() ? 600 : 20;
        for (unsigned t = 0; t < ticks && __atomic_load_n(&hfs_state, __ATOMIC_ACQUIRE) != HFS_STOPPING; ++t) IOSleep(100);
    }
close:
    for (unsigned i = 0; i < 2; ++i) if (slots[i]) vnode_close(slots[i], FWRITE | FWASWRITTEN, ctx);
    __atomic_store_n(&hfs_state, HFS_STOPPED, __ATOMIC_RELEASE);
done:
    thread_terminate(current_thread());
    thread_block(THREAD_CONTINUE_NULL);
}
/* reboot_kernel(), before sync and unmount: one last snapshot, then the slots closed */
void hakios_usb_log_stop(void)
{
    uint32_t state = HFS_IDLE;
    if (__atomic_compare_exchange_n(&hfs_state, &state, HFS_STOPPED, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return;
    state = HFS_OPEN;
    __atomic_compare_exchange_n(&hfs_state, &state, HFS_STOPPING, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
    while (__atomic_load_n(&hfs_state, __ATOMIC_ACQUIRE) != HFS_STOPPED) IOSleep(10);
}
static void hakios_usb_init(void)
{
    unsigned enabled = 0;
    thread_t thread;
    extern bool loader_is_production(void);
    if (loader_is_production()) {
        /* Reuse only the HFS ring snapshots. Production never takes USB from UEFI. */
        if (kernel_thread_start(usb_file_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
        return;
    }
    if (!PE_parse_boot_argn("usb", &enabled, sizeof(enabled)) || enabled != 1) return;
    unsigned network = 0;
    PE_parse_boot_argn("usb_ncm", &network, sizeof(network));
    ncm_enabled = network == 1;
    if (kernel_thread_start(usb_file_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
    if (kernel_thread_start(usb_thread, NULL, &thread) == KERN_SUCCESS) thread_deallocate(thread);
}
STARTUP(EARLY_BOOT, STARTUP_RANK_LAST, hakios_usb_init);
