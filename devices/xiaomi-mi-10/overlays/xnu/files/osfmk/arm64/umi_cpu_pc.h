/*
 * UMI: per-processor breadcrumbs -- where each processor was when the machine died.
 *
 * The hang of 2026-11-12 (Settings > Personal Hotspot) stops the whole kernel with no panic and
 * no line, between two ordinary radio register writes; from the MMIO ring alone, a processor
 * holding a lock and a processor that was asleep look the same. This table is one entry per
 * processor, written by that processor only:
 *
 *   - the interrupt entry path (sleh_interrupt_handler_prologue(), IRQ and FIQ both) writes the
 *     PC and LR it interrupted, the time, the thread, and the state;
 *   - machine_idle() writes the idle flag on the way in and takes it away on the way out, so a
 *     processor in WFI can be told from one that stopped;
 *   - lck_spin_lock() writes the caller and the lock of the spin it is about to take, and
 *     hw_lock_lock_contended() marks the entry when that spin has lasted longer than
 *     UMI_CPU_PC_SPIN_NS, once per episode;
 *   - gic_irq_handler() writes the INTID it has just acknowledged and is handling (version 2),
 *     and the table says which interrupt nobody owned, so that a report can tell a processor
 *     switched off in the middle of the same broadcast interrupt from one that simply stopped;
 *   - pmap_destroy_internal() and pmap_unmap_commpage() say, in three phases, how far the
 *     broadcast invalidation of a dying address space got -- before the "tlbi ...is", after it,
 *     and after the "dsb ish" that waits for every DVM participant to answer it -- with the pmap,
 *     its ASID and the process that owned it (version 3);
 *   - proc_exit() writes the last eight processes to exit (pid, name, time, processor) into a
 *     small ring of the same table, so a teardown seen in flight can be named (version 3);
 *   - the radio's bus (bsd/dev/hakios.lx.c, lx_os_mmio_begin(), from wifi/lx/src/lx_bus.c) marks
 *     the processor it is accessing the chip's registers from, with the ring entry's own number,
 *     so that the report can match a pending access to the processor that was making it -- and
 *     tell one that never came back from one that was finished just before the machine stopped
 *     (version 4; work/v29i/INVESTIGATION-hotspot-hang-v29z.md §2.4).
 *   - the debugger's cross-call: the first hook to enter the debugger on a processor writes which
 *     one it was (Debugger(), a kernel BRK, handle_debugger_trap(), the two ends of the SIGPdebug
 *     broadcast), when, on whose behalf, the exception and code, and the pc it came from; the table
 *     keeps the first and the newest of those, and each processor says whether it is the one that
 *     sent the signal or one that is still waiting for it. A machine stopped in the debugger parks
 *     every processor on SGI 0, writes no panic and no line, and looks from every other record like
 *     hardware that stopped -- this says which processor began it and from where (version 5;
 *     work/v29i/INVESTIGATION-hotspot-hang-v30.md §4.2);
 *   - the panic's own text, the pc that called panic() and the return addresses above it, written
 *     at the very top of panic_trap_to_debugger() -- before any lock, console or debugger -- and
 *     the stage the panic has reached, one marker per step of the way out (the debugger's lock, the
 *     other processors signalled, the platform's panic actions, the panic log, ... the return to
 *     the boot loader and each driver cut off on the way, psci_smp.c's waits included). Two panics
 *     in a row have written nothing to the console and nothing to a file on this port; the record
 *     says which panic it was and where it stopped even when the machine dies 200 ms later
 *     (version 6; work/v29i/INVESTIGATION-panic-xcall-hang.md §5.1).
 *
 * Every write is a plain store: to this processor's own entry, except the two words of the
 * unhandled-interrupt count in the table's first line. No locks, no atomics, no logging,
 * no MMIO. The only reader is the loader, on its own processor, once the kernel has been silent
 * for 50 s: it finds this table through loader_live.cpu_pc (a kernel VA; add the slide) and
 * prints it beside the MMIO ring (loader.c, log_of_hung_cpu_pc()).
 *
 * loader.c repeats struct lx_cpu_breadcrumbs and these constants; the two must change together,
 * and the table carries its version and size so the loader refuses one it does not know, exactly
 * as it does for the MMIO ring (struct lx_mmio_ring).
 */
#ifndef _ARM64_UMI_CPU_PC_H_
#define _ARM64_UMI_CPU_PC_H_

#include <stdint.h>
#include <kern/thread.h>

#define LX_CPU_PC_MAGIC         0x43425550434d584cULL   /* "LXMCPUBC" */
/* 1: pc/lr/sp/time, idle and lock state; 2: + the GIC INTIDs and the unhandled-interrupt counter
 * (a version 1 table has zeros in those fields); 3: + per-processor pmap-teardown phases (the
 * entry is 192 bytes) and the exit ring after the entries; 4: + the radio's bus marker in the
 * entry's last line (the size is still 192: it takes the two words that were padding); 5: + the
 * debugger's cross-call records after the exit ring (the entry is still 192: they are a block of
 * their own, so every version 4 offset stays where it was); 6: + the panic's record after those
 * (the same: its own block, and no offset above changes). */
#define LX_CPU_PC_VERSION       6
#define LX_CPU_PC_CORES         8       /* umi: MPIDR Aff1 0..7 (core << 8: 0x700 = cpu7; NOT Aff0, which is 0 on every core) */

