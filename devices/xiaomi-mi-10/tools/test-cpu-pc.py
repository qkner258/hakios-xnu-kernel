#!/usr/bin/env python3
"""Host-only tests of the per-processor breadcrumbs and the hung-machine register dump (v29i): the
kernel header's entry points, the loader's copy of the structure, its report (version 1 and 2),
and the read-only GIC/SMMU/PCIe dump. No phone, no kernel build: the real sources are compiled
against fakes, as tools/test-sleep.py and tools/test-usb-handoff.py do."""
import pathlib
import re
import subprocess
import tempfile

D = pathlib.Path(__file__).resolve().parents[1]
HEADER = D / 'overlays/xnu/files/osfmk/arm64/umi_cpu_pc.h'
LOADER = D / 'loader/loader.c'


def between(text, start_marker, end_marker):
    a = text.index(start_marker)
    b = text.index(end_marker, a) + len(end_marker)
    return text[a:b]


def function(text, name):
    # (the loader puts a definition's return type on a line of its own: "static void", "static int")
    match = re.search(r'(?m)^static [^\n]+\n' + re.escape(name) + r'\(', text)
    assert match, name
    a = match.start()
    return text[a:text.index('\n}\n', a) + 3]


loader_c = LOADER.read_text()
kernel_h = HEADER.read_text()

begin = loader_c.index('#define LX_MMIO_ACCESSES')
end = loader_c.index('\n};\n', loader_c.index('struct lx_cpu_breadcrumbs {', begin)) + len('\n};\n')
loader_struct = loader_c[begin:end]
loader_live = between(loader_c, 'struct kernel_live {', '\n};\n')
loader_report = (function(loader_c, 'hung_cpu_pc_open') + '\n' + function(loader_c, 'hung_cpu_pc_panic') +
                 '\n' + function(loader_c, 'log_of_hung_cpu_pc'))
loader_last = (function(loader_c, 'hung_dbg_kind') + '\n' + function(loader_c, 'hung_panic_stage') + '\n' +
               function(loader_c, 'hung_log_panic') + '\n' + function(loader_c, 'log_of_hung_last'))

# the E2 dump: its constants and the functions that read the registers (the two read helpers are
# the test's own, so that every register value comes from the fake below)
dump_a = loader_c.index('#define HUNG_GICD_CTLR')
dump_b = loader_c.index('\n\nstatic uint32_t\nhung_mmio_rd32', dump_a)
dump_defs = loader_c[dump_a:dump_b]
struct_a = loader_c.index('struct hung_smmu_cb {')
dump_struct = loader_c[struct_a:loader_c.index('\n};\n', struct_a) + 4]
dump_functions = '\n'.join(function(loader_c, name) for name in (
    'hung_spi_list', 'hung_gic_dump', 'hung_smmu_lookup', 'hung_smmu_cb_print', 'hung_smmu_dump',
    'hung_pcie_dump', 'log_of_hung_mmio_state'))

assert 'struct lx_cpu_breadcrumbs {' in loader_struct and loader_struct.count('struct lx_cpu_breadcrumb ') >= 1
assert 'cpu_pc' in loader_live and 'magic4' in loader_live
assert 'intids' in loader_struct and 'gic_unhandled_count' in loader_struct
assert 'spin' in loader_report and '%p' in loader_report and 'irq' in loader_report
assert 'HUNG_GICD_CTLR' in dump_defs and 'hung_pcie_dump' in dump_functions
# (v34) the panic's fast path parks the panicking processor instead of returning to the loader,
# and the loader reports on the record's word rather than only on 50 s of silence
assert 'umi_cpu_pc_panic_park' in kernel_h and 'LX_PANIC_STAGE_PARKED' in kernel_h
assert 'umi_cpu_pc_panicked' in kernel_h
assert 'hung_panic_parked' in loader_c and 'PANIC_REPORT_SECONDS' in loader_c


def run(source, directory, name, extra_headers=None):
    (directory / name).mkdir(exist_ok=True)
    work = directory / name
    (work / (name + '.c')).write_text(source)
    for path, text in (extra_headers or {}).items():
        (work / path).parent.mkdir(parents=True, exist_ok=True)
        (work / path).write_text(text)
    subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-parameter',
                    '-I' + str(work), str(work / (name + '.c')), '-o', str(work / name)], check=True)
    return subprocess.run([str(work / name)], check=True, capture_output=True, text=True).stdout


thread_h = '''
#include <stdint.h>
typedef int boolean_t;                  /* (mach/boolean.h, as the kernel's kern/thread.h brings it) */
#ifndef TRUE
#define TRUE 1
#define FALSE 0
#endif
struct thread { uint64_t thread_id; };
typedef struct thread *thread_t;
static struct thread fake_thread = { .thread_id = 0x1f3 };
static inline thread_t current_thread(void) { return &fake_thread; }
static inline uint64_t thread_tid(thread_t thread) { return thread ? thread->thread_id : 0; }
'''

report_harness = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define KERNEL_LIVE_MAGIC2      0x324556494c554e58ULL
#define KERNEL_LIVE_MAGIC3      0x334556494c554e58ULL
#define KERNEL_LIVE_MAGIC4      0x344556494c554e58ULL

/* (the loader's own copies, cut out of loader.c by the test) */
__LOADER_STRUCT__
__LOADER_LIVE__

static uint64_t hung_mmio_newest_ns;
static const struct lx_cpu_breadcrumbs *hung_cpu_table;
static int hung_cpu_opened;
static uint64_t kernel_image_lo = 0x40000000, kernel_image_hi = 0x50000000;
static uint64_t kernel_window_lo = 0x40000000, kernel_window_hi = 0x58000000;
static char out[16384];
static size_t out_len;
static void flush_dcache(uint64_t a, uint64_t b) { (void)a; (void)b; }
static void print(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') { out[out_len++] = *fmt; continue; }
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) out[out_len++] = *s++;
		} else if (*fmt == 'p' || *fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = *fmt == 'd' ? 10 : 16;
			char tmp[24]; int i = 0;
			do { tmp[i++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
			if (*fmt == 'p') for (int pad = i; pad < 16; pad++) out[out_len++] = '0';
			while (i) out[out_len++] = tmp[--i];
		} else out[out_len++] = *fmt;
	}
	out[out_len] = 0;
	__builtin_va_end(ap);
}
__LOADER_REPORT__

/* (the two MMIO helpers log_of_hung_last() prints through: the ring itself is the mmio harness's) */
static const struct lx_mmio_access *hung_mmio_pending(const struct lx_mmio_ring *ring) { (void)ring; return NULL; }
static const char *hung_mmio_what(const struct lx_mmio_access *a) { (void)a; return "?"; }
__LOADER_LAST__

/* (the table lives in a buffer of the largest size a version 6 table can have:
 * 64 + 8 * 192 + 448 + 8 * 128 + 128 + 256) */
static unsigned char arena[4096] __attribute__((aligned(64)));
#define table (*(struct lx_cpu_breadcrumbs *)(void *)arena)
static struct kernel_live live;

/* (the version 3 layout: a 192-byte entry, and the exit ring after the entries) */
static struct lx_cpu_breadcrumb *core_of(unsigned i)
{
	return (struct lx_cpu_breadcrumb *)(void *)(arena + LX_CPU_PC_CORE_OFFSET + (uint64_t)i * table.size);
}
static struct lx_cpu_pmap_record *record_of(unsigned i)
{
	return (struct lx_cpu_pmap_record *)(void *)((char *)core_of(i) + sizeof(struct lx_cpu_breadcrumb));
}
static uint64_t *exits_total(void)
{
	return (uint64_t *)(void *)(arena + LX_CPU_PC_CORE_OFFSET + (uint64_t)table.cores * table.size);
}
static struct lx_proc_exit *exit_of(unsigned i)
{
	return (struct lx_proc_exit *)(void *)((char *)exits_total() + LX_CPU_EXIT_RING_OFFSET) + i;
}
static struct lx_cpu_debugger *debug_of(unsigned i)
{
	return (struct lx_cpu_debugger *)(void *)((char *)exits_total() + LX_CPU_PC_DEBUG_OFFSET) + i;
}
static struct lx_dbg_globals *globals_of(void)
{
	return (struct lx_dbg_globals *)(void *)((char *)exits_total() + LX_CPU_PC_DEBUG_OFFSET +
	    LX_CPU_PC_CORES * sizeof(struct lx_cpu_debugger));
}
static struct lx_panic_record *panic_of(void)
{
	return (struct lx_panic_record *)(void *)((char *)exits_total() + LX_CPU_PC_PANIC_OFFSET);
}

