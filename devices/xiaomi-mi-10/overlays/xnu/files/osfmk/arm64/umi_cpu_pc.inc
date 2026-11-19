/*
 * UMI: the table of per-processor breadcrumbs (osfmk/arm64/umi_cpu_pc.h describes it and the
 * entry points that fill it in). Included once, by loader_return.c, which also offers the table
 * to the loader: a loader watching from another processor reads it after this kernel has been
 * silent for 50 s (loader.c, log_of_hung_cpu_pc()).
 *
 * The table is the kernel's: plain data, written entry by entry by each processor's own code,
 * never by a lock. loader_live.cpu_pc is its virtual address, and this kernel is loaded at its
 * link address, so the loader's slide (physical - virtual) takes it to the physical memory the
 * loader can read.
 */
#include <kern/clock.h>
#include <arm64/umi_cpu_pc.h>

struct lx_cpu_breadcrumbs umi_cpu_pc_table = {
	.magic = LX_CPU_PC_MAGIC,
	.version = LX_CPU_PC_VERSION,
	.cores = LX_CPU_PC_CORES,
	.size = sizeof(struct lx_cpu_breadcrumb),
};

/* The clock the MMIO ring uses (bsd/dev/hakios.lx.c, lx_os_nanoseconds()): the one place the two
 * records can be compared, and the reason a breadcrumb's age can be read against a ring entry. */
uint64_t
umi_cpu_pc_now_ns(void)
{
	uint64_t nanoseconds;

	absolutetime_to_nanoseconds(mach_absolute_time(), &nanoseconds);
	return nanoseconds;
}

/* (see umi_cpu_pc.h: the radio's bus marks the processor it is reaching the chip from, so that the
 * loader's report can say which processor an unfinished ring entry belongs to) */
void
umi_cpu_pc_mmio_begin(uint64_t seq, uint64_t when_ns)
{
	umi_cpu_pc_note_mmio(seq, when_ns);
}

void
umi_cpu_pc_mmio_end(uint64_t seq)
{
	umi_cpu_pc_note_mmio_done(seq);
}

/*
 * Whether a panic has been written into this table at all (version 6). Read from the panic path
 * and from the timer that shows the loader a sign of life (loader_return.c, hv_wdt_pet()): once
 * there is a panic, that sign stops, so a watching loader is not told a dying kernel is alive by
 * a processor the panic could not stop.
 */
boolean_t
umi_cpu_pc_panicked(void)
{
	return umi_cpu_pc_table.panic.magic == LX_PANIC_MAGIC;
}

/*
 * Whether the panic may skip the steps that wait on other processors and on the firmware (see
 * umi_cpu_pc.h): there is a panic in this table, and its debugger cross-call either timed out
 * (some processor never answered) or reached every one it signalled (all of them are parked in
 * the debugger). Before the cross-call has run the record says sent 0, so the answer is no: a
 * panic must get that far first.
 */
boolean_t
umi_cpu_pc_panic_fast(void)
{
	const struct lx_panic_record *record = &umi_cpu_pc_table.panic;

	if (record->magic != LX_PANIC_MAGIC) {
		return FALSE;
	}
	if (record->ack_timeouts != 0) {
		return TRUE;
	}
	return record->xcall_sent != 0 && record->xcall_sync == 0;
}
