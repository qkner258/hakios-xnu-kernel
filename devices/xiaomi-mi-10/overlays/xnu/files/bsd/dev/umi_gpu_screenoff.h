/* Called only with HakiosGPU's submission/teardown lock held, after all fences
 * retire (down), or before the next submission (up). GMU performs a normal
 * OPP transition: no raw PLL or clock gating. */
void umi_gpu_screenoff_vote(bool off);
void
umi_gpu_screenoff_vote(bool off)
{
	static bool failed;
	if (failed || !umi_a650 || !gpu_kept || !gmu_talks ||
	    !umi_gpu_boot_index) return;
	uint32_t target_khz = off ? 305000 : umi_gpu_boot_khz;
	if (core_khz == target_khz) return;
	uint32_t index = off ? 1 : umi_gpu_boot_index;
	uint32_t vote[4] = {0, 1, index, HFI_NO_INDEX};
	if (!hfi_send("idle OPP", HFI_GX_BW_PERF_VOTE, vote, 4)) {
		failed = true; /* ambiguous acknowledgement: never blindly cycle power */
		printf("umi-pm: GPU OPP FAILED; further idle votes disabled\n");
		return;
	}
	core_khz = target_khz;
}