/* How long a spin has to last before it is written down, and how often the clock is asked:
 * a contended lock asks every 64 rounds of the lock's own spin loop (~64 us on this phone). */
#define UMI_CPU_PC_SPIN_NS      100000ULL

/* state: what wrote the pc/lr of the entry last (the interrupt path, each time) */
#define LX_CPU_STATE_IRQ        0x1u    /* written by the interrupt path */
#define LX_CPU_STATE_USER       0x2u    /* ... which interrupted user code (the pc is not a kernel one) */
/* flags: a word the idle and lock paths own; the interrupt path never touches it */
#define LX_CPU_FLAG_IDLE        0x1u    /* the processor is inside machine_idle() */
#define LX_CPU_FLAG_SPIN        0x2u    /* ... and is in a spin past UMI_CPU_PC_SPIN_NS */
/* An INTID is 24 bits; this value is the "none" of the active half of entry->intids. */
#define LX_CPU_INTID_NONE       0xffffffffu

/*
 * How far a pmap teardown got on a processor (version 3, entry->pmap_phase; the fourth phase is
 * version 4). The invalidation of a dying address space is a broadcast "tlbi ...is" followed by a
 * "dsb ish" that cannot return until every DVM participant (the SMMUs' TCUs among them) has
 * answered it, and an "isb" after it; a processor that stops in the middle looks, from every other
 * record, exactly like one that stopped anywhere else. The phases put it between two of them: a
 * processor found at TLBI is one whose "tlbi" was issued and whose dsb had not returned, one at
 * DVM_DONE has the dsb's answer and was stopped before (or in) the isb (the shape of the v29z
 * hang, work/v29i/INVESTIGATION-hotspot-hang-v29z.md §2.3), and DONE is a teardown that ran out.
 */
#define LX_PMAP_PHASE_NONE      0u      /* no pmap teardown has run on this processor */
#define LX_PMAP_PHASE_ENTER     1u      /* about to issue the invalidations */
#define LX_PMAP_PHASE_TLBI      2u      /* ... issued; the dsb that waits for them has not returned */
#define LX_PMAP_PHASE_DVM_DONE  3u      /* ... and it returned: every DVM participant answered (version 4) */
#define LX_PMAP_PHASE_DONE      4u      /* ... and the isb after it has run */

/* The last exits kept, newest first, in the table's ring (version 3). */
#define LX_CPU_EXIT_RING        8

/*
 * The debugger entry a processor last took (version 5, struct lx_cpu_debugger). The first hook to
 * note an entry since the last one returned keeps it, and the hooks nest in the order they are
 * written here -- Debugger() over the BRK in sleh.c, the BRK over handle_debugger_trap(), and that
 * over the SIGPdebug broadcast -- so the kind on a stuck processor names the outermost way in.
 * The receiver's entry is not cleared until its wait returns, which is how a processor still parked
 * in the debugger when the machine stopped is told from one that came back.
 */
#define LX_DBG_KIND_NONE        0u      /* no debugger entry on this processor since the last one returned */
#define LX_DBG_KIND_TRAP        1u      /* handle_debugger_trap(), with nobody above it */
#define LX_DBG_KIND_DEBUGGER    2u      /* Debugger() / DebuggerWithContext() */
#define LX_DBG_KIND_XCALL_ENTER 3u      /* DebuggerXCallEnter(): this processor sent the SIGPdebugs */
#define LX_DBG_KIND_XCALL_WAIT  4u      /* DebuggerXCall(): a receiving end, in wait_while_mp_kdp_trap() */
#define LX_DBG_KIND_BRK         5u      /* a kernel BRK (sleh.c, DebuggerCall()) */
/* roles: what this processor is doing in the debugger, a bit each way (more than one only if a
 * nested entry did both on the same processor) */
#define LX_DBG_ROLE_INITIATOR   0x1u    /* the debugger was entered on it: it sent the signals */
#define LX_DBG_ROLE_RECEIVER    0x2u    /* ... it was signalled into it and is waiting for the sender */

/*
 * One entry per processor. The first 64 bytes are what the interrupt entry path writes; the
 * second hold what the idle and lock paths write, and the word at +120 which the GIC handler
 * updates for every interrupt it takes -- so the entry path itself never dirties the line those
 * paths are writing. The third (version 3) holds the last pmap teardown this processor ran.
 */