static void reset(void)
{
	out_len = 0; out[0] = 0;
	hung_mmio_newest_ns = 0;
	hung_cpu_table = NULL;
	hung_cpu_opened = 0;
	memset(arena, 0, sizeof(arena));
	memset(&live, 0, sizeof(live));
	live.magic2 = KERNEL_LIVE_MAGIC2;
	live.magic3 = KERNEL_LIVE_MAGIC3;
	live.magic4 = KERNEL_LIVE_MAGIC4;
	table.magic = LX_CPU_PC_MAGIC;
	table.version = 2;
	table.cores = LX_CPU_PC_CORES;
	table.size = sizeof(struct lx_cpu_breadcrumb);
	/* the loader's view of a dying kernel: the table's link address, its physical address
	 * (the fake one), and the RAM window and image the loader put the kernel in */
	uint64_t table_pa = (uint64_t)(uintptr_t)arena;
	live.cpu_pc = 0xfffffff008000000ULL;
	kernel_image_lo = table_pa - 0x1000;
	kernel_image_hi = table_pa + sizeof(arena) + 0x1000;
	kernel_window_lo = kernel_image_lo;
	kernel_window_hi = kernel_image_hi;
}
/* the slide that takes the kernel's VA to the fake table's address */
static uint64_t slide_of(void)
{
	return (uint64_t)(uintptr_t)arena - live.cpu_pc;
}

