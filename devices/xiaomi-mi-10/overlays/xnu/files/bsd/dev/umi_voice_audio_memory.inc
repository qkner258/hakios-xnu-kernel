/* SPDX-License-Identifier: BSD-2-Clause
 * Included by hakios.sound.c after shared_put(). Pin the global mailbox and
 * audio domain until reboot: VSS_IPKTEXG has no unset command in voice_mhi.c.
 * It must never alias playback or capture memory, or be freed under the DSP.
 */
#include "umi_voice_audio.h"
#define UVA_AUDIO_OFFSET 0x20000u /* beyond 32 KiB playback + 64 KiB capture */
static vm_offset_t uva_memory;
static bool uva_memory_mapped;

int
umi_voice_audio_memory(uint64_t *physical, uint64_t *adsp)
{
	int error = 0;
	lck_mtx_lock(&hakios_smmu_lock);
	if (!uva_memory_mapped) {
		if (!uva_memory && kmem_alloc_contig(kernel_map, &uva_memory, UVA_MAILBOX_BYTES, PAGE_MASK,
		    (ppnum_t)(0xffffffffull >> PAGE_SHIFT), 0, KMA_KOBJECT | KMA_ZERO | KMA_DATA,
		    VM_KERN_MEMORY_IOKIT) != KERN_SUCCESS) {
			error = ENOMEM;
			goto out;
		}
		if (!shared_get(2)) {
			error = ENXIO;
			goto out;
		}
		/* Both DMA domains use noncached pages. Clean the initial zeroes;
		 * the AP never reads or writes the live mailbox or assumes a PCM ABI. */
		for (vm_offset_t line = uva_memory; line < uva_memory + UVA_MAILBOX_BYTES; line += 64)
			__asm__ volatile("dc civac, %0" :: "r"(line) : "memory");
		__asm__ volatile("dsb sy" ::: "memory");
		if (!space.passed) {
			uint64_t *third = (uint64_t *)(space.tables + 8192);
			for (unsigned page = 0; page < UVA_MAILBOX_BYTES / 4096; page++)
				third[((AUDIO_ADDRESSES + UVA_AUDIO_OFFSET) >> 12 & 511) + page] =
				    (kvtophys(uva_memory) + page * 4096) | AUDIO_PTE;
			for (vm_offset_t line = space.tables + 8192; line < space.tables + 12288; line += 64)
				__asm__ volatile("dc civac, %0" :: "r"(line) : "memory");
			__asm__ volatile("dsb sy" ::: "memory");
			put(&apps_smmu_context, "voice tlbiasid", SMMU_CB_TLBIASID, space.context);
			put(&apps_smmu_context, "voice tlbsync", SMMU_CB_TLBSYNC, 1);
			for (unsigned ms = 0; rd(apps_smmu_context.base + SMMU_CB_TLBSTATUS) & 1; ms++) {
				if (ms == 100) { error = ETIMEDOUT; goto out; }
				sleep_ms(1);
			}
		}
		uva_memory_mapped = true;
	}
	*physical = kvtophys(uva_memory);
	*adsp = (space.passed ? *physical : AUDIO_ADDRESSES + UVA_AUDIO_OFFSET) | AUDIO_STREAM_BITS;
out:
	lck_mtx_unlock(&hakios_smmu_lock);
	return error;
}