struct lx_cpu_breadcrumb {
	uint64_t pc;                    /* +0   the interrupted PC (ELR_EL1 as the vector saved it) */
	uint64_t lr;                    /* +8   ... and LR */
	uint64_t sp;                    /* +16  ... and SP */
	uint64_t when_ns;               /* +24  absolutetime_to_nanoseconds(), the MMIO ring's clock */
	uint64_t count;                 /* +32  captures on this processor since boot */
	uint64_t tid;                   /* +40  thread_tid() of the interrupted thread */
	uint64_t state;                 /* +48  LX_CPU_STATE_* */
	uint64_t mpidr;                 /* +56  the processor that wrote the entry */
	uint64_t idle_entries;          /* +64  machine_idle() entries since boot */
	uint64_t idle_since_ns;         /* +72  when the current idle began (0: never) */
	uint64_t lock_caller;           /* +80  the caller of the spin lock last taken or waited for */
	uint64_t lock;                  /* +88  ... and the lock itself */
	uint64_t spin_when_ns;          /* +96  when that wait first went past the threshold */
	uint64_t spins;                 /* +104 waits past the threshold since boot */
	uint64_t flags;                 /* +112 LX_CPU_FLAG_* */
	uint64_t intids;                /* +120 the last INTID acknowledged, [31:0]; the one being handled now,
	                                 *      [63:32] (LX_CPU_INTID_NONE: none). Written by the GIC handler only;
	                                 *      one store of both halves per interrupt, one more when it is ended. */
	/* (version 3: the pmap teardown this processor last began, pmap_destroy_internal()) */
	uint64_t pmap_phase;            /* +128 LX_PMAP_PHASE_* (0: no teardown on this processor yet) */
	uint64_t pmap_when_ns;          /* +136 when that phase was written (the MMIO ring's clock) */
	uint64_t pmap;                  /* +144 the pmap being torn down */
	uint64_t pmap_ids;              /* +152 [15:0] its hw_asid, [31:16] the pid of the process that owned it */
	char     pmap_comm[16];         /* +160 ... and the first 16 bytes of that process's name, NUL-padded */
	/* (version 4: the radio's bus, while it is inside an access to the chip's registers) */
	uint64_t mmio_since_ns;         /* +176 when that access began (lx_os_nanoseconds(); 0: not inside one) */
	uint64_t mmio_seq;              /* +184 its number in the MMIO ring (1, 2, 3, ...; 0: not inside one) */
} __attribute__((aligned(64)));

/* One process that has exited (version 3), written by proc_exit() on the processor that ran it. */
struct lx_proc_exit {
	uint64_t seq;                   /* +0  the exit's number since boot (0: this slot was never written) */
	uint64_t when_ns;               /* +8  proc_exit()'s clock */
	uint64_t pid;                   /* +16 the process */
	uint64_t cpu;                   /* +24 ... and the processor that ran its exit */
	char     comm[16];              /* +32 its p_comm, NUL-padded */
};                                      /* 48 bytes */

/*
 * The ring those exits go into: a counter and the eight newest of them, after the per-processor
 * entries (version 3). A writer takes the next number from the counter and fills that slot; a
 * slot's seq says both that it was written and how old it is, so the reader needs no lock either.
 */
struct lx_exit_ring {
	uint64_t seq;                   /* +0  exits recorded since boot */
	uint64_t reserved[7];           /* +8  (the rest of the line) */
	struct lx_proc_exit exit[LX_CPU_EXIT_RING];     /* +64 */
};

/*
 * One processor's last debugger entry (version 5), after the exit ring. Written by that processor
 * only, with two exceptions the hooks make plain: the table's globals below, which the processor
 * that opens an entry updates, and nothing else. The entry is not in struct lx_cpu_breadcrumb on
 * purpose: the entry's 192 bytes and every offset in a version 4 table stay where they were, and a
 * reader that does not know this block simply never looks for it.
 */
struct lx_cpu_debugger {
	uint64_t kind;                  /* +0  LX_DBG_KIND_*; 0: this processor is not in one and has not been */
	uint64_t since_ns;              /* +8  when that entry was noted (a receiver: still waiting since then) */
	uint64_t roles;                 /* +16 LX_DBG_ROLE_*: it sent the signals, or is waiting for them */
	uint64_t exc_code;              /* +24 [31:0] the exception, [63:32] its code (handle_debugger_trap()) */
	uint64_t elr;                   /* +32 the pc (or the caller) the entry was made from */
	uint64_t extra;                 /* +40 the BRK's instruction, or the trap's subcode */
	uint64_t xcall_func;            /* +48 the last broadcast xcall this processor was about to run ... */
	uint64_t xcall_arg;             /* +56 ... and its argument ... */
	uint64_t xcall_when_ns;         /* +64 ... and when (the same clock as everything else here) */
	uint64_t reserved[7];           /* +72 (the rest of the line) */
};                                      /* 128 bytes */

/* (version 5) The first and the newest debugger entry of the table, after the per-processor ones:
 * how many there have been, and which processor each was on, which hook, with what exception and
 * code, from which pc, and when. Plain stores, and a reader that catches one entry half written
 * still reads a whole entry before it. */
struct lx_dbg_globals {
	uint64_t count;                 /* +0  debugger entries since boot */
	uint64_t first_cpu;             /* +8  the processor the first of them was noted on ... */
	uint64_t first_kind;            /* +16 ... which hook it was ... */
	uint64_t first_exc_code;        /* +24 ... its exception and code ... */
	uint64_t first_elr;             /* +32 ... the pc it was made from ... */
	uint64_t first_when_ns;         /* +40 ... and when */
	uint64_t last_cpu;              /* +48 the same for the newest of them */
	uint64_t last_kind;             /* +56 */
	uint64_t last_exc_code;         /* +64 */
	uint64_t last_elr;              /* +72 */
	uint64_t last_when_ns;          /* +80 */
	uint64_t reserved[5];           /* +88 (the rest of the line) */
};                                      /* 128 bytes */

/*
 * The panic, said once at the top of panic_trap_to_debugger() and again at each step of the way out
 * (version 6, struct lx_panic_record). Two panics in a row on this port left no panic string at
 * all: the only line the kernel writes is the caller's address, into a log the loader does not put
 * on the screen, and the machine stopped before anything else was said. The message here is
 * formatted at the panic's first instruction, before any lock, console or debugger, and the stages
 * after it say how far the panic got -- the debugger's lock, the other processors signalled, the
 * platform's panic actions, the panic log, the return to the boot loader and each driver cut off on
 * the way. Plain stores, one clock read per stage; the loader reads it from another processor.
 *
 * A reader trusts the message once msg_len is non-zero (it is the last store of the message), and
 * the record at all once magic is LX_PANIC_MAGIC. The first panic of a boot keeps the message: a
 * second one is already on its way to the same wall, and the reason for the first is the one worth
 * reading.
 */