int main(void)
{
	/* 1. the structure is one 64-byte line of capture, one of state and (version 3) one of teardown;
	 *    the loader must see exactly the same bytes, so print the layout for the Python side to
	 *    compare. (Its own `struct lx_cpu_breadcrumb` holds the first two lines, and the third is a
	 *    `struct lx_cpu_pmap_record` of its own, because a version 1 or 2 table has only two.) */
	printf("loader size %zu pc %zu lr %zu sp %zu when %zu count %zu tid %zu state %zu mpidr %zu\n",
	    sizeof(struct lx_cpu_breadcrumb) + sizeof(struct lx_cpu_pmap_record),
	    offsetof(struct lx_cpu_breadcrumb, pc),
	    offsetof(struct lx_cpu_breadcrumb, lr), offsetof(struct lx_cpu_breadcrumb, sp),
	    offsetof(struct lx_cpu_breadcrumb, when_ns), offsetof(struct lx_cpu_breadcrumb, count),
	    offsetof(struct lx_cpu_breadcrumb, tid), offsetof(struct lx_cpu_breadcrumb, state),
	    offsetof(struct lx_cpu_breadcrumb, mpidr));
	printf("loader idle %zu since %zu caller %zu lock %zu spinwhen %zu spins %zu flags %zu intids %zu\n",
	    offsetof(struct lx_cpu_breadcrumb, idle_entries), offsetof(struct lx_cpu_breadcrumb, idle_since_ns),
	    offsetof(struct lx_cpu_breadcrumb, lock_caller), offsetof(struct lx_cpu_breadcrumb, lock),
	    offsetof(struct lx_cpu_breadcrumb, spin_when_ns), offsetof(struct lx_cpu_breadcrumb, spins),
	    offsetof(struct lx_cpu_breadcrumb, flags), offsetof(struct lx_cpu_breadcrumb, intids));
	printf("loader last %zu count %zu table %zu full %zu\n",
	    offsetof(struct lx_cpu_breadcrumbs, gic_last_unhandled),
	    offsetof(struct lx_cpu_breadcrumbs, gic_unhandled_count),
	    LX_CPU_PC_CORE_OFFSET + LX_CPU_PC_CORES * sizeof(struct lx_cpu_breadcrumb),
	    LX_CPU_PC_CORE_OFFSET + LX_CPU_PC_CORES * (sizeof(struct lx_cpu_breadcrumb) + sizeof(struct lx_cpu_pmap_record)) +
	    LX_CPU_EXIT_RING_OFFSET + LX_CPU_EXIT_RING * sizeof(struct lx_proc_exit) +
	    LX_CPU_PC_CORES * sizeof(struct lx_cpu_debugger) + sizeof(struct lx_dbg_globals) +
	    sizeof(struct lx_panic_record));
	/* (version 3, the same six numbers the kernel's harness prints) */
	printf("loader record %zu entry %zu phase %zu exit %zu ringoff %zu ring %zu\n",
	    sizeof(struct lx_cpu_pmap_record),
	    sizeof(struct lx_cpu_breadcrumb) + sizeof(struct lx_cpu_pmap_record),
	    sizeof(struct lx_cpu_breadcrumb), sizeof(struct lx_proc_exit),
	    LX_CPU_PC_CORE_OFFSET + LX_CPU_PC_CORES * (sizeof(struct lx_cpu_breadcrumb) + sizeof(struct lx_cpu_pmap_record)),
	    (size_t)LX_CPU_EXIT_RING_OFFSET);
	/* (version 5: the debugger's block, which the loader must see at the same offsets) */
	printf("loader dbgsz %zu dbgkind %zu dbgsince %zu dbgroles %zu dbgexc %zu dbgelr %zu dbgextra %zu dbgxf %zu dbgxa %zu dbgxw %zu\n",
	    sizeof(struct lx_cpu_debugger), offsetof(struct lx_cpu_debugger, kind),
	    offsetof(struct lx_cpu_debugger, since_ns), offsetof(struct lx_cpu_debugger, roles),
	    offsetof(struct lx_cpu_debugger, exc_code), offsetof(struct lx_cpu_debugger, elr),
	    offsetof(struct lx_cpu_debugger, extra), offsetof(struct lx_cpu_debugger, xcall_func),
	    offsetof(struct lx_cpu_debugger, xcall_arg), offsetof(struct lx_cpu_debugger, xcall_when_ns));
	printf("loader gsz %zu gcount %zu gfirst %zu gfirstkind %zu glast %zu glastkind %zu\n",
	    sizeof(struct lx_dbg_globals), offsetof(struct lx_dbg_globals, count),
	    offsetof(struct lx_dbg_globals, first_cpu), offsetof(struct lx_dbg_globals, first_kind),
	    offsetof(struct lx_dbg_globals, last_cpu), offsetof(struct lx_dbg_globals, last_kind));
	/* (version 6: the panic's record, which the loader must see at the same offsets) */
	printf("loader panic %zu pmagic %zu pcpu %zu pcaller %zu pwhen %zu pstage %zu pstagens %zu pfps %zu"
	    " psync %zu psent %zu ppending %zu pack %zu pmsglen %zu pstagecpu %zu pmsg %zu poff %zu\n",
	    sizeof(struct lx_panic_record), offsetof(struct lx_panic_record, magic),
	    offsetof(struct lx_panic_record, cpu), offsetof(struct lx_panic_record, caller),
	    offsetof(struct lx_panic_record, when_ns), offsetof(struct lx_panic_record, stage),
	    offsetof(struct lx_panic_record, stage_ns), offsetof(struct lx_panic_record, fps),
	    offsetof(struct lx_panic_record, xcall_sync), offsetof(struct lx_panic_record, xcall_sent),
	    offsetof(struct lx_panic_record, xcall_pending), offsetof(struct lx_panic_record, ack_timeouts),
	    offsetof(struct lx_panic_record, msg_len), offsetof(struct lx_panic_record, stage_cpu),
	    offsetof(struct lx_panic_record, msg), (size_t)LX_CPU_PC_PANIC_OFFSET);

	/* 2. nothing recorded at all is a refusal, not a crash */
	reset();
	live.magic4 = 0;
	log_of_hung_cpu_pc(&live, slide_of());
	assert(strstr(out, "left no record of where its processors were"));
	reset();
	live.magic3 = 0;
	log_of_hung_cpu_pc(&live, slide_of());
	assert(strstr(out, "too old to leave breadcrumbs"));
	reset();
	live.cpu_pc = 0xfffffff008000008ULL;             /* (not a cache-line boundary) */
	log_of_hung_cpu_pc(&live, slide_of());
	assert(strstr(out, "is not a pointer"));
	reset();
	table.magic = 0x1234;
	log_of_hung_cpu_pc(&live, slide_of());
	assert(strstr(out, "is not one this loader knows"));

	/* 2b. a version this loader does not know is refused; version 1 is printed without the INTIDs */
	reset();
	table.version = LX_CPU_PC_VERSION + 1;
	log_of_hung_cpu_pc(&live, slide_of());
	assert(strstr(out, "is not one this loader knows"));
	reset();
	table.version = LX_CPU_PC_VERSION_MIN;
	for (unsigned i = 0; i < LX_CPU_PC_CORES; i++) {
		table.core[i].pc = 0xfffffff007a1b000ULL + i;
		table.core[i].state = LX_CPU_STATE_IRQ;
		table.core[i].when_ns = 1999000000ULL;
		table.core[i].intids = 0xdeadbeefdeadbeefULL;   /* (a version 1 table has no such field) */
	}
	hung_mmio_newest_ns = 2000000000ULL;
	log_of_hung_cpu_pc(&live, slide_of());
	assert(strstr(out, "CPU 0 pc fffffff007a1b000"));
	assert(!strstr(out, " irq") && !strstr(out, "dead"));
	assert(!strstr(out, "GIC: unhandled"));

	/* 3. a table outside the image the loader put in RAM is refused before it is read */
	reset();
	kernel_image_lo = kernel_image_hi = 0;
	log_of_hung_cpu_pc(&live, slide_of());
	assert(strstr(out, "outside the kernel image"));

	/* 4. a real report: ages against the newest record of either kind, idle and spin marked,
	 *    and the two INTIDs of the GIC handler on the same lines. */
	reset();
	hung_mmio_newest_ns = 2000000000ULL;             /* t = 2.000 s on the same clock */
	for (unsigned i = 0; i < LX_CPU_PC_CORES; i++) {
		table.core[i].pc = 0xfffffff007a1b000ULL + i;
		table.core[i].lr = 0xfffffff007a1af00ULL + i;
		table.core[i].state = LX_CPU_STATE_IRQ;
		table.core[i].mpidr = i << 8;
		table.core[i].tid = 0x1f3 + i;
		table.core[i].when_ns = 1999000000ULL;       /* 1 ms before the radio's newest access */
	}
	table.core[2].flags = LX_CPU_FLAG_IDLE;
	table.core[5].flags = LX_CPU_FLAG_SPIN;
	table.core[5].lock_caller = 0xfffffff007b1a008ULL;
	table.core[5].lock = 0xffffffe001234560ULL;
	table.core[5].spin_when_ns = 1990000000ULL;      /* (10 ms before it, so the wait had lasted that) */
	table.core[7].when_ns = 1999999999ULL;           /* the newest breadcrumb: 1 ns old */
	table.core[0].intids = ((uint64_t)LX_CPU_INTID_NONE << 32) | 5;   /* irq 5, nothing in flight */
	table.core[1].intids = ((uint64_t)27 << 32) | 27;                 /* irq 27 act 27 */
	table.core[6].state = 0;                                          /* never interrupted */
	table.gic_unhandled_count = 3;
	table.gic_last_unhandled = 480;
	log_of_hung_cpu_pc(&live, slide_of());
	printf("---- report ----\n%s---- end ----\n", out);
	assert(strstr(out, "CPU 0 pc fffffff007a1b000 lr fffffff007a1af00 t 1ms krn tid 1f3 irq 5 act -"));
	assert(strstr(out, "CPU 1 pc fffffff007a1b001 lr fffffff007a1af01 t 1ms krn tid 1f4 irq 27 act 27"));
	assert(strstr(out, "CPU 2 pc fffffff007a1b002 lr fffffff007a1af02 t 1ms krn idle tid 1f5 irq 0 act 0"));
	assert(strstr(out, "CPU 5 pc fffffff007a1b005 t 1ms krn spin fffffff007b1a008 lk ffffffe001234560 s 10ms irq 0 act 0"));
	assert(strstr(out, "CPU 6 pc fffffff007a1b006 lr fffffff007a1af06 t 1ms none tid 1f9 irq - act -"));
	assert(strstr(out, "CPU 7 pc fffffff007a1b007 lr fffffff007a1af07 t 0ms krn tid 1fa irq 0 act 0"));
	assert(strstr(out, "GIC: unhandled 3, the last 1e0"));
	assert(!strstr(out, "tlbi") && !strstr(out, "EXITS"));     /* (a version 2 table has neither) */

	/* 5. a version 3 table: how far each processor's pmap teardown got, and the ring of exits */
	reset();
	table.version = LX_CPU_PC_VERSION;
	table.size = sizeof(struct lx_cpu_breadcrumb) + sizeof(struct lx_cpu_pmap_record);
	hung_mmio_newest_ns = 2000000000ULL;             /* the newest record of any kind */
	for (unsigned i = 0; i < LX_CPU_PC_CORES; i++) {
		core_of(i)->pc = 0xfffffff007a1b000ULL + i;
		core_of(i)->lr = 0xfffffff007a1af00ULL + i;
		core_of(i)->state = LX_CPU_STATE_IRQ;
		core_of(i)->tid = 0x1e0 + i;
		core_of(i)->intids = ((uint64_t)LX_CPU_INTID_NONE << 32) | 27;
		core_of(i)->when_ns = 1999000000ULL;         /* 1 ms before it */
	}
	record_of(5)->phase = LX_PMAP_PHASE_TLBI;        /* (a teardown waiting for its dsb) */
	record_of(5)->when_ns = 1500000000ULL;
	record_of(5)->pmap = 0xffffffe00a1b2000ULL;
	record_of(5)->ids = ((uint64_t)0x1f4 << 16) | 0x1a;
	memcpy(record_of(5)->comm, "backboardd", 11);
	record_of(6)->phase = LX_PMAP_PHASE_DONE;        /* (one that returned: not a suspect) */
	record_of(6)->when_ns = 1900000000ULL;
	record_of(6)->pmap = 0xffffffe00a1b3000ULL;
	record_of(6)->ids = ((uint64_t)0x1e5 << 16) | 7;
	memcpy(record_of(6)->comm, "launchd", 8);
	record_of(4)->mmio_since_ns = 1998000000ULL;     /* (version 4: inside one of the radio's accesses) */
	record_of(4)->mmio_seq = 0x2a;
	*exits_total() = 3;
	exit_of(1)->seq = 1; exit_of(1)->when_ns = 1500000000ULL; exit_of(1)->pid = 0x1e0; exit_of(1)->cpu = 3;
	memcpy(exit_of(1)->comm, "dock", 5);
	exit_of(2)->seq = 2; exit_of(2)->when_ns = 1800000000ULL; exit_of(2)->pid = 0x1e5; exit_of(2)->cpu = 3;
	memcpy(exit_of(2)->comm, "launchd", 8);
	exit_of(3)->seq = 3; exit_of(3)->when_ns = 1999000000ULL; exit_of(3)->pid = 0x1f4; exit_of(3)->cpu = 7;
	memcpy(exit_of(3)->comm, "backboardd", 11);
	log_of_hung_cpu_pc(&live, slide_of());
	printf("---- report3 ----\n%s---- end3 ----\n", out);
	assert(strstr(out, "CPU 5 pc fffffff007a1b005 lr fffffff007a1af05 t 1ms krn tid 1e5 irq 27 act -"
	    " tlbi 2 500ms ffffffe00a1b2000 asid 1a pid 1f4 backboardd\n"));
	assert(strstr(out, "CPU 6 pc fffffff007a1b006 lr fffffff007a1af06 t 1ms krn tid 1e6 irq 27 act -"
	    " tlbi 4 100ms ffffffe00a1b3000 asid 7 pid 1e5 launchd\n"));
	assert(strstr(out, " mmio 2ms #2a"));            /* (version 4: the processor inside an access) */
	assert(strstr(out, "CPU 0 pc fffffff007a1b000 lr fffffff007a1af00 t 1ms krn tid 1e0 irq 27 act -\n"));
	assert(strstr(out, "EXITS 3 (newest first): 1f4 backboardd 1ms c7 | 1e5 launchd 200ms c3 |"
	    " 1e0 dock 500ms c3 |\n"));

	/* 6. a version 5 table: the broadcast xcall a processor was in, the processors still waiting in
	 *    the debugger's cross-call, and the two LAST: lines that name the entry (the v30 fingerprint:
	 *    every processor on SGI 0, no panic, no line) */
	reset();
	table.version = LX_CPU_PC_VERSION_DEBUGGER;      /* (a version 5 table: no panic record) */
	table.size = sizeof(struct lx_cpu_breadcrumb) + sizeof(struct lx_cpu_pmap_record);
	hung_mmio_newest_ns = 2000000000ULL;
	for (unsigned i = 0; i < LX_CPU_PC_CORES; i++) {
		core_of(i)->pc = 0xfffffff007a1b000ULL + i;
		core_of(i)->state = LX_CPU_STATE_IRQ;
		core_of(i)->intids = 0;                          /* (SGI 0 acknowledged and never ended) */
		core_of(i)->when_ns = 1999000000ULL;
	}
	debug_of(3)->xcall_func = 0xfffffff007a1b100ULL;     /* (about to run a broadcast xcall) */
	debug_of(3)->xcall_arg = 0xffffffe001234560ULL;
	debug_of(3)->xcall_when_ns = 1995000000ULL;          /* 500 ms before the newest record */
	debug_of(1)->kind = LX_DBG_KIND_XCALL_WAIT;          /* (waiting in the debugger's cross-call) */
	debug_of(1)->roles = LX_DBG_ROLE_RECEIVER;
	debug_of(1)->since_ns = 1999999000ULL;
	debug_of(2)->kind = LX_DBG_KIND_XCALL_WAIT;
	debug_of(2)->roles = LX_DBG_ROLE_RECEIVER;
	debug_of(2)->since_ns = 1999999000ULL;
	globals_of()->count = 1;                             /* (one entry, on processor 4) */
	globals_of()->first_cpu = 4;
	globals_of()->first_kind = LX_DBG_KIND_DEBUGGER;
	globals_of()->first_elr = 0xfffffff007315accULL;
	globals_of()->first_when_ns = 1999999500ULL;
	globals_of()->last_cpu = 4;
	globals_of()->last_kind = LX_DBG_KIND_DEBUGGER;
	globals_of()->last_elr = 0xfffffff007315accULL;
	globals_of()->last_when_ns = 1999999500ULL;
	log_of_hung_cpu_pc(&live, slide_of());
	printf("---- report5 ----\n%s---- end5 ----\n", out);
	assert(strstr(out, " xc 5ms fffffff007a1b100 ffffffe001234560"));
	assert(strstr(out, "CPU 1 pc fffffff007a1b001 lr 0000000000000000 t 1ms krn tid 0 irq 0 act 0 dbg-wait 0ms\n"));
	assert(strstr(out, "CPU 0 pc fffffff007a1b000 lr 0000000000000000 t 1ms krn tid 0 irq 0 act 0\n"));
	assert(hung_mmio_newest_ns == 2000000000ULL);         /* (an older debugger entry does not age it) */
	out_len = 0; out[0] = 0;
	log_of_hung_last(NULL, 7);
	printf("---- last5 ----\n%s---- endl5 ----\n", out);
	assert(strstr(out, "LAST: dbg 1 first c4 dbg e0/0 pc fffffff007315acc t 0ms\n"));
	assert(strstr(out, "LAST: dbg last c4 dbg e0/0 pc fffffff007315acc t 0ms rx 6\n"));
	for (const char *p = out; *p != '\0'; ) {        /* (no LAST: line may wrap at the console's 80 columns) */
		const char *nl = strchr(p, '\n');
		size_t n = nl != NULL ? (size_t)(nl - p) : strlen(p);

		assert(n <= 80);
		if (nl == NULL) {
			break;
		}
		p = nl + 1;
	}

	/*
	 * 7. a version 6 table: the panic's own record. Its three lines go at the top of the report and
	 *    again with the last lines, each within the console's 80 columns; a message longer than the
	 *    line is cut where the caller's address begins, and a table without a panic record prints
	 *    exactly what it did before (the assertions above are that case: version 6, no magic).
	 */
	reset();
	table.version = LX_CPU_PC_VERSION;
	table.size = sizeof(struct lx_cpu_breadcrumb) + sizeof(struct lx_cpu_pmap_record);
	hung_mmio_newest_ns = 2000000000ULL;
	for (unsigned i = 0; i < LX_CPU_PC_CORES; i++) {
		core_of(i)->pc = 0xfffffff007a1b000ULL + i;
		core_of(i)->state = LX_CPU_STATE_IRQ;
		core_of(i)->when_ns = 2000000000ULL;             /* (the newest records: 1 ms after the stage) */
	}
	assert(LX_CPU_PC_VERSION == 6);
	assert(LX_CPU_PC_VERSION_PANIC == 6);
	panic_of()->magic = LX_PANIC_MAGIC;
	panic_of()->cpu = 4;
	panic_of()->caller = 0xfffffff007331508ULL;
	panic_of()->when_ns = 1998000000ULL;
	panic_of()->stage = LX_PANIC_STAGE_QUIESCE_MPSS;
	panic_of()->stage_ns = 1999000000ULL;               /* 1 ms before the newest record */
	panic_of()->stage_cpu = 4;
	panic_of()->xcall_sync = 0;
	panic_of()->xcall_sent = 7;
	panic_of()->xcall_pending = 0;
	panic_of()->ack_timeouts = 0;
	panic_of()->fps[0] = 0xfffffff007331508ULL;         /* (the caller again, then up the chain) */
	panic_of()->fps[1] = 0xfffffff0073a1b20ULL;
	panic_of()->fps[2] = 0xfffffff007005a04ULL;
	panic_of()->fps[3] = 0xfffffff007a1b2c4ULL;
	panic_of()->fps[4] = 0xfffffff007331510ULL;         /* (a fifth one: it does not fit on the line) */
	{
		/* (a message longer than the 48 columns the line keeps, with a newline in it) */
		static const char message[] =
		    "zone_require failed: invalid zone type\nat address 0xffffffe00a1b2000";
		size_t len = sizeof(message) - 1;

		memcpy(panic_of()->msg, message, len + 1);
		panic_of()->msg_len = (uint32_t)len;
	}
	log_of_hung_cpu_pc(&live, slide_of());
	/* (the processor lines are what they were: the panic's lines are the report's first and last
	 * ones, printed by report_hung_kernel() and by log_of_hung_last(), not on these lines) */
	assert(!strstr(out, "PANIC"));
	out_len = 0; out[0] = 0;
	log_of_hung_last(NULL, 4);
	printf("---- last6 ----\n%s---- endl6 ----\n", out);
	assert(strstr(out, "PANIC: zone_require failed: invalid zone type.at addres caller fffffff007331508\n"));
	assert(strstr(out, "PANIC BT: fffffff007331508 fffffff0073a1b20 fffffff007005a04 fffffff007a1b2c4\n"));
	assert(!strstr(out, " fffffff007331510 fffffff"));      /* (the fifth frame does not fit on the line) */
	assert(strstr(out, "LAST: panic stage 14 (QUIESCE_MPSS) c4 t 1ms\n"));
	for (const char *p = out; *p != '\0'; ) {
		const char *nl = strchr(p, '\n');
		size_t n = nl != NULL ? (size_t)(nl - p) : strlen(p);

		assert(n <= 80);                                /* (every line of the tail fits the panel) */
		if (nl == NULL) {
			break;
		}
		p = nl + 1;
	}
	/* (the stage's own name, for a report read by a person) */
	for (uint64_t stage = LX_PANIC_STAGE_ENTRY; stage <= LX_PANIC_STAGE_PARKED; stage++) {
		assert(strcmp(hung_panic_stage(stage), "none") != 0);    /* (every number the kernel writes is named) */
	}
	assert(strcmp(hung_panic_stage(LX_PANIC_STAGE_PARKED), "PARKED") == 0);
	assert(LX_PANIC_STAGE_LAST == LX_PANIC_STAGE_PARKED);
	assert(strcmp(hung_panic_stage(LX_PANIC_STAGE_NONE), "none") == 0);
	assert(strcmp(hung_panic_stage(LX_PANIC_STAGE_LAST + 1), "none") == 0);

	/* ... and a table whose entries are the old size is refused as a whole */
	reset();
	table.version = LX_CPU_PC_VERSION;
	log_of_hung_cpu_pc(&live, slide_of());
	assert(strstr(out, "is not one this loader knows"));
	return 0;
}
'''
report_harness = (report_harness.replace('__LOADER_STRUCT__', loader_struct)
                               .replace('__LOADER_LIVE__', loader_live)
                               .replace('__LOADER_REPORT__', loader_report)
                               .replace('__LOADER_LAST__', loader_last))

mmio_harness = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

__DUMP_DEFS__

__LOADER_STRUCT__

static int screen_quiet, screen_forced;
static const struct lx_cpu_breadcrumbs *hung_cpu_table;
static char out[16384], dbg[65536];
static size_t out_len, dbg_len;

static void put(char c)
{
	assert(dbg_len + 2 < sizeof(dbg) && out_len + 2 < sizeof(out));
	dbg[dbg_len++] = c;
	if (!screen_quiet || screen_forced) {
		out[out_len++] = c;
	}
}
static void print(const char *fmt, ...)
{
	__builtin_va_list ap;
	__builtin_va_start(ap, fmt);
	for (; *fmt; fmt++) {
		if (*fmt != '%') { put(*fmt); continue; }
		fmt++;
		if (*fmt == 's') {
			const char *s = __builtin_va_arg(ap, const char *);
			while (*s) put(*s++);
		} else if (*fmt == 'p' || *fmt == 'x' || *fmt == 'd') {
			uint64_t v = __builtin_va_arg(ap, uint64_t);
			unsigned base = *fmt == 'd' ? 10 : 16;
			char tmp[24]; int i = 0;
			do { tmp[i++] = "0123456789abcdef"[v % base]; v /= base; } while (v);
			if (*fmt == 'p') for (int pad = i; pad < 16; pad++) put('0');
			while (i) put(tmp[--i]);
		} else put(*fmt);
	}
	__builtin_va_end(ap);
}

/* (the platform the dump is gated on: the phone's GIC, from loader.c's platform_umi) */
struct platform { uint64_t gic_dist, gic_redist; };
static const struct platform platform_umi = { .gic_dist = 0x17a00000, .gic_redist = 0x17a60000 };
static const struct platform *PLAT = &platform_umi;

/* The registers the dump reads, as the fake sets them. */
static uint32_t f_ctlr = 0x37, f_typer = 1;      /* (ITLinesNumber 1: two SPI words, 32..63) */
static uint32_t f_pend[8], f_act[8];
static uint32_t f_gdsc0 = 0x80000000u, f_gdsc2 = 0;
static uint32_t f_spi_pend_word1 = 0x10000;      /* (INTID 48) */
static unsigned pcie0_reads, pcie2_reads;

static uint32_t
hung_mmio_rd32(uint64_t pa)
{
	if (pa == 0x17a00000) return f_ctlr;
	if (pa == 0x17a00004) return f_typer;
	if (pa == 0x17a00204) return f_spi_pend_word1;
	if (pa == 0x17a00304) return 0;
	for (unsigned k = 0; k < 8; k++) {
		uint64_t base = 0x17a60000 + (uint64_t)k * 0x20000;
		if (pa == base + 0x0014) return 2;
		if (pa == base + 0x10200) return f_pend[k];
		if (pa == base + 0x10300) return f_act[k];
	}
	if (pa == 0x00100000 + 0x6b004) return f_gdsc0;
	if (pa == 0x00100000 + 0x6004) return f_gdsc2;
	if (pa == 0x01c00000 + 0x1b0) { pcie0_reads++; return 0x11; }
	if (pa == 0x01c00000 + 0x2c00) return 0;
	if (pa == 0x01c10000 + 0x1b0) { pcie2_reads++; return 0x11; }
	if (pa == 0x01c10000 + 0x2c00) return 0;
	if (pa == 0x01c00000 || pa == 0x01c00040 || pa == 0x01c10000 || pa == 0x01c10040) return 0;
	if (pa == 0x15000000) return 0x250406;        /* sCR0: enabled */
	if (pa == 0x15000020) return 0x4c017e57;      /* IDR0: 87 stream match registers */
	if (pa == 0x15000024) return 0x6000004a;      /* IDR1: 74 context banks, 4 KiB pages, 128 pages */
	if (pa == 0x15000048 || pa == 0x15000054 || pa == 0x15000074) return 0;
	if (pa >= 0x15000800 && pa < 0x15000c00) {    /* SMR: SID 0x820 at 10, SID 0 at 5 */
		unsigned i = (unsigned)((pa - 0x15000800) / 4);
		if (i == 10) return 0x80000820;
		if (i == 5) return 0x80000000;
		return 0;
	}
	if (pa >= 0x15000c00 && pa < 0x15001000) {    /* S2CR: both translated, context banks 10 and 5 */
		unsigned i = (unsigned)((pa - 0x15000c00) / 4);
		if (i == 10) return 10;
		if (i == 5) return 5;
		return 0;
	}
	return 0;
}

static uint64_t
hung_mmio_rd64(uint64_t pa)
{
	for (unsigned k = 0; k < 8; k++) {
		if (pa == 0x17a60000 + (uint64_t)k * 0x20000 + 0x0008) {
			return ((uint64_t)k << 40) | (k == 7 ? 0x10 : 0);   /* (affected core in [47:40], LAST on 7) */
		}
	}
	return 0;
}

__DUMP_STRUCT__

__DUMP_FUNCTIONS__

static struct lx_cpu_breadcrumbs fake_table;

static void reset(void)
{
	memset(&fake_table, 0, sizeof(fake_table));
	fake_table.version = LX_CPU_PC_VERSION;
	fake_table.cores = LX_CPU_PC_CORES;
	fake_table.size = sizeof(struct lx_cpu_breadcrumb);
	hung_cpu_table = &fake_table;
	out_len = dbg_len = 0; out[0] = dbg[0] = 0;
	screen_quiet = 0; screen_forced = 1;
	f_ctlr = 0x37; f_typer = 1; f_spi_pend_word1 = 0x10000;
	f_gdsc0 = 0x80000000u; f_gdsc2 = 0;
	memset(f_pend, 0, sizeof(f_pend));
	memset(f_act, 0, sizeof(f_act));
	pcie0_reads = pcie2_reads = 0;
}

static unsigned lines_of(const char *s)
{
	unsigned n = 0;
	for (; *s; s++) if (*s == '\n') n++;
	return n;
}

int main(void)
{
	/* 1. one short line each for the GIC (its distributor and the eight redistributors), the two
	 *    SMMUs streams, and the two links; everything else only in the log. */
	reset();
	f_pend[0] = 0x08000000u;                     /* PPI 27, the virtual timer, pending on core 0 */
	f_act[1] = 0x00000001u;                      /* SGI 0 active on core 1 */
	log_of_hung_mmio_state();
	printf("---- mmio ----\n%s---- end ----\n", out);
	assert(strstr(out, "GIC: ctlr 37 typer 1 unhandled 0\n"));
	assert(strstr(out, "GICR pend/act 0 8000000/0 1 0/1 2 0/0 3 0/0\n"));
	assert(strstr(out, "GICR pend/act 4 0/0 5 0/0 6 0/0 7 0/0\n"));
	assert(strstr(out, "GICD spi pend 48 act none\n"));
	assert(strstr(out, "SMMU: cr0 250406 idr0 4c017e57 gfsr 0 gfsynr1 0 tlbgstatus 0\n"));
	assert(strstr(out, "SMMU: sid820 cb10 sctlr 0 fsr 0 tlb 0 | sid0 cb5 sctlr 0 fsr 0 tlb 0\n"));
	assert(strstr(out, "PCIE: p0 ltssm 11 bdf2sid 0 | p2 off gdsc 0\n"));
	assert(lines_of(out) == 7);
	assert(strstr(dbg, "GIC detail:") && strstr(dbg, "GIC core 7 typer 0000070000000010"));
	assert(strstr(dbg, "SMMU detail:") && strstr(dbg, "SMMU sid 820") && strstr(dbg, "SMMU sid 0 "));
	assert(strstr(dbg, "PCIE detail:"));
	assert(pcie0_reads > 0 && pcie2_reads == 0);  /* (the powered-off link was never read) */

	/* 2. a link whose power domain is off is not read at all: that is the one read that could hang
	 *    the loader, so the GDSC decides before the PARF is touched */
	reset();
	f_gdsc0 = 0;
	f_gdsc2 = 0x80000000u;
	log_of_hung_mmio_state();
	assert(strstr(out, "PCIE: p0 off gdsc 0 | p2 ltssm 11 bdf2sid 0\n"));
	assert(pcie0_reads == 0 && pcie2_reads > 0);

	/* 3. a block that does not answer is skipped with one line, not read further */
	reset();
	f_ctlr = 0xffffffffu;
	log_of_hung_mmio_state();
	assert(strstr(out, "GIC: the distributor at 17a00000 does not answer (ffffffff): skipped\n"));
	assert(!strstr(out, "GICR pend/act"));

	/* 4. the handler's unhandled interrupt is on the GIC line when the table is one it can read */
	reset();
	fake_table.gic_unhandled_count = 2;
	fake_table.gic_last_unhandled = 0x1d4;
	log_of_hung_mmio_state();
	assert(strstr(out, "GIC: ctlr 37 typer 1 unhandled 2 last 1d4\n"));
	return 0;
}
'''
mmio_harness = (mmio_harness.replace('__DUMP_DEFS__', dump_defs)
                           .replace('__LOADER_STRUCT__', loader_struct)
                           .replace('__DUMP_STRUCT__', dump_struct)
                           .replace('__DUMP_FUNCTIONS__', dump_functions))

