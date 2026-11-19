#!/usr/bin/env python3
"""Host-only: compile the actual EP0 handler with fake DMA/endpoint commands."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
pinned = Path(__file__).resolve().parent / 'sources/hakios.usb.c'
source = (pinned if pinned.is_file() else root / 'drivers/umi/hakios.usb.c').read_text()
ntb_path = pinned.parent / 'umi_usb_ntb.h' if pinned.is_file() else root / 'overlays/xnu/files/bsd/dev/umi_usb_ntb.h'
ntb = ntb_path.read_text()
macros = source[source.index('#define BIT(n)'):source.index('/* Prefix of the existing')]
state = source[source.index('struct usb_trb {'):source.index('static uint32_t rr(')]
handlers = source[source.index('static const uint8_t device_desc[]'):source.index('static void usb_thread(')]
a, b = handlers.index('static bool dep_cmd('), handlers.index('static bool start_xfer(')
handlers = handlers[:a] + r'''
static unsigned starts, last_ep, last_type, last_len, cfg_starts, modifies, stalls, ended;
static bool dep_cmd(unsigned ep, unsigned cmd, uint32_t p0, uint32_t p1, uint32_t p2)
{
    (void)ep; (void)p1; (void)p2;
    if ((cmd & 15) == STARTCFG) ++cfg_starts;
    if ((cmd & 15) == SETEPCFG && (p0 >> 30) == 2) ++modifies;
    if ((cmd & 15) == SETSTALL) ++stalls;
    if ((cmd & 15) == ENDXFER) ended |= 1U << ep;
    if ((cmd & 15) == STARTXFER) {
        unsigned off[] = { DMA_TRB_OUT, DMA_TRB_IN, DMA_TRB_BULK_OUT, DMA_TRB_BULK_IN, 0x2200, 0x2280, 0x2300, 0x2380, 0x2400, 0x2480 };
        struct usb_trb *t = (void *)(dma + off[ep]);
        assert((t->control & (TRB_HWO | TRB_LST | TRB_IOC)) == (TRB_HWO | TRB_LST | TRB_IOC));
        ++starts; last_ep = ep; last_len = t->size; last_type = t->control & (63U << 4);
        resource[ep] = ep; transfer_active[ep] = true;
    }
    return true;
}
''' + handlers[b:]
# Actual address table builder is tested too, without SMMU/MMIO operations.
tables = source[source.index('#define TABLE_BYTES'):source.index('static bool take_controller(')]
harness = r'''
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include <assert.h>
struct {
    uint64_t magic, heartbeat;
    const char *log;
    const uint32_t *len, *ring_at;
    const bool *wrapped;
    uint64_t head, ring;
} loader_live;
''' + ntb + macros + r'''
typedef const uint8_t *mbuf_t;
typedef void *ifnet_t;
typedef void *proc_t;
static proc_t proc_find(int pid) { assert(pid==1); return (void *)1; }
static void proc_rele(proc_t p) { (void)p; }
#define LCK_GRP_DECLARE(n, s) int n
#define LCK_MTX_DECLARE(n, g) int n
static void lck_mtx_lock(int *lock) { (void)lock; }
static void lck_mtx_unlock(int *lock) { (void)lock; }
static int mbuf_copydata(mbuf_t p, unsigned off, size_t n, void *dst) { memcpy(dst, p+off, n); return 0; }
struct hakios_net_link { const char *name; unsigned unit; uint8_t address[6]; bool (*transmit)(mbuf_t,size_t); };
static void hakios_net_usb_diagnostic(void) {}
static unsigned net_received;
static bool net_up;
static int ifnet_find_by_name(const char *s, ifnet_t *p) { (void)s; *p = (void *)1; return 0; }
static void ifnet_release(ifnet_t p) { (void)p; }
static bool hakios_net_attach(const struct hakios_net_link *p) { assert(p->unit==2); return true; }
static void hakios_net_link_state_unit(unsigned u, bool up) { assert(u==2); net_up=up; }
static void hakios_net_input_unit(unsigned u,const uint8_t *p,size_t n) { assert(u==2 && n>=14 && p); ++net_received; }
struct selinfo { int unused; };
static unsigned mux_wakeups;
static void wakeup(void *p) { (void)p; ++mux_wakeups; }
static void selwakeup(struct selinfo *p) { (void)p; }
''' + state + r'''
static uint32_t rr(volatile uint32_t *r, unsigned off) { return r[off / 4]; }
static unsigned acknowledged, arrive_on_ack;
static void wr(volatile uint32_t *r, unsigned off, uint32_t v) {
    if (r == regs && off == GEVNTCOUNT) {
        assert(v <= r[off / 4]);
        r[off / 4] -= v; acknowledged += v;
        if (arrive_on_ack) { r[off / 4] += 4; arrive_on_ack = 0; }
    } else r[off / 4] = v;
}
static void rmw(volatile uint32_t *r, unsigned off, uint32_t m, uint32_t v) { wr(r, off, (rr(r, off) & ~m) | (v & m)); }
static void dma_sync(void *p, unsigned n, bool write) { (void)p; (void)n; (void)write; }
static uint32_t fake_smmu[0x100000 / 4];
static int hakios_smmu_lock;
static uintptr_t ml_io_map(uint64_t pa, unsigned size) { (void)pa; (void)size; return (uintptr_t)fake_smmu; }
static bool wait_bits(volatile uint32_t *r, unsigned off, uint32_t mask, uint32_t value) { return (rr(r, off) & mask) == value; }
''' + tables + handlers + r'''
static void setup(uint8_t type, uint8_t req, uint16_t value, uint16_t index, uint16_t len)
{
    struct usb_setup s = { type, req, value, index, len };
    memcpy(dma + DMA_SETUP, &s, sizeof(s));
    phase = SETUP;
    event_one(1U << 6); /* SETUP complete, physical EP0 */
}
static void status_ready(unsigned ep) { event_one((ep << 1) | (3U << 6) | (2U << 12)); }
static void complete(unsigned ep) {
    unsigned off[] = { DMA_TRB_OUT, DMA_TRB_IN, DMA_TRB_BULK_OUT, DMA_TRB_BULK_IN, 0x2200, 0x2280, 0x2300, 0x2380, 0x2400, 0x2480 };
    struct usb_trb *t = (void *)(dma + off[ep]);
    t->size &= 0xff000000; t->control &= ~TRB_HWO;
    event_one((ep << 1) | (1U << 6));
}
/* usbmux interface 3: session-start halts, OUT ownership until consumed, one IN
 * transfer per packet with a ZLP after 512-multiples, residue, SET_INTERFACE. */
static void mux_test(void)
{
    struct usb_trb *o = (void *)(dma + 0x2400), *in = (void *)(dma + 0x2480);
    assert(mux_up && (rr(regs, DALEPENA) & 0x300) == 0x300);
    ncm_poll(); assert(transfer_active[8] && ((struct usb_trb *)(dma + 0x2400))->size == MUX_CHUNK);
    mux_reset = false; ended = 0;
    setup(2, 1, 0, 4, 0); status_ready(1); complete(1);
    assert(mux_reset && (ended & BIT(8)) && !transfer_active[8] && !ncm_halted[8]);
    setup(2, 3, 0, 0x84, 0); status_ready(1); complete(1); assert(ncm_halted[9]);
    setup(0x82, 0, 0, 0x84, 2); assert(last_len == 2 && dma[0] == 1); complete(1); status_ready(0); complete(0);
    setup(2, 1, 0, 0x84, 0); status_ready(1); complete(1); assert(!ncm_halted[9]);
    setup(0x81, 10, 0, 3, 1); assert(last_len == 1 && dma[0] == 0); complete(1); status_ready(0); complete(0);
    setup(0x81, 0, 0, 3, 2); assert(last_len == 2); complete(1); status_ready(0); complete(0);
    mux_reset = false;
    ncm_poll(); assert(transfer_active[8]);
    memcpy(dma + DMA_MUX_OUT, "\0\0\0\0\0\0\0\x14\0\0\0\2\0\0\0\x08\0\0\0\0", 20);
    o->size = MUX_CHUNK - 20; o->control &= ~TRB_HWO;
    unsigned woken = mux_wakeups;
    event_one((8U << 1) | (1U << 6));
    assert(mux_out_len == 20 && !mux_out_at && mux_rx == 20 && !transfer_active[8] && mux_wakeups > woken);
    unsigned before = starts; ncm_poll(); assert(starts == before); /* owned until read */
    mux_out_at = mux_out_len = 0; /* read() consumed all 20 bytes */
    ncm_poll(); assert(transfer_active[8] && last_ep == 8);
    o->size = MUX_CHUNK; o->control &= ~TRB_HWO; event_one((8U << 1) | (1U << 6));
    assert(!mux_out_len && !transfer_active[8]); ncm_poll(); assert(transfer_active[8]); /* ZLP: rearm */
    uint64_t tx = mux_tx;
    mux_in_len = 28; ncm_poll(); assert(transfer_active[9] && last_ep == 9 && last_len == 28);
    before = starts; ncm_poll(); assert(starts == before);
    complete(9); assert(!mux_in_len && !transfer_active[9] && mux_tx == tx + 1);
    mux_in_len = 1024; ncm_poll(); assert(last_ep == 9 && last_len == 1024);
    complete(9); assert(mux_in_len == 1024 && mux_in_zlp && transfer_active[9] && last_len == 0);
    complete(9); assert(!mux_in_len && !mux_in_zlp && mux_tx == tx + 2);
    mux_in_len = 30; ncm_poll(); in->size = 2; in->control &= ~TRB_HWO;
    event_one((9U << 1) | (1U << 6)); assert(failed && mux_tx == tx + 2);
    failed = false; mux_in_len = 0;
    mux_reset = false;
    setup(1, 11, 0, 3, 0); status_ready(1); complete(1);
    assert(mux_reset && !mux_out_len && !transfer_active[8] && !failed);
    puts("PASS: usbmux interface 3: halts reset the session, OUT owned until read, ZLP rearm, IN one transfer per packet + ZLP after 512-multiple, IN residue is fatal, SET_INTERFACE resets");
}
static void tx_queue_test(void)
{
    uint8_t frame[NCM_FRAME_MAX];
    struct ncm_datagram d[NCM_DATAGRAMS]; struct ncm_decode_info info;
    uint64_t q = ncm_queued, dq = ncm_dequeued, s = ncm_submitted, c = ncm_completed;
    uint64_t drop = ncm_drop, tx = ncm_tx;
    /* Eight full rings, delayed completion, idle restart, and exact USB packet
     * boundaries. Every frame must reach IN once, in order, byte-for-byte. */
    for (unsigned batch = 0; batch < 8; ++batch) {
        for (unsigned i = 0; i < NCM_QUEUE; ++i) {
            memset(frame, (uint8_t)(batch * NCM_QUEUE + i), sizeof(frame));
            assert(ncm_transmit(frame, 484 + (i % 3) * 512));
        }
        assert(!ncm_transmit(frame, sizeof(frame))); /* full ring */
        for (unsigned i = 0; i < NCM_QUEUE; ++i) {
            ncm_poll(); assert(transfer_active[7] && last_ep == 7);
            unsigned bytes = last_len;
            assert(bytes == 513 + (i % 3) * 512);
            assert(ncm_decode(dma + DMA_NCM_IN, bytes, d, &info) == 1);
            assert(d[0].length == 484 + (i % 3) * 512);
            for (unsigned j = 0; j < d[0].length; ++j)
                assert(dma[DMA_NCM_IN + d[0].offset + j] == (uint8_t)(batch * NCM_QUEUE + i));
            unsigned before = starts, pending = ncm_count;
            for (unsigned j = 0; j < 10; ++j) ncm_poll();
            assert(starts == before && ncm_count == pending); /* no overwrite while owned */
            complete(7); assert(!transfer_active[7] && !resource[7]);
        }
        unsigned before = starts;
        for (unsigned j = 0; j < 10; ++j) ncm_poll();
        assert(starts == before && !ncm_count); /* idle, then next batch restarts */
    }
    assert(ncm_queued == q + 512 && ncm_dequeued == dq + 512);
    assert(ncm_submitted == s + 512 && ncm_completed == c + 512 && ncm_tx == tx + 512);
    assert(ncm_drop == drop + 8 && ncm_tx_size == 0 && !(ncm_tx_control & TRB_HWO));
    /* A real XferComplete with residue must not count as a successful NTB. */
    assert(ncm_transmit(frame, 42)); ncm_poll();
    struct usb_trb *t = (void *)(dma + 0x2380);
    t->size = 1; t->control &= ~TRB_HWO;
    event_one((7U << 1) | (1U << 6));
    assert(failed && ncm_tx == tx + 512 && ncm_completed == c + 513 && ncm_tx_size == 1);
    failed = false;
    /* Cancel one active IN and flush one queued frame on alt0, restart on alt1. */
    assert(ncm_transmit(frame, 42)); ncm_poll();
    assert(ncm_transmit(frame, 42));
    uint64_t cancelled = ncm_cancelled, flushed = ncm_flushed;
    assert(ncm_set_alt(0));
    assert(!ncm_count && !transfer_active[7] && ncm_cancelled == cancelled + 1 && ncm_flushed == flushed + 1);
    assert(ncm_set_alt(1)); assert(ncm_transmit(frame, 42)); ncm_poll();
    complete(7); assert(ncm_tx == tx + 513 && !failed);
    puts("PASS: actual TX queue/IN path: 512 ordered frames, 8 ring wraps, full queue, delayed completion, idle restart, 512/1024/1536 boundaries, residue rejection, alt0 cancel/flush and alt1 restart");
}
int main(void)
{
    static uint8_t memory[DMA_SIZE], table_memory[TABLE_BYTES];
    static uint32_t registers[1024];
    dma = memory; regs = registers; tables = table_memory; tables_pa = 0x100000000ULL;
    /* Identity mapping across 2 MiB and 1 GiB boundaries: no broad blocks. */
    assert(map_buffer(0x13ffff000ULL, 16384));
    assert(map_buffer(0x180000000ULL, 4096));
    assert(!map_buffer(0x180000001ULL, 4096));
    for (uint64_t pa = 0x13ffff000ULL; pa < 0x140003000ULL; pa += 4096) {
        uint64_t *l1 = (uint64_t *)tables;
        uint64_t *l2 = next_table(&l1[(pa >> 30) & 511]);
        uint64_t *l3 = next_table(&l2[(pa >> 21) & 511]);
        assert((l3[(pa >> 12) & 511] & PTE_ADDR) == pa);
        assert((l3[(pa >> 12) & 511] & 3) == 3);
    }
    assert(table_count <= 8);
    /* Exact SID0 has a new CB; SID0x20 keeps the UEFI shared CB5. */
    wr(fake_smmu, 0, 0x250406); wr(fake_smmu, 0x20, 0x4c017e57); wr(fake_smmu, 0x24, 0x6000004a);
    for (unsigned i = 0; i < 7; ++i) {
        wr(fake_smmu, 0x800 + 4*i, BIT(31) | (i < 5 ? 0x100 + i : (i == 5 ? 0 : 0x20)));
        wr(fake_smmu, 0xc00 + 4*i, i < 5 ? i : 5);
    }
    wr(fake_smmu, 0x85000, 1); wr(fake_smmu, 0x1014, 0x1f300);
    assert(smmu_prepare(false)); assert(rr(fake_smmu, 0xc14) == 5);
    assert(smmu_prepare(true)); assert(rr(fake_smmu, 0xc14) == 6);
    assert(rr(fake_smmu, 0xc18) == 5 && rr(fake_smmu, 0x85000) == 1);
    assert(rr(fake_smmu, 0x86020) == (uint32_t)tables_pa);
    /* Wildcard ownership must not be stolen, even when SID0 matches. */
    wr(fake_smmu, 0x814, BIT(31) | BIT(16));
    assert(!smmu_prepare(true)); assert(rr(fake_smmu, 0xc18) == 5);
    /* An explicit bypass stream needs no context writes. */
    wr(fake_smmu, 0x814, BIT(31)); wr(fake_smmu, 0xc14, 1U << 16);
    assert(smmu_prepare(true)); assert(rr(fake_smmu, 0xc14) == 1U << 16);
    setup(0x80, 6, 0x100, 0, 64);
    assert(phase == DATA && last_type == TRB_DATA && last_len == 18 && last_ep == 1);
    assert(memory[10] == 0 && memory[11] == 0x10); /* actual PID=1000 */
    complete(1); assert(phase == STATUS_WAIT);
    status_ready(0); assert(phase == STATUS && last_type == TRB_STATUS3);
    complete(0); assert(phase == SETUP && last_type == TRB_SETUP);
    /* Exact configuration requests from the Mac trace: header then complete
     * 9-byte configuration + 9-byte interface + two 7-byte endpoints, then a fresh SETUP. */
    for (unsigned n = 9; n <= 32; n += 23) {
        setup(0x80, 6, 0x200, 0, n);
        assert(last_len == n && memory[0] == 9 && memory[1] == 2 && memory[2] == 32);
        if (n == 32) {
            assert(memory[9] == 9 && memory[10] == 4 && memory[13] == 2);
            assert(memory[20] == 1 && memory[27] == 0x81 && memory[23] == 2 && memory[30] == 2);
        }
        complete(1); status_ready(0); complete(0);
        assert(phase == SETUP && transfer_active[0] && resource[0] == 0);
    }
    /* SET_ADDRESS including zero is applied, and uses STATUS2. */
    setup(0, 5, 17, 0, 0); assert((rr(regs, DCFG) & (127U << 3)) == 17U << 3);
    status_ready(1); assert(last_type == TRB_STATUS2);
    complete(1); assert(phase == SETUP);
    setup(0, 5, 0, 0, 0); assert(!(rr(regs, DCFG) & (127U << 3)));
    status_ready(1); complete(1);
    setup(0, 9, 1, 0, 0); assert(configuration == 1);
    status_ready(1); complete(1);
    setup(0x80, 8, 0, 0, 1); assert(last_len == 1 && memory[0] == 1);
    complete(1); status_ready(0); complete(0);
    /* A host abort during IN data must restart SETUP on the STATUS event. */
    setup(0x80, 6, 0x100, 0, 18);
    ((struct usb_trb *)(dma + DMA_TRB_IN))->size = 2U << 28;
    complete(1); assert(setup_pending && phase == STATUS_WAIT);
    unsigned abort_stalls = stalls;
    status_ready(0); assert(stalls == abort_stalls + 1 && phase == SETUP && !setup_pending);
    ((struct usb_trb *)(dma + DMA_TRB_IN))->size = 0;
    setup(0x80, 6, 0x100, 0, 18);
    event_one((3U << 6) | (1U << 12));
    assert(phase == SETUP && stalls == abort_stalls + 2);
    unsigned old_stalls = stalls;
    setup(0x80, 6, 0x600, 0, 10); assert(stalls == old_stalls + 1 && phase == SETUP);
    old_stalls = stalls;
    setup(0x40, 0xff, 0, 0, 64); assert(stalls == old_stalls + 1 && phase == SETUP);
    unsigned old_starts = starts;
    event_one(1 | (2U << 8)); /* ConnectDone must not arm SETUP twice */
    assert(starts == old_starts && modifies == 2 && cfg_starts == 1);
    event_one(1 | (1U << 8)); assert(configuration == 0 && phase == SETUP);
    static char log_memory[LOG_HEAD + LOG_RING];
    memset(log_memory, 'H', LOG_HEAD); memset(log_memory + LOG_HEAD, 'R', LOG_RING);
    log_memory[LOG_HEAD + 9] = 'A'; log_memory[LOG_HEAD + 8] = 'Z';
    uint32_t log_len = 27, at = 9; bool wrapped = false;
    loader_live.log = log_memory; loader_live.len = &log_len; loader_live.ring_at = &at;
    loader_live.wrapped = &wrapped; loader_live.head = LOG_HEAD; loader_live.ring = LOG_RING;
    setup(0xc0, 0x5a, 0, 0, 24); assert(snapshot_len == 27 && last_len == 24);
    complete(1); status_ready(0); complete(0);
    old_stalls = stalls;
    setup(0xc0, 0x5b, 28, 0, 1); assert(stalls == old_stalls + 1);
    wrapped = true; snapshot_log();
    assert(snapshot_len == LOG_HEAD + LOG_RING && log_snapshot[LOG_HEAD] == 'A');
    assert(log_snapshot[snapshot_len - 1] == 'Z');
    setup(0xc0, 0x5b, 0, 0, 1024); assert(last_len == 1024 && !need_zlp);
    complete(1); status_ready(0); assert(last_type == TRB_STATUS3);
    complete(0);
    assert(transfer_active[0] && resource[0] == 0);
    ended = 0; end_xfers();
    assert((ended & 1) && !transfer_active[0]); /* index zero MUST get ENDXFER */
    /* Exercise actual NORMAL TRB buffers and bulk snapshot framing, including
     * a full 4096-byte packet multiple, final short chunk, and host retry. */
    wrapped = false; log_len = 5000;
    setup(0, 9, 1, 0, 0); status_ready(1); complete(1);
    assert(configuration && transfer_active[2] && rr(regs, DALEPENA) == 15);
    struct usb_trb *bo = (void *)(dma + DMA_TRB_BULK_OUT);
    struct usb_trb *bi = (void *)(dma + DMA_TRB_BULK_IN);
    memcpy(dma + DMA_BULK_OUT, "LOG1", 4); bo->size = 508;
    event_one((2U << 1) | (1U << 6));
    assert(bulk_busy && last_ep == 3 && last_len == 24 && !transfer_active[2]);
    uint32_t *meta = (void *)(dma + DMA_BULK_IN);
    assert(meta[0] == 0x31474f4c && meta[2] == 5000 && meta[3] == 4096);
    unsigned busy_stalls = stalls;
    setup(0xc0, 0x5a, 0, 0, 24); assert(stalls == busy_stalls + 1 && bulk_busy);
    complete(3); assert(last_len == 4096 && !memcmp(dma + DMA_BULK_IN, log_memory, 4096));
    complete(3); assert(last_len == 904);
    complete(3); assert(!bulk_busy && transfer_active[2] && !transfer_active[3]);
    assert((bi->lo == dma_pa + DMA_BULK_IN) && bo->lo == dma_pa + DMA_BULK_OUT);
    /* Malformed command is ignored without reading outside the OUT buffer. */
    bo->size = 512; event_one((2U << 1) | (1U << 6)); assert(!bulk_busy && transfer_active[2]);
    memcpy(dma + DMA_BULK_OUT, "LOG1", 4); bo->size = 508;
    event_one((2U << 1) | (1U << 6)); assert(bulk_busy);
    ended = 0;
    setup(1, 11, 0, 0, 0); status_ready(1); complete(1);
    assert((ended & 8) && !bulk_busy && transfer_active[2] && !transfer_active[3]);
    setup(2, 3, 0, 0x81, 0); status_ready(1); complete(1);
    assert(bulk_halted[1] && !transfer_active[2]);
    setup(0x82, 0, 0, 0x81, 2); assert(memory[0] == 1);
    complete(1); status_ready(0); complete(0);
    setup(2, 1, 0, 0x81, 0); status_ready(1); complete(1);
    assert(!bulk_halted[1] && transfer_active[2]);
    event_one(1 | (1U << 8)); assert(!configuration && !transfer_active[2] && !transfer_active[3]);
    assert(rr(regs, DALEPENA) == 3);
    /* NCM descriptor, negotiation and simultaneous log/network endpoints. */
    ncm_enabled = true;
    setup(0x80,6,0x100,0,18);
    assert(last_len==18 && ncm_le16(memory+8)==0x05ac && ncm_le16(memory+10)==0x12c0 && memory[16]==3);
    complete(1); status_ready(0); complete(0);
    setup(0x80,6,0x303,0x409,255); assert(last_len==82 && memory[1]==3);
    for (unsigned k=0;k<40;++k) assert(memory[2+2*k]=="27da00a11185304aa7aebac7de8154212fb67796"[k] && !memory[3+2*k]);
    complete(1); status_ready(0); complete(0);
    setup(0x80,6,0x200,0,255);
    assert(last_len==140 && ncm_le16(memory+2)==140 && memory[4]==4);
    unsigned desc_at=0; while(desc_at<140) { assert(memory[desc_at]>=2); desc_at+=memory[desc_at]; } assert(desc_at==140);
    assert(!memcmp(memory+117,"\x09\x04\x03\x00\x02\xff\xfe\x02\x00\x07\x05\x04\x02\x00\x02\x00\x07\x05\x84\x02\x00\x02\x00",23));
    complete(1); status_ready(0); complete(0);
    setup(0,9,1,0,0); status_ready(1); complete(1);
    assert(configuration && transfer_active[2] && (rr(regs,DALEPENA)&0x20));
    setup(0xa1,0x80,0,1,28); assert(last_len==28 && ncm_le32(memory+4)==16384 && ncm_le16(memory+26)==16);
    complete(1);status_ready(0);complete(0);
    setup(0x21,0x86,0,1,4); assert(phase==DATA_OUT && last_ep==0 && last_len==64);
    ncm_put32(memory,4096); ((struct usb_trb *)(memory+DMA_TRB_OUT))->size=60;
    event_one(1U<<6); assert(phase==STATUS_WAIT && ncm_input_size==4096);
    status_ready(1); assert(last_type==TRB_STATUS3); complete(1); assert(phase==SETUP);
    setup(0x21,0x86,0,1,4); ncm_put32(memory,1000); ((struct usb_trb *)(memory+DMA_TRB_OUT))->size=60;
    old_stalls=stalls; event_one(1U<<6); assert(stalls==old_stalls+1 && ncm_input_size==4096);
    setup(1,11,1,2,0);status_ready(1);complete(1);
    ncm_poll(); assert(ncm_attached && ncm_alt && transfer_active[5] && transfer_active[6]);
    complete(5);ncm_poll(); assert(transfer_active[5] && last_len==16);complete(5);
    setup(0x21,0x43,0x0f,1,0);status_ready(1);complete(1); assert(net_up);
    uint8_t frame[512];memset(frame,0x34,sizeof(frame));
    assert(ncm_transmit(frame,sizeof(frame)));ncm_poll();assert(transfer_active[7]);
    unsigned ntb_size=last_len; assert(ntb_size==540);complete(7);assert(ncm_tx==1);
    memcpy(memory+DMA_NCM_OUT,memory+DMA_NCM_IN,ntb_size);
    ((struct usb_trb *)(memory+0x2300))->size=NCM_NTB_MAX-ntb_size;
    event_one((6U<<1)|(1U<<6));assert(net_received==1 && transfer_active[6]);

    /* Zero block + unaligned Ethernet offset through the actual OUT completion. */
    uint8_t *rx=memory+DMA_NCM_OUT;
    memmove(rx+30,rx+28,512);ncm_put16(rx+8,0);ncm_put16(rx+20,30);
    ((struct usb_trb *)(memory+0x2300))->size=NCM_NTB_MAX-(ntb_size+2);
    event_one((6U<<1)|(1U<<6));assert(net_received==2 && ncm_zero_block==1 && ncm_unaligned==1);
    /* Malformed input is diagnosed/rearmed, never delivered; only first four dumps. */
    for(unsigned k=0;k<6;++k) {
        memset(rx,0,80);((struct usb_trb *)(memory+0x2300))->size=NCM_NTB_MAX-80;
        event_one((6U<<1)|(1U<<6));
        assert(net_received==2 && transfer_active[6] && !failed);
    }
    assert(ncm_bad==6 && ncm_bad_reason[NCM_NTH_SIGNATURE]==6 && ncm_bad_dumps==4);
    ((struct usb_trb *)(memory+0x2300))->size=NCM_NTB_MAX;
    event_one((6U<<1)|(1U<<6));assert(ncm_zlp==1 && ncm_bad==6 && transfer_active[6]);
    ((struct usb_trb *)(memory+0x2300))->size=NCM_NTB_MAX+1;
    event_one((6U<<1)|(1U<<6));assert(ncm_bad_reason[NCM_RESIDUE]==1 && transfer_active[6]);
    setup(1,11,0,0,0);status_ready(1);complete(1);
    assert(ncm_alt && net_up && transfer_active[6]); /* log reconnect does not kill NCM */
    mux_test();
    tx_queue_test();
    setup(1,11,0,2,0);status_ready(1);complete(1);
    assert(!ncm_alt && !net_up && !transfer_active[6] && transfer_active[2] && mux_up);
    event_one(1|(1U<<8));assert(!configuration && !transfer_active[5] && !mux_up && !transfer_active[8] && mux_reset);
    ncm_enabled=false;
    /* Poll the real event loop past 64 entries and through a full 4 KiB wrap.
     * Fake MMIO models GEVNTCOUNT's subtract-on-write semantics. */
    static uint32_t fake_gcc[0x100000/4];
    static uint8_t ring_memory[EVENT_SIZE];
    gcc = fake_gcc; events = ring_memory; event_pa = 0xff9f4000;
    wr(gcc, 0xf004, BIT(31));
    wr(regs, DCTL, BIT(31)); wr(regs, DCFG, 0);
    wr(regs, GEVNTADRLO, event_pa); wr(regs, GEVNTADRHI, 0);
    wr(regs, GEVNTSIZ, EVENT_SIZE | BIT(31));
    unsigned before = event_total;
    event_pos = EVENT_SIZE - 4;
    for (unsigned batch = 0; batch < 4; ++batch) {
        uint32_t value = 0;
        for (unsigned j = 0; j < 300; ++j) {
            value = ((batch * 300 + j) << 16) | (4U << 1) | (1U << 6);
            memcpy(events + (event_pos + 4*j) % EVENT_SIZE, &value, 4);
        }
        regs[GEVNTCOUNT/4] = 300 * 4;
        poll_events();
        assert(!failed && rr(regs, GEVNTCOUNT) == 0 && last_event == value);
        assert(event_total == before + (batch+1)*300);
    }
    assert(event_pos == (EVENT_SIZE - 4 + 4800) % EVENT_SIZE && acknowledged == 4800);
    /* A new event arriving during acknowledgment is left for the next poll. */
    uint32_t e = (4U << 1) | (1U << 6);
    memcpy(events + event_pos, &e, 4);
    memcpy(events + (event_pos + 4) % EVENT_SIZE, &e, 4);
    regs[GEVNTCOUNT/4] = 4; arrive_on_ack = 1;
    poll_events(); assert(rr(regs, GEVNTCOUNT) == 4);
    poll_events(); assert(rr(regs, GEVNTCOUNT) == 0 && acknowledged == 4808);
    /* External core reinitialization stops polling before acknowledging or writing anything. */
    wr(regs, DCTL, 0xf00000); wr(regs, DCFG, 0x80805);
    poll_events(); assert(failed && acknowledged == 4808 && rr(regs, DCFG) == 0x80805);
    puts("PASS: EP0/reset/abort/resource-zero, log ABI, DMA/SID0 isolation, 1200 events with wrap/byte ACK/new arrival, external-owner detection (simulated)");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-usb-ep0-', dir=os.environ.get('TMPDIR')) as tmp:
    path = Path(tmp)
    (path / 'test.c').write_text(harness)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-Wno-unused-variable',
                    '-Wno-unused-function', '-fsanitize=address,undefined', '-g',
                    str(path / 'test.c'), '-o', str(path / 'test')], check=True)
    subprocess.run([str(path / 'test')], check=True)