#define LX_PANIC_MSG            96      /* (the message, NUL-terminated and truncated to this) */
#define LX_PANIC_FPS            8       /* (return addresses kept: the frame chain above panic()) */
#define LX_PANIC_MAGIC          0x43494e41504d584cULL   /* "LXMPANIC" */

/* Which step of the way out the panic reached last (record->stage): the name is the loader's. */
#define LX_PANIC_STAGE_NONE             0u      /* no panic has reached this table */
#define LX_PANIC_STAGE_ENTRY            1u      /* panic_trap_to_debugger(): the message is written */
#define LX_PANIC_STAGE_LOCK             2u      /* handle_debugger_trap(): the debugger's lock is held */
#define LX_PANIC_STAGE_HALTED           3u      /* ... and the other processors were signalled (see below) */
#define LX_PANIC_STAGE_KDP_ENTER        4u      /* kdp_callouts(KDP_EVENT_ENTER) has returned */
#define LX_PANIC_STAGE_PANIC_BEGIN      5u      /* the platform's panic actions (kPEPanicBegin) are about to run */
#define LX_PANIC_STAGE_SAVEINFO         6u      /* SavePanicInfo(): the backtraces and the panic stackshot */
#define LX_PANIC_STAGE_DIAG_DONE        7u      /* the diagnostics are done (kPEPanicDiagnosticsDone) */
#define LX_PANIC_STAGE_REBOOT           8u      /* kdp_machine_reboot_type(): loader_return() is next */
#define LX_PANIC_STAGE_STOP_OTHERS      9u      /* loader_return(): psci_smp_stop_others() */
#define LX_PANIC_STAGE_QUIESCE_UFS      10u     /* ... and the driver cut off before the next one ... */
#define LX_PANIC_STAGE_QUIESCE_ADRENO   11u
#define LX_PANIC_STAGE_QUIESCE_WIFI     12u
#define LX_PANIC_STAGE_QUIESCE_ADSP     13u
#define LX_PANIC_STAGE_QUIESCE_MPSS     14u
#define LX_PANIC_STAGE_QUIESCE_DDR      15u
#define LX_PANIC_STAGE_QUIESCE_HAPTIC   16u
#define LX_PANIC_STAGE_QUIESCE_TORCH    17u
#define LX_PANIC_STAGE_QUIESCE_CAMERA   18u
#define LX_PANIC_STAGE_QUIESCE_MIC      19u
#define LX_PANIC_STAGE_JUMP             20u     /* loader_jump(): the way back itself */
#define LX_PANIC_STAGE_PSCI_BOOT_WANTED 21u     /* psci_smp.c: a processor that is not the boot one hands the return over */
#define LX_PANIC_STAGE_PSCI_MASTER_WAIT 22u     /* ... and waits for the boot processor to answer */
#define LX_PANIC_STAGE_PSCI_STOP_REST   23u     /* every other processor is told to switch itself off */
#define LX_PANIC_STAGE_PSCI_WAIT_CPU    24u     /* ... and each of them is waited for */
#define LX_PANIC_STAGE_PSCI_AFFINITY    25u     /* PSCI_AFFINITY_INFO: the firmware is asked whether one has gone */
#define LX_PANIC_STAGE_PSCI_CPU_OFF     26u     /* PSCI_CPU_OFF: this processor is switching itself off */
#define LX_PANIC_STAGE_PARKED           27u     /* the panic's fast path: this processor stopped here on purpose */
#define LX_PANIC_STAGE_LAST             LX_PANIC_STAGE_PARKED

struct lx_panic_record {
	uint64_t magic;                 /* +0  LX_PANIC_MAGIC once there has been a panic */
	uint64_t cpu;                   /* +8  the processor panic_trap_to_debugger() first ran on */
	uint64_t caller;                /* +16 panic()'s caller (the pc the panic is blamed on) */
	uint64_t when_ns;               /* +24 when the panic was entered (the table's clock) */
	uint64_t stage;                 /* +32 LX_PANIC_STAGE_*: the last step it reached ... */
	uint64_t stage_ns;              /* +40 ... and when */
	uint64_t fps[LX_PANIC_FPS];     /* +48 the return addresses above the caller (the frame chain) */
	uint64_t xcall_sync;            /* +112 debugger_sync as DebuggerXCallEnter() left it (0: every signalled processor answered) */
	uint64_t xcall_sent;            /* +120 how many processors it signalled (0: the cross-call did not run) */
	uint64_t xcall_pending;         /* +128 a bit per processor still carrying SIGPdebug: it did not answer */
	uint64_t ack_timeouts;          /* +136 debug_ack_timeout_count when it ended (0: it did not time out) */
	uint32_t msg_len;               /* +144 the message's length; written last, after msg */
	uint32_t stage_cpu;             /* +148 the processor that last wrote stage */
	char     msg[LX_PANIC_MSG];     /* +152 the panic's text, formatted at its first instruction */
} __attribute__((aligned(64)));         /* 256 bytes */