kernel_harness = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint64_t fake_mpidr = 0x700;                  /* the kernel's processor on this phone */
static uint64_t fake_now_ns;
#define __builtin_arm_rsr64(name) (fake_mpidr)   /* (the header reads MPIDR_EL1 only) */

#include "arm64/umi_cpu_pc.h"

struct lx_cpu_breadcrumbs umi_cpu_pc_table = {
	.magic = LX_CPU_PC_MAGIC,
	.version = LX_CPU_PC_VERSION,
	.cores = LX_CPU_PC_CORES,
	.size = sizeof(struct lx_cpu_breadcrumb),
};
uint64_t umi_cpu_pc_now_ns(void) { return fake_now_ns; }

int main(void)
{
	printf("kernel size %zu pc %zu lr %zu sp %zu when %zu count %zu tid %zu state %zu mpidr %zu\n",
	    sizeof(struct lx_cpu_breadcrumb), offsetof(struct lx_cpu_breadcrumb, pc),
	    offsetof(struct lx_cpu_breadcrumb, lr), offsetof(struct lx_cpu_breadcrumb, sp),
	    offsetof(struct lx_cpu_breadcrumb, when_ns), offsetof(struct lx_cpu_breadcrumb, count),
	    offsetof(struct lx_cpu_breadcrumb, tid), offsetof(struct lx_cpu_breadcrumb, state),
	    offsetof(struct lx_cpu_breadcrumb, mpidr));
	printf("kernel idle %zu since %zu caller %zu lock %zu spinwhen %zu spins %zu flags %zu intids %zu\n",
	    offsetof(struct lx_cpu_breadcrumb, idle_entries), offsetof(struct lx_cpu_breadcrumb, idle_since_ns),
	    offsetof(struct lx_cpu_breadcrumb, lock_caller), offsetof(struct lx_cpu_breadcrumb, lock),
	    offsetof(struct lx_cpu_breadcrumb, spin_when_ns), offsetof(struct lx_cpu_breadcrumb, spins),
	    offsetof(struct lx_cpu_breadcrumb, flags), offsetof(struct lx_cpu_breadcrumb, intids));
	printf("kernel last %zu count %zu table %zu full %zu\n",
	    offsetof(struct lx_cpu_breadcrumbs, gic_last_unhandled),
	    offsetof(struct lx_cpu_breadcrumbs, gic_unhandled_count),
	    offsetof(struct lx_cpu_breadcrumbs, core) + LX_CPU_PC_CORES * 128,
	    sizeof(struct lx_cpu_breadcrumbs));
	/* (version 3: where the pmap teardown sits in an entry, and where the exit ring sits in the table) */
	printf("kernel record %zu entry %zu phase %zu exit %zu ringoff %zu ring %zu\n",
	    sizeof(struct lx_cpu_breadcrumb) - offsetof(struct lx_cpu_breadcrumb, pmap_phase),
	    sizeof(struct lx_cpu_breadcrumb),
	    offsetof(struct lx_cpu_breadcrumb, pmap_phase), sizeof(struct lx_proc_exit),
	    offsetof(struct lx_cpu_breadcrumbs, exits), offsetof(struct lx_exit_ring, exit));
	/* (version 5: the debugger's block, which the loader must see at the same offsets) */
	printf("kernel dbgsz %zu dbgkind %zu dbgsince %zu dbgroles %zu dbgexc %zu dbgelr %zu dbgextra %zu dbgxf %zu dbgxa %zu dbgxw %zu\n",
	    sizeof(struct lx_cpu_debugger), offsetof(struct lx_cpu_debugger, kind),
	    offsetof(struct lx_cpu_debugger, since_ns), offsetof(struct lx_cpu_debugger, roles),
	    offsetof(struct lx_cpu_debugger, exc_code), offsetof(struct lx_cpu_debugger, elr),
	    offsetof(struct lx_cpu_debugger, extra), offsetof(struct lx_cpu_debugger, xcall_func),
	    offsetof(struct lx_cpu_debugger, xcall_arg), offsetof(struct lx_cpu_debugger, xcall_when_ns));
	printf("kernel gsz %zu gcount %zu gfirst %zu gfirstkind %zu glast %zu glastkind %zu\n",
	    sizeof(struct lx_dbg_globals), offsetof(struct lx_dbg_globals, count),
	    offsetof(struct lx_dbg_globals, first_cpu), offsetof(struct lx_dbg_globals, first_kind),
	    offsetof(struct lx_dbg_globals, last_cpu), offsetof(struct lx_dbg_globals, last_kind));
	/* (version 6: the panic's record, which the loader must see at the same offsets) */
	printf("kernel panic %zu pmagic %zu pcpu %zu pcaller %zu pwhen %zu pstage %zu pstagens %zu pfps %zu"
	    " psync %zu psent %zu ppending %zu pack %zu pmsglen %zu pstagecpu %zu pmsg %zu poff %zu\n",
	    sizeof(struct lx_panic_record), offsetof(struct lx_panic_record, magic),
	    offsetof(struct lx_panic_record, cpu), offsetof(struct lx_panic_record, caller),
	    offsetof(struct lx_panic_record, when_ns), offsetof(struct lx_panic_record, stage),
	    offsetof(struct lx_panic_record, stage_ns), offsetof(struct lx_panic_record, fps),
	    offsetof(struct lx_panic_record, xcall_sync), offsetof(struct lx_panic_record, xcall_sent),
	    offsetof(struct lx_panic_record, xcall_pending), offsetof(struct lx_panic_record, ack_timeouts),
	    offsetof(struct lx_panic_record, msg_len), offsetof(struct lx_panic_record, stage_cpu),
	    offsetof(struct lx_panic_record, msg),
	    (size_t)(offsetof(struct lx_cpu_breadcrumbs, panic) - offsetof(struct lx_cpu_breadcrumbs, exits)));

	/* the table is where the loader will look for it: its own signature first */
	assert(umi_cpu_pc_table.magic == LX_CPU_PC_MAGIC);
	assert(umi_cpu_pc_table.version == LX_CPU_PC_VERSION);
	assert(umi_cpu_pc_table.cores == LX_CPU_PC_CORES);
	assert(umi_cpu_pc_table.size == sizeof(struct lx_cpu_breadcrumb));
	assert(LX_CPU_PC_CORES == 8);
	assert(LX_CPU_PC_VERSION == 6);

	/* an interrupt on processor 7 records what the vector saved, when, and whose thread */
	struct lx_cpu_breadcrumb *b = &umi_cpu_pc_table.core[7];
	fake_now_ns = 1234567890123ULL;
	umi_cpu_pc_note_irq(0xfffffff007a1b2c4ULL, 0xfffffff007a1b124ULL, 0xffffffe001234560ULL, 0);
	assert(b->pc == 0xfffffff007a1b2c4ULL);
	assert(b->lr == 0xfffffff007a1b124ULL);
	assert(b->sp == 0xffffffe001234560ULL);
	assert(b->when_ns == fake_now_ns);
	assert(b->state == LX_CPU_STATE_IRQ);
	assert(umi_cpu_pc_table.core[7].state == LX_CPU_STATE_IRQ);
	assert(b->tid == 0x1f3);
	assert(b->mpidr == 7);
	assert(b->count == 1);
	umi_cpu_pc_note_irq(1, 2, 3, 1);
	assert(b->count == 2 && b->pc == 1);
	assert(b->state == (LX_CPU_STATE_IRQ | LX_CPU_STATE_USER));
	assert(umi_cpu_pc_table.core[0].count == 0);      /* (only this processor's entry) */

	/* the GIC handler's two INTIDs: one store, and "none" is not 0 (SGI 0 is a real interrupt) */
	uint64_t before = b->intids;
	umi_cpu_pc_note_irq(4, 5, 6, 0);
	assert(b->intids == before);                      /* (the interrupt path leaves it alone) */
	umi_cpu_pc_note_intid(27);
	assert(umi_cpu_pc_intid_last(b) == 27);
	assert(umi_cpu_pc_intid_active(b) == 27);
	umi_cpu_pc_note_intid_done();
	assert(umi_cpu_pc_intid_last(b) == 27);
	assert(umi_cpu_pc_intid_active(b) == LX_CPU_INTID_NONE);
	umi_cpu_pc_note_intid(0);                         /* SGI 0, the signal between processors */
	assert(umi_cpu_pc_intid_active(b) == 0 && umi_cpu_pc_intid_last(b) == 0);
	umi_cpu_pc_note_intid_done();
	assert(umi_cpu_pc_intid_active(b) == LX_CPU_INTID_NONE);

	/* the interrupt nobody owned: global, in the table's first line, and counted */
	assert(umi_cpu_pc_table.gic_unhandled_count == 0);
	umi_cpu_pc_note_unhandled(480);
	assert(umi_cpu_pc_table.gic_last_unhandled == 480);
	assert(umi_cpu_pc_table.gic_unhandled_count == 1);

	/* idle is a flag of its own, and the interrupt path does not disturb it */
	umi_cpu_pc_note_idle_enter();
	assert(b->flags == LX_CPU_FLAG_IDLE);
	assert(b->idle_entries == 1);
	assert(b->idle_since_ns == fake_now_ns);
	umi_cpu_pc_note_irq(4, 5, 6, 0);
	assert((b->flags & LX_CPU_FLAG_IDLE) && b->pc == 4);
	umi_cpu_pc_note_idle_exit();
	assert(b->flags == 0);

	/* a lock: who asked, for which lock, and the wait marked once */
	umi_cpu_pc_note_lock(0xfffffff007b1a008ULL, 0xffffffe001234560ULL);
	umi_cpu_pc_note_spin();
	assert(b->lock_caller == 0xfffffff007b1a008ULL);
	assert(b->lock == 0xffffffe001234560ULL);
	assert(b->flags == LX_CPU_FLAG_SPIN);
	assert(b->spins == 1);
	assert(b->spin_when_ns == fake_now_ns);
	umi_cpu_pc_note_spin_done();
	assert(b->flags == 0);
	assert(b->spins == 1);

	/* another processor's MPIDR lands in its own entry, and nothing past the table is written */
	fake_mpidr = 0x300;
	umi_cpu_pc_note_irq(7, 8, 9, 0);
	umi_cpu_pc_note_intid(33);
	assert(umi_cpu_pc_table.core[3].pc == 7 && b->pc != 7);
	assert(umi_cpu_pc_intid_last(&umi_cpu_pc_table.core[3]) == 33);
	fake_mpidr = 0x900;                               /* (not one of the eight: the last entry) */
	umi_cpu_pc_note_irq(10, 11, 12, 0);
	assert(umi_cpu_pc_table.core[7].pc == 10);

	/* a pmap teardown: the three phases, the pmap, its ASID, and the process that owned it */
	fake_mpidr = 0x700;
	fake_now_ns = 5555000ULL;
	umi_cpu_pc_note_pmap(LX_PMAP_PHASE_ENTER, 0xffffffe00a1b2000ULL, 0x1a, 0x1f4, "backboardd");
	assert(b->pmap_phase == LX_PMAP_PHASE_ENTER);
	assert(b->pmap_when_ns == fake_now_ns);
	assert(b->pmap == 0xffffffe00a1b2000ULL);
	assert((uint32_t)(b->pmap_ids & 0xffff) == 0x1a);
	assert((uint32_t)(b->pmap_ids >> 16) == 0x1f4);
	assert(strcmp(b->pmap_comm, "backboardd") == 0);
	assert(b->pmap_comm[11] == 0 && b->pmap_comm[15] == 0);   /* (the rest is padding, NUL) */
	fake_now_ns = 5556000ULL;
	umi_cpu_pc_note_pmap(LX_PMAP_PHASE_TLBI, 0xffffffe00a1b2000ULL, 0x1a, 0x1f4, "backboardd");
	assert(b->pmap_phase == LX_PMAP_PHASE_TLBI && b->pmap_when_ns == fake_now_ns);
	umi_cpu_pc_note_pmap(LX_PMAP_PHASE_DONE, 0xffffffe00a1b2000ULL, 0x1a, 0x1f4, "backboardd");
	assert(b->pmap_phase == LX_PMAP_PHASE_DONE);
	{                                                 /* (a kernel pmap: no pid, no name) */
		char name[16];
		memset(name, 'x', sizeof(name));
		umi_cpu_pc_note_pmap(LX_PMAP_PHASE_ENTER, 0xffffffe000000000ULL, 0, 0, name);
		assert(b->pmap_comm[0] == 'x');
		umi_cpu_pc_note_pmap(LX_PMAP_PHASE_ENTER, 0xffffffe000000000ULL, 0, 0, NULL);
		assert(b->pmap_comm[0] == 0 && b->pmap_comm[15] == 0);
		assert((uint32_t)(b->pmap_ids >> 16) == 0);
	}
	/* (the interrupt path leaves the teardown alone) */
	umi_cpu_pc_note_irq(13, 14, 15, 0);
	assert(b->pmap_phase == LX_PMAP_PHASE_ENTER && b->pmap == 0xffffffe000000000ULL);

	/* the exit ring: numbered, the newest in its own slot, the processor that ran the exit */
	assert(umi_cpu_pc_table.exits.seq == 0);
	fake_now_ns = 7000000ULL;
	umi_cpu_pc_note_exit(0x1e5, "launchd");
	assert(umi_cpu_pc_table.exits.seq == 1);
	assert(umi_cpu_pc_table.exits.exit[1].seq == 1);         /* (the number is the slot, mod 8) */
	assert(umi_cpu_pc_table.exits.exit[1].when_ns == fake_now_ns);
	assert(umi_cpu_pc_table.exits.exit[1].pid == 0x1e5);
	assert(umi_cpu_pc_table.exits.exit[1].cpu == 7);
	assert(strcmp(umi_cpu_pc_table.exits.exit[1].comm, "launchd") == 0);
	fake_mpidr = 0x300;
	umi_cpu_pc_note_exit(0x1f4, "backboardd");
	assert(umi_cpu_pc_table.exits.exit[2].pid == 0x1f4);
	assert(umi_cpu_pc_table.exits.exit[2].cpu == 3);
	for (int i = 0; i < LX_CPU_EXIT_RING; i++) {             /* (a full ring: the oldest slot is reused) */
		umi_cpu_pc_note_exit(0x100 + (uint64_t)i, "x");
	}
	assert(umi_cpu_pc_table.exits.seq == 10);
	assert(umi_cpu_pc_table.exits.exit[10 % LX_CPU_EXIT_RING].seq == 10);
	assert(umi_cpu_pc_table.exits.exit[10 % LX_CPU_EXIT_RING].pid == 0x107);
	assert(umi_cpu_pc_table.exits.exit[1 % LX_CPU_EXIT_RING].seq == 9);

	/* the radio's bus marker (version 4): the ring number and the time, written together, and
	 * only the mark this access set is this access's to take back (a thread that moved between
	 * the two calls must not clear another processor's or another access's mark) */
	fake_mpidr = 0x700;
	fake_now_ns = 8100000ULL;
	umi_cpu_pc_note_mmio(0x2a, fake_now_ns);
	assert(b->mmio_seq == 0x2a);
	assert(b->mmio_since_ns == fake_now_ns);
	umi_cpu_pc_note_mmio_done(0x29);                  /* (another access's number: the mark stays) */
	assert(b->mmio_seq == 0x2a && b->mmio_since_ns == fake_now_ns);
	umi_cpu_pc_note_mmio_done(0x2a);
	assert(b->mmio_seq == 0 && b->mmio_since_ns == 0);

	/* the debugger's entry (version 5): the first hook keeps it, the table's first and newest carry
	 * the processor and the pc, and the reason is filled in without disturbing the hook that began
	 * it; a receiving end gets its own entry, which stays until its wait returns */
	assert(umi_cpu_pc_table.dbg.count == 0);
	fake_mpidr = 0x500;
	fake_now_ns = 9000000ULL;
	umi_cpu_pc_note_debugger(LX_DBG_KIND_DEBUGGER, 0, 0xfffffff007315accULL, 0);
	assert(umi_cpu_pc_table.debug[5].kind == LX_DBG_KIND_DEBUGGER);
	assert(umi_cpu_pc_table.debug[5].since_ns == fake_now_ns);
	assert(umi_cpu_pc_table.debug[5].elr == 0xfffffff007315accULL);
	assert(umi_cpu_pc_table.debug[5].roles == 0);
	assert(umi_cpu_pc_table.dbg.count == 1);
	assert(umi_cpu_pc_table.dbg.first_cpu == 5 && umi_cpu_pc_table.dbg.last_cpu == 5);
	assert(umi_cpu_pc_table.dbg.first_kind == LX_DBG_KIND_DEBUGGER);
	assert(umi_cpu_pc_table.dbg.first_when_ns == fake_now_ns);
	fake_now_ns = 9000500ULL;
	umi_cpu_pc_note_debugger_why(6, 0, 3, 0);         /* (EXC_BREAKPOINT, a subcode) */
	assert(umi_cpu_pc_table.debug[5].kind == LX_DBG_KIND_DEBUGGER);   /* (the outermost hook kept it) */
	assert((uint32_t)umi_cpu_pc_table.debug[5].exc_code == 6);
	assert(umi_cpu_pc_table.debug[5].extra == 3);
	assert(umi_cpu_pc_table.dbg.count == 1);          /* (the reason is not a second entry) */
	assert((uint32_t)umi_cpu_pc_table.dbg.last_exc_code == 6);
	fake_mpidr = 0x100;                                /* (a processor signalled into it) */
	umi_cpu_pc_note_debugger(LX_DBG_KIND_XCALL_WAIT, LX_DBG_ROLE_RECEIVER, 0xfffffff007a1b2c4ULL, 0);
	assert(umi_cpu_pc_table.debug[1].kind == LX_DBG_KIND_XCALL_WAIT);
	assert(umi_cpu_pc_table.debug[1].roles == LX_DBG_ROLE_RECEIVER);
	assert(umi_cpu_pc_table.debug[1].elr == 0xfffffff007a1b2c4ULL);
	assert(umi_cpu_pc_table.dbg.count == 2);
	assert(umi_cpu_pc_table.dbg.last_cpu == 1 && umi_cpu_pc_table.dbg.first_cpu == 5);
	fake_now_ns = 9100000ULL;
	umi_cpu_pc_note_xcall(0xfffffff007a1b100ULL, 0xffffffe001234560ULL);
	assert(umi_cpu_pc_table.debug[1].xcall_func == 0xfffffff007a1b100ULL);
	assert(umi_cpu_pc_table.debug[1].xcall_arg == 0xffffffe001234560ULL);
	assert(umi_cpu_pc_table.debug[1].xcall_when_ns == fake_now_ns);
	umi_cpu_pc_note_debugger_done(LX_DBG_ROLE_RECEIVER);
	assert(umi_cpu_pc_table.debug[1].kind == LX_DBG_KIND_NONE);
	assert(umi_cpu_pc_table.debug[1].roles == 0 && umi_cpu_pc_table.debug[1].elr == 0);
	assert(umi_cpu_pc_table.debug[1].xcall_func == 0xfffffff007a1b100ULL);   /* (not the debugger's to clear) */
	assert(umi_cpu_pc_table.dbg.count == 2);          /* (the table's own first and newest stay) */
	fake_mpidr = 0x500;
	/* (the entry it opened, with no role left in it, is ended by the first done() that names this
	 * processor's -- the hooks always end what they began; the table's record is not touched) */
	umi_cpu_pc_note_debugger_done(0);
	assert(umi_cpu_pc_table.debug[5].kind == LX_DBG_KIND_NONE && umi_cpu_pc_table.debug[5].elr == 0);

	/* the panic's record (version 6): the message and the pc at the table's tail, the frame chain
	 * above it, the stage the panic reached, and how its debugger cross-call ended -- all plain
	 * stores, and a second panic keeps the first one's message */
	{
		struct lx_panic_record *panic = &umi_cpu_pc_table.panic;
		const uint64_t pcs[] = { 0xfffffff007331508ULL, 0xfffffff0073a1b20ULL, 0xfffffff007005a04ULL };
		char longmsg[LX_PANIC_MSG + 8];

		assert(panic->magic == 0);
		for (unsigned i = 0; i < sizeof(longmsg); i++) {
			longmsg[i] = (char)('a' + (i % 26));
		}
		fake_mpidr = 0x400;
		fake_now_ns = 12000000ULL;
		umi_cpu_pc_note_panic_bt(pcs, 3);          /* (before the message: no panic yet, harmless) */
		umi_cpu_pc_note_panic_xcall(2, 7, 0x10, 1);  /* (dropped: there is no panic in the record) */
		assert(panic->xcall_sent == 0 && panic->ack_timeouts == 0);
		umi_cpu_pc_note_panic(0xfffffff007331508ULL, (uint32_t)(strlen(longmsg) + 100), longmsg);
		assert(panic->magic == LX_PANIC_MAGIC);
		assert(panic->cpu == 4);
		assert(panic->caller == 0xfffffff007331508ULL);
		assert(panic->when_ns == fake_now_ns);
		assert(panic->msg_len == LX_PANIC_MSG - 1);        /* (a longer message is cut, and NUL-terminated) */
		assert(strncmp(panic->msg, longmsg, LX_PANIC_MSG - 1) == 0);
		assert(panic->msg[LX_PANIC_MSG - 1] == '\0');
		assert(panic->stage == LX_PANIC_STAGE_NONE);
		fake_mpidr = 0x700;                        /* (the stage names the processor that wrote it) */
		fake_now_ns = 12300000ULL;
		umi_cpu_pc_note_panic_stage(LX_PANIC_STAGE_SAVEINFO);
		assert(panic->stage == LX_PANIC_STAGE_SAVEINFO);
		assert(panic->stage_ns == fake_now_ns);
		assert(panic->stage_cpu == 7);
		umi_cpu_pc_note_panic_xcall(0, 7, 0, 0);   /* (every signalled processor answered) */
		assert(panic->xcall_sync == 0 && panic->xcall_sent == 7 && panic->xcall_pending == 0);
		assert(panic->ack_timeouts == 0);
		/* (a second panic keeps the first one's message: the reason for the first is the one worth
		 * reading, and this one is on its way to the same wall) */
		fake_now_ns = 12400000ULL;
		umi_cpu_pc_note_panic(0xfffffff007a1b2c4ULL, 3, "bad");
		assert(panic->caller == 0xfffffff007331508ULL && panic->when_ns == 12000000ULL);
		assert(strncmp(panic->msg, longmsg, LX_PANIC_MSG - 1) == 0);
		/* (the return addresses: the ones given, then zeros) */
		umi_cpu_pc_note_panic_bt(pcs, 3);
		assert(panic->fps[0] == pcs[0] && panic->fps[2] == pcs[2]);
		for (unsigned i = 3; i < LX_PANIC_FPS; i++) {
			assert(panic->fps[i] == 0);
		}
	}
	return 0;
}
'''

with tempfile.TemporaryDirectory() as temp:
    t = pathlib.Path(temp)
    loader_out = run(report_harness, t, 'loader_report')
    mmio_out = run(mmio_harness, t, 'loader_mmio')
    kernel_out = run(kernel_harness, t, 'kernel_breadcrumbs',
                     {'kern/thread.h': thread_h, 'arm64/umi_cpu_pc.h': kernel_h})

    def layout(tag, text):
        values = {}
        for line in text.splitlines():
            words = line.split()
            if words and words[0] == tag:
                values.update({words[i]: words[i + 1] for i in range(1, len(words) - 1, 2)})
        assert values, text
        return values

    # the kernel's structure and the loader's copy of it are the same bytes
    kernel = layout('kernel', kernel_out)
    loader = layout('loader', loader_out)
    assert kernel == loader, (kernel, loader)
    assert int(kernel['size']) == 192, kernel          # (a version 3 entry: two lines of state and one of teardown)
    assert int(kernel['intids']) == 120, kernel
    assert int(kernel['last']) == 24 and int(kernel['count']) == 32, kernel
    assert int(kernel['table']) == 1088, kernel        # (a version 1 or 2 table: the header and the entries)
    assert int(kernel['full']) == 3456, kernel         # (a version 6 table: ring, debugger block and the panic)
    assert int(kernel['record']) == 64 and int(kernel['entry']) == 192, kernel
    assert int(kernel['phase']) == 128 and int(kernel['exit']) == 48, kernel
    assert int(kernel['ringoff']) == 1600 and int(kernel['ring']) == 64, kernel
    assert int(kernel['dbgsz']) == 128, kernel         # (a version 5 debugger record: two lines)
    assert int(kernel['gsz']) == 128, kernel
    assert int(kernel['panic']) == 256, kernel         # (version 6: the panic's record, four lines)
    assert int(kernel['poff']) == 1600, kernel         # (after the exit ring and the debugger's block: 448 + 1024 + 128)

    # the report: one header, eight processor lines and the GIC's unhandled line, each short
    report = loader_out.split('---- report ----\n')[1].split('---- end ----\n')[0]
    lines = report.splitlines()
    assert len(lines) == 1 + 8 + 1, lines
    for line in lines:
        assert len(line) <= 130, (len(line), line)
    assert lines[1].startswith('CPU 0 pc ')
    assert lines[8].startswith('CPU 7 pc ')
    assert lines[9].startswith('GIC: unhandled ')

    # ... and a version 3 report adds the teardown to the processor lines and one line of exits
    report3 = loader_out.split('---- report3 ----\n')[1].split('---- end3 ----\n')[0]
    lines3 = report3.splitlines()
    assert len(lines3) == 1 + 8 + 1 + 1, lines3
    assert lines3[1].startswith('CPU 0 pc ') and 'tlbi' not in lines3[1]
    assert ' tlbi 2 ' in lines3[6]
    assert lines3[9].startswith('GIC: unhandled ')
    assert lines3[10].startswith('EXITS 3 (newest first): ')
    for line in lines3:
        assert len(line) <= 150, (len(line), line)

    # ... and the register dump: at most eight lines on the panel, the rest only in the log
    mmio = mmio_out.split('---- mmio ----\n')[1].split('---- end ----\n')[0]
    assert len(mmio.splitlines()) <= 8, mmio

print(kernel_out)
print(loader_out)
print(mmio_out)
print('test-cpu-pc: ok')