struct lx_cpu_breadcrumbs {
	uint64_t magic;
	uint32_t version;
	uint32_t cores;
	uint32_t size;                  /* sizeof(struct lx_cpu_breadcrumb) */
	uint32_t unused;
	uint64_t gic_last_unhandled;    /* +24 the last INTID nobody owned (gic_irq_handler()'s else branch) */
	uint64_t gic_unhandled_count;   /* +32 ... and how many there have been since boot */
	uint64_t reserved[3];           /* +40 (the rest of the first line; core[0] starts at +64) */
	struct lx_cpu_breadcrumb core[LX_CPU_PC_CORES];
	struct lx_exit_ring exits;      /* (version 3; after the entries, whose size is `size`) */
	struct lx_cpu_debugger debug[LX_CPU_PC_CORES];  /* (version 5; after the exit ring) */
	struct lx_dbg_globals dbg;      /* (version 5) */
	struct lx_panic_record panic;   /* (version 6; the last of them) */
} __attribute__((aligned(64)));

/* The table (osfmk/arm64/umi_cpu_pc.inc, included once by loader_return.c, which also offers it
 * to the loader) and the clock it uses. */
extern struct lx_cpu_breadcrumbs umi_cpu_pc_table;
uint64_t umi_cpu_pc_now_ns(void);

/* The same two entry points, callable from outside osfmk/arm64 (bsd/dev/hakios.lx.c, which the
 * radio's bus calls): bsd/ does not get this directory's include path, so it declares these by
 * hand, exactly as it does umi_cpu_pc_record_exit() through pmap.c. */
void umi_cpu_pc_mmio_begin(uint64_t seq, uint64_t when_ns);
void umi_cpu_pc_mmio_end(uint64_t seq);

/* This processor's entry. MPIDR Aff1 (bits 15:8) is the processor's number on this SoC (0x700 for the last
 * of the eight); a value past the table is put on the last entry rather than written past it. */
static inline struct lx_cpu_breadcrumb *
umi_cpu_pc_self(void)
{
	uint32_t index = (uint32_t)((__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff);
	return &umi_cpu_pc_table.core[index < LX_CPU_PC_CORES ? index : LX_CPU_PC_CORES - 1];
}

/* This processor's debugger record (version 5), found the same way as its entry above. */
static inline struct lx_cpu_debugger *
umi_cpu_pc_debug_self(void)
{
	uint32_t index = (uint32_t)((__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff);
	return &umi_cpu_pc_table.debug[index < LX_CPU_PC_CORES ? index : LX_CPU_PC_CORES - 1];
}

/*
 * The interrupt entry path, for every interrupt on every processor: where it interrupted, when,
 * and whose thread. Eight stores and one clock read; nothing else.
 */
static inline void
umi_cpu_pc_note_irq(uint64_t pc, uint64_t lr, uint64_t sp, int is_user)
{
	struct lx_cpu_breadcrumb *entry = umi_cpu_pc_self();

	entry->pc = pc;
	entry->lr = lr;
	entry->sp = sp;
	entry->when_ns = current_thread() != NULL ? umi_cpu_pc_now_ns() : 0;      /* (the clock goes through the thread's CpuDatap) */
	entry->tid = thread_tid(current_thread());
	entry->state = LX_CPU_STATE_IRQ | (is_user ? LX_CPU_STATE_USER : 0);
	entry->mpidr = (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	entry->count++;
}

/* machine_idle() entry: the processor may now sit in WFI; said with three stores so that a core
 * that is asleep at the moment the machine stops is not mistaken for one that stopped. */
static inline void
umi_cpu_pc_note_idle_enter(void)
{
	struct lx_cpu_breadcrumb *entry = umi_cpu_pc_self();

	entry->idle_since_ns = umi_cpu_pc_now_ns();
	entry->idle_entries++;
	entry->flags |= LX_CPU_FLAG_IDLE;
}

static inline void
umi_cpu_pc_note_idle_exit(void)
{
	struct lx_cpu_breadcrumb *entry = umi_cpu_pc_self();

	entry->flags &= ~(uint64_t)LX_CPU_FLAG_IDLE;
}

/*
 * A spin lock is about to be taken (lck_spin_lock() and its three siblings): remember who asked
 * and for which lock, so that hw_lock_lock_contended() can say it if the wait turns long. Two
 * stores on every acquisition, contended or not -- cheaper than the atomic the lock itself does.
 */
static inline void
umi_cpu_pc_note_lock(uint64_t caller, uint64_t lock)
{
	struct lx_cpu_breadcrumb *entry = umi_cpu_pc_self();

	entry->lock_caller = caller;
	entry->lock = lock;
}

/* The wait on entry->lock passed UMI_CPU_PC_SPIN_NS: said once per episode (the caller and lock
 * are already in the entry). */
static inline void
umi_cpu_pc_note_spin(void)
{
	struct lx_cpu_breadcrumb *entry = umi_cpu_pc_self();

	entry->spin_when_ns = umi_cpu_pc_now_ns();
	entry->spins++;
	entry->flags |= LX_CPU_FLAG_SPIN;
}

/* ... and it was taken or given up: no longer spinning. */
static inline void
umi_cpu_pc_note_spin_done(void)
{
	struct lx_cpu_breadcrumb *entry = umi_cpu_pc_self();

	entry->flags &= ~(uint64_t)LX_CPU_FLAG_SPIN;
}

/*
 * The GIC handler has just acknowledged an interrupt and is about to handle it: one store writes
 * both halves of entry->intids (last acknowledged in [31:0], in flight in [63:32]). The dispatch
 * loop handles one INTID at a time with interrupts masked, so the outer one cannot be overwritten.
 */
static inline void
umi_cpu_pc_note_intid(uint32_t intid)
{
	struct lx_cpu_breadcrumb *entry = umi_cpu_pc_self();

	entry->intids = ((uint64_t)intid << 32) | intid;
}

/* ... and it has been ended (EOI): nothing is in flight, the last INTID stays for the report. */
static inline void
umi_cpu_pc_note_intid_done(void)
{
	struct lx_cpu_breadcrumb *entry = umi_cpu_pc_self();

	entry->intids = (entry->intids & 0xffffffffULL) | ((uint64_t)LX_CPU_INTID_NONE << 32);
}

/* The two halves, for a reader (the loader's copy of the structure is the same bytes). */
static inline uint32_t
umi_cpu_pc_intid_last(const struct lx_cpu_breadcrumb *entry)
{
	return (uint32_t)entry->intids;
}

static inline uint32_t
umi_cpu_pc_intid_active(const struct lx_cpu_breadcrumb *entry)
{
	return (uint32_t)(entry->intids >> 32);
}

/*
 * An SPI that nobody had attached fired: the GIC masks it, so it does not storm, and the report
 * has to name it (a source that keeps firing unnoticed looks, from the breadcrumbs alone, like a
 * processor that simply stopped). Written from every processor; the count and the INTID are two
 * plain stores and either order still says what fired last.
 */
static inline void
umi_cpu_pc_note_unhandled(uint32_t intid)
{
	umi_cpu_pc_table.gic_last_unhandled = intid;
	umi_cpu_pc_table.gic_unhandled_count++;
}

/*
 * A pmap teardown, said three times: before the invalidations of a dying address space, after the
 * "tlbi ...is" that issues them, and after the "dsb ish" that has to wait for every DVM participant
 * to answer. A processor that stops between the second and the third is a processor waiting for the
 * interconnect, and the entry names the pmap, its ASID and the process that owned it.
 *
 * Four stores and a clock read; no lock, no log line. `comm` is at most 16 bytes (p_comm is 17
 * with its NUL) and may be NULL for a kernel pmap. The phase is written last so that a reader
 * which catches the entry mid-write still sees the time that goes with the phase it read.
 */
static inline void
umi_cpu_pc_note_pmap(uint32_t phase, uint64_t pmap, uint32_t asid, uint32_t pid, const char *comm)
{
	struct lx_cpu_breadcrumb *entry = umi_cpu_pc_self();
	int ended = comm == NULL;

	for (unsigned int i = 0; i < (unsigned int)sizeof(entry->pmap_comm); i++) {
		entry->pmap_comm[i] = ended ? '\0' : comm[i];
		ended = ended || comm[i] == '\0';
	}
	entry->pmap = pmap;
	entry->pmap_ids = ((uint64_t)pid << 16) | (asid & 0xffffu);
	entry->pmap_when_ns = umi_cpu_pc_now_ns();
	entry->pmap_phase = phase;
}

/*
 * A process has exited (proc_exit()): the eight newest of them are kept in the table's ring, so
 * that a pmap teardown seen in flight can be matched to the process it belonged to. A processor
 * takes the next number from the ring's counter and fills that slot; the slot's own seq is written
 * last, which is what makes it readable. A lost race costs one entry's number, never a torn one.
 */
static inline void
umi_cpu_pc_note_exit(uint64_t pid, const char *comm)
{
	struct lx_cpu_breadcrumbs *table = &umi_cpu_pc_table;
	const uint64_t seq = __atomic_add_fetch(&table->exits.seq, 1, __ATOMIC_RELAXED);
	struct lx_proc_exit *entry = &table->exits.exit[seq % LX_CPU_EXIT_RING];
	int ended = comm == NULL;

	for (unsigned int i = 0; i < (unsigned int)sizeof(entry->comm); i++) {
		entry->comm[i] = ended ? '\0' : comm[i];
		ended = ended || comm[i] == '\0';
	}
	entry->when_ns = umi_cpu_pc_now_ns();
	entry->pid = pid;
	entry->cpu = (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
	__atomic_store_n(&entry->seq, seq, __ATOMIC_RELEASE);
}

/*
 * The radio's bus is inside one access to the chip's registers on this processor (version 4):
 * wifi/lx/src/lx_bus.c sets this before the access is made and clears it once it has come back,
 * through bsd/dev/hakios.lx.c (lx_os_mmio_begin()). The number is the access's own entry in the
 * MMIO ring (which begins at 1), so a report can put the ring's pending entry beside the
 * processor that was making it; the time is the access's start, on the ring's clock, so its age
 * can be read against everything else. A processor still marked when the machine stopped is one
 * the access never came back to -- and one not marked, whose ring entry is the pending one, says
 * that entry was finished and the machine stopped elsewhere (INVESTIGATION-hotspot-hang-v29z §2.4).
 *
 * Two stores, no lock, no clock read (the caller already has the time); the number is written
 * last, so a reader that sees one sees its time.
 */
static inline void
umi_cpu_pc_note_mmio(uint64_t seq, uint64_t when_ns)
{
	struct lx_cpu_breadcrumb *entry = umi_cpu_pc_self();

	entry->mmio_since_ns = when_ns;
	entry->mmio_seq = seq;
}

/* ... and it has come back. Only this access's mark is this one's to clear: the thread that made
 * it may have moved to another processor in between, whose mark then belongs to another access. */
static inline void
umi_cpu_pc_note_mmio_done(uint64_t seq)
{
	struct lx_cpu_breadcrumb *entry = umi_cpu_pc_self();

	if (entry->mmio_seq == seq) {
		entry->mmio_seq = 0;
		entry->mmio_since_ns = 0;
	}
}

/*
 * The debugger was entered on this processor (version 5), through the hook named by `kind`
 * (osfmk/kern/debug.c, osfmk/arm/model_dep.c, osfmk/arm64/sleh.c). The first hook to say so since
 * the last return keeps the entry -- Debugger() has noted itself before the BRK under it, and the
 * BRK before handle_debugger_trap() -- and the table's first and newest entries are written with
 * it. The clock is read only for a new entry, not for every hook of the same one; the caller's
 * role is added every time, so a processor that was signalled into an entry another began says so.
 * No lock, no log line: a machine stopped here can only be read from another processor.
 */
static inline void
umi_cpu_pc_note_debugger(uint32_t kind, uint32_t role, uint64_t elr, uint64_t extra)
{
	struct lx_cpu_debugger *dbg = umi_cpu_pc_debug_self();
	struct lx_dbg_globals *globals = &umi_cpu_pc_table.dbg;

	if (dbg->kind == LX_DBG_KIND_NONE) {
		const uint64_t when = umi_cpu_pc_now_ns();
		const uint64_t cpu = (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;
		const uint64_t count = globals->count + 1;

		/* (the entry first, the table's record after it, and the count last) */
		dbg->elr = elr;
		dbg->extra = extra;
		dbg->exc_code = 0;
		dbg->roles = 0;
		dbg->since_ns = when;
		dbg->kind = kind;
		if (globals->count == 0) {
			globals->first_cpu = cpu;
			globals->first_kind = kind;
			globals->first_exc_code = 0;
			globals->first_elr = elr;
			globals->first_when_ns = when;
		}
		globals->last_cpu = cpu;
		globals->last_kind = kind;
		globals->last_exc_code = 0;
		globals->last_elr = elr;
		globals->last_when_ns = when;
		globals->count = count;
	}
	dbg->roles |= role;
}

/*
 * ... and why it was entered: handle_debugger_trap() is what every entry of the kernel's own
 * debugger path passes through, and it is the one place with the exception and its code. `elr` is
 * 0 where the caller has no saved state to read a pc from -- the hook above already gave it.
 */
static inline void
umi_cpu_pc_note_debugger_why(uint32_t exception, uint32_t code, uint32_t subcode, uint64_t elr)
{
	struct lx_cpu_debugger *dbg = umi_cpu_pc_debug_self();

	if (dbg->kind == LX_DBG_KIND_NONE) {
		umi_cpu_pc_note_debugger(LX_DBG_KIND_TRAP, 0, elr, 0);
	}
	if (dbg->elr == 0) {
		dbg->elr = elr;
	}
	dbg->exc_code = ((uint64_t)code << 32) | exception;
	if (subcode != 0) {
		dbg->extra = subcode;
	}
	umi_cpu_pc_table.dbg.last_exc_code = dbg->exc_code;
}

/* ... and it has returned: this processor is no longer in one. The table's own first and newest
 * stay, so a debugger that was entered and left is still on the report. */
static inline void
umi_cpu_pc_note_debugger_done(uint32_t role)
{
	struct lx_cpu_debugger *dbg = umi_cpu_pc_debug_self();

	dbg->roles &= ~(uint64_t)role;
	if (dbg->roles == 0) {
		dbg->kind = LX_DBG_KIND_NONE;           /* (first, so a reader never pairs it with a half-cleared entry) */
		dbg->exc_code = 0;
		dbg->extra = 0;
		dbg->elr = 0;
		dbg->since_ns = 0;
	}
}

/*
 * The broadcast xcall this processor is about to run (version 5): cpu_handle_xcall() writes the
 * function and its argument before calling it, on every processor, so that a machine stopped with
 * every processor on SGI 0 can also say which broadcast it was -- the second answer to the same
 * fingerprint as the debugger's, when no processor entered one (H3 of the v30 investigation).
 * Three stores and a clock read, once per xcall.
 */
static inline void
umi_cpu_pc_note_xcall(uint64_t func, uint64_t arg)
{
	struct lx_cpu_debugger *dbg = umi_cpu_pc_debug_self();

	dbg->xcall_func = func;
	dbg->xcall_arg = arg;
	dbg->xcall_when_ns = umi_cpu_pc_now_ns();
}

/*
 * The panic (version 6): its text, the pc it is blamed on and the return addresses above it,
 * written at the very top of panic_trap_to_debugger() -- before anything that can wait on a lock,
 * on another processor or on the firmware, and before the console, which on this port never shows
 * a panic's text. Every store is plain and the clock is asked once. `len` bytes of `msg` are kept
 * (the caller truncates to LX_PANIC_MSG); msg_len is the last store, so a reader that finds it
 * whole has the whole message. A second panic keeps the first one's message (see above).
 */
static inline void
umi_cpu_pc_note_panic(uint64_t caller, uint32_t len, const char *msg)
{
	struct lx_panic_record *record = &umi_cpu_pc_table.panic;
	const uint64_t cpu = (__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff;

	if (record->magic == LX_PANIC_MAGIC) {
		return;                 /* (the first panic keeps the message) */
	}
	record->msg_len = 0;
	record->cpu = cpu;
	record->caller = caller;
	record->when_ns = umi_cpu_pc_now_ns();
	record->stage = LX_PANIC_STAGE_NONE;
	record->stage_ns = 0;
	record->stage_cpu = (uint32_t)cpu;
	for (unsigned int i = 0; i < (unsigned int)(sizeof(record->fps) / sizeof(record->fps[0])); i++) {
		record->fps[i] = 0;
	}
	record->xcall_sync = 0;
	record->xcall_sent = 0;
	record->xcall_pending = 0;
	record->ack_timeouts = 0;
	for (unsigned int i = 0; i < (unsigned int)sizeof(record->msg); i++) {
		record->msg[i] = i < len ? msg[i] : '\0';
	}
	if (len >= (uint32_t)sizeof(record->msg)) {
		len = (uint32_t)sizeof(record->msg) - 1;
		record->msg[len] = '\0';
	}
	record->magic = LX_PANIC_MAGIC;
	record->msg_len = len;
}

/* The return addresses above the caller, in the order the frame chain gives them (up to
 * LX_PANIC_FPS of them; the rest of the array is cleared, so a stale one is never read). */
static inline void
umi_cpu_pc_note_panic_bt(const uint64_t *pcs, unsigned int n)
{
	struct lx_panic_record *record = &umi_cpu_pc_table.panic;

	for (unsigned int i = 0; i < (unsigned int)(sizeof(record->fps) / sizeof(record->fps[0])); i++) {
		record->fps[i] = i < n ? pcs[i] : 0;
	}
}

/* ... and the step of the way out the panic has reached: the time first, the stage last, so a
 * reader that sees a stage sees the time that goes with it. */
static inline void
umi_cpu_pc_note_panic_stage(uint32_t stage)
{
	struct lx_panic_record *record = &umi_cpu_pc_table.panic;

	record->stage_ns = umi_cpu_pc_now_ns();
	record->stage_cpu = (uint32_t)((__builtin_arm_rsr64("MPIDR_EL1") >> 8) & 0xff);
	record->stage = stage;
}

/* ... and how the debugger's cross-call ended (DebuggerXCallEnter(), model_dep.c): its final count
 * of processors still to answer, how many it signalled, which of them never did, and whether it
 * timed out. Written from the processor that ran the cross-call; no reader takes a lock. */
static inline void
umi_cpu_pc_note_panic_xcall(uint64_t sync, uint64_t sent, uint64_t pending, uint64_t timeouts)
{
	struct lx_panic_record *record = &umi_cpu_pc_table.panic;

	if (record->magic != LX_PANIC_MAGIC) {
		return;                 /* (no panic: nothing to account for) */
	}
	record->xcall_sync = sync;
	record->xcall_sent = sent;
	record->xcall_pending = pending;
	record->ack_timeouts = timeouts;
}

/*
 * The panic's last step on its fast path (version 6, stage PARKED): the record says so and this
 * processor stops here, with every interrupt masked and nothing left for another processor to
 * wait on. The kernel does not take the way back to the boot loader -- the return through the
 * drivers, the firmware and the other processors is what took the panics before this one
 * (IMPL-panic-record-v6.md, "v34"). The loader watches from its own processor; what it finds is
 * a kernel that stopped showing signs of life, and the panic's lines in this table are what its
 * report prints. A wakeup event cannot make this loop leave: it has no exit.
 */
static inline void __attribute__((noreturn))
umi_cpu_pc_panic_park(void)
{
	umi_cpu_pc_note_panic_stage(LX_PANIC_STAGE_PARKED);
	/*
	 * The record is in this processor's cache and the loader reads it from another one: clean
	 * its lines to the point of coherency first, as loader_jump() does for the log. The table is
	 * kernel data, so none of these can fault.
	 */
	for (uintptr_t p = (uintptr_t)&umi_cpu_pc_table.panic & ~(uintptr_t)63;
	    p < (uintptr_t)&umi_cpu_pc_table.panic + sizeof(umi_cpu_pc_table.panic); p += 64) {
		__asm__ volatile ("dc cvac, %0" : : "r" (p) : "memory");
	}
	__asm__ volatile ("msr daifset, #0xf\n\tdsb sy" ::: "memory");
	for (;;) {
		__builtin_arm_wfe();
	}
}

/* Is there a panic in this table at all? (the loader's sign of a machine that will say nothing
 * more, and what stops the kernel's own heartbeat: loader_return.c, hv_wdt_pet()). */
boolean_t umi_cpu_pc_panicked(void);

/*
 * Is this machine on a panic's way out, with the other processors already dealt with -- either
 * signalled and parked in the debugger's cross-call, or not to be had at all (the cross-call timed
 * out; on this port nothing can stop them: there is no debug wrapper)? Then the steps that wait on
 * another processor's lock, or for the firmware to answer for a modem or a DSP that may be gone,
 * are skipped -- they are what took the last two panics with them
 * (work/v29i/INVESTIGATION-panic-xcall-hang.md §5.2-5, §5.2-6).
 *
 * Defined with the table (umi_cpu_pc.inc), because the kernel's panic path and the platform's panic
 * actions both ask it. It is FALSE before the cross-call has run: the panic must reach it first.
 */
boolean_t umi_cpu_pc_panic_fast(void);

#endif /* _ARM64_UMI_CPU_PC_H_ */
