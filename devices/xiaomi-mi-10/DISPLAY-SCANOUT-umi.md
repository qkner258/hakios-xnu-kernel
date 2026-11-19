# UMI display scanout: from CPU copies to DPU page flips

Status 2026-11-10 ~00:30 CST. **fb_dpu=1 works on the phone** (logs/test-1109-232501-umi-fbdpu1). Every frame was flipped, and 27 of 27 flips latched. Copy time is 0 and scaling about 1 ms. There was no fallback and no hang. fb_dpu=2 (self-checking) and fb_dpu=3 (DPU scaling) are built but have not run yet.

## Why the UI is janky (baseline)

CoreAnimation renders the iPhone X screen (1125x2436 BGRA IOSurface). Then, for every frame,
`MiniIOMobileFramebuffer` (overlays/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp) does this on the CPU:

1. It scales the surface to 1080x2340 (`fb_threads` threads) into a 10 MB staging buffer.
   Meanwhile `SwapEnd` blocks CoreAnimation until this step is done.
2. It waits for the command-mode DMA idle window after TE (`hakios_display_wait_copy`).
3. It `memcpy`s 10 MB from the staging buffer into the GOP frame at 0x9C000000. The DPU fetches that frame at every TE (INTF autorefresh).
4. It `bzero`s the 11 MB of kept surface memory (`MiniIOSurfaceClearKept`) before taking the next frame.

v3 run (`logs/test-1109-215741-umi-ca-flash-v3.kernel.log`):

- 32–42 frames/s;
- 10–13.5 ms per frame in the driver;
- frame-to-frame median 18–32 ms;
- the GPU is mostly idle.

Mainline msm DRM does no CPU pixel work. The DPU scans GPU buffers directly.

## Stage 1: measurement (`fb_stats=1`, also on with `metal_fps=1`)

Every 2 s while frames flow, the kernel prints one `hakios_tl` line. The format is below; the numbers are only illustrative:

```
fbstat: 2004 ms staged-copy: 80 swaps in (39.9/s), 74 to the panel (36.9/s), latched 0, 6 not in time, 0 dropped;
 scale 3900 us avg 6100 max; TE wait 4100 us avg 9000 max; copy 2900 us avg 3300 max;
 surface clear 1200 us avg 2100 max; GPU wait 1500 us avg; swap to panel 14000 us avg 30000 max; 6 threads, nearest by runs
```

What each field means:

- **swaps in**: `SwapEnd` calls from CoreAnimation.
- **to the panel**:
  - staged: frames copied in their window;
  - dpu: flips issued;
  - direct: frames written.
- **latched**: (fb_dpu) flips the DPU actually took at a TE. This is the real panel frame rate.
- **not in time**: staged frames that missed their window, or flips that were not latched.
- **dropped**: swaps that gave up waiting (700 ms).
- **scale**: CPU time to scale all layers.
- **TE wait / flip wait**: the wait for the idle window, or for the previous flip to latch.
- **copy**: staging to panel memcpy (0 with fb_dpu).
- **surface clear**: `MiniIOSurfaceClearKept` per frame.
- **GPU wait**: waiting for the surface's GPU fence.
- **swap to panel**: from `SwapEnd` to copy start or flip issue. With fb_dpu, add up to one TE period (16.8 ms) for the latch.

The old 5-second `MiniIOMobileFramebuffer: N frames ...` lines are unchanged.
The Metal `fps: ... 0 presented` line stays at 0 because CA presents through this framebuffer, not through a CAMetalLayer. `fbstat` is the real number.

## Stage 2: cheap wins (default ON, output identical)

- **Nearest scale by runs** (`fb_filter=0`): `drawLayer()` turns the column map into runs of consecutive source pixels.
  At 1125 to 1080 that is 45 `memcpy`s of 24 pixels per row instead of 1080 gathers. The runs are only used when the average run is at least 8 pixels.
  A host check against the per-pixel map passed for 2000 random geometries.
- **Dynamic row chunks** (`fb_threads>1`): the presenter and helpers take 32-row chunks from an atomic counter. They no longer get fixed bands.
  Previously a helper on an A55 (XNU cpu4–6, about 4x slower per row than cpu0–3) set the frame time. Now slow cores just do fewer chunks.
  This is used instead of hard-binding threads to cpu0–3: a bound thread would wait whenever CoreAnimation occupies its big core.
- **Surface clear is measured, not removed.** The clear keeps new IOSurfaces zeroed. Skipping it in the presenter would move the `bzero` onto CoreAnimation's own allocation path.
  To A/B it, `surf_clear=0` is now accepted by the loader, and `fbstat` shows its cost.

## Stage 3: zero-copy scanout (`fb_dpu=1`, default OFF)

### What exists on the hardware (sources and DT)

- UEFI leaves DSI0 in **command mode with INTF_1 autorefresh** (INTF+0x2b4 = 0x80000001).
  At every TE the source pipe(s) fetch the GOP frame at 0x9C000000 (1080x2340, stride 4320).
- The panel is the J2 Samsung **DSC** command panel (2 slices of 540). UEFI therefore probably uses two LMs and two DSCs, and possibly two pipes or a multirect pipe.
- 0x9C000000 is `cont_splash_region`, size 0x2300000 (umi live DT). That is room for **three** 10,108,800-byte frames at a 0x9B0000 stride. XNU's RAM window starts at about 0xF7000000, so XNU never allocates here.
- The display's SMMU context is `iommus = <apps_smmu 0x820 0x402>` with `qcom,iommu-earlymap`, meaning the splash region is mapped 1:1 for handoff. **Frames inside the carve-out are fetchable without touching the SMMU.** Whether XNU's own RAM is reachable is unknown (see fb_dpu=2).
- SM8250 DPU (mainline catalog dpu_6_0_sm8250.h, MDP at 0xAE01000):
  - CTL_n at +0x1000+0x200n;
  - SSPP VIG0..3 at +0x4000/0x6000/0x8000/0xa000 (QSEED3LITE scaler);
  - DMA0..3 at +0x24000..0x2a000 (no scaler);
  - LM_n at +0x44000+0x1000n.
  - SSPP flush bits: VIG 0,1,2,18; DMA 11,12,24,25. LM bits 6–10,20. CTL bit 17.

### What fb_dpu=1 does (bsd/dev/hakios.display.c, `hakios_display_dpu_*`)

1. **Snapshot (read-only):**
   - all six CTLs: INTF/fetch/DSC/merge3d active, flush, layer configs;
   - the CTL that drives INTF_1;
   - the LMs staged on it;
   - every SSPP whose SRC0..3_ADDR points into the GOP frame (size, xy, stride, format, op mode, multirect, QSEED op for VIGs).

   Each address register is kept as an **offset into the frame**, which makes split, multirect or two-pipe setups work unchanged.
2. **Buffers:** frame 0 is the GOP frame. Frames 1 and 2 are at 0x9C9B0000 and 0x9D360000, mapped write-combined in the kernel map (not the 32 MB `ml_io_map` window).
3. **Per frame (presenter):**
   - pick a frame that is neither scanned nor pending;
   - scale straight into it;
   - release the source (`SwapEnd` returns: no staging, no idle-window wait, no copy);
   - wait at most 100 ms for the previous flip to latch (CTL_FLUSH bits clear at the autorefresh frame start);
   - write SRCn_ADDR = frame + offset, then CTL_FLUSH = pipes | LMs | CTL.
   - Never more than one flip is pending.
4. **Fallback:** a flip not latched in 100 ms (or the display clocks gone) causes these steps:
   - the latest frame is copied into the GOP frame;
   - the pipes go back to it;
   - a staging buffer is allocated;
   - the old CPU staged-copy path continues.
   - Log: `display: fb_dpu: a flip was not latched within 100 ms; back to the CPU copy path`.
5. **Back to the loader / panic:** `hakios_ddr_quiesce()` (cpufreq.c, called from `loader_return()`) first calls `hakios_display_dpu_restore()`. The panel then shows the GOP frame that the loader and the panic console draw into.

The CPU still scales (one streaming pass, about 11 MB read cached and 10 MB written WC) but does nothing else per frame.

### fb_dpu=2: one visual test, then as fb_dpu=1

At framebuffer start, the kernel:

1. allocates 10 MB contiguous **below 4 GiB in XNU RAM** (SSPP address registers are 32-bit);
2. fills it with **8 vertical colour bars** (white, yellow, cyan, green, magenta, red, blue, black);
3. points the pipes at it for 3 s;
4. points them back.

A person must look at the panel:

- **bars visible**: the DPU reaches XNU RAM as UEFI left the SMMU. IOSurfaces (allocated contiguous below 4 GiB) could then be scanned out directly.
- **black, grey or garbage**: the SMMU translates, and a stage-1 mapping would be needed (as UniXNU/uke did on SM8550).

Possible logs:

- `display: fb_dpu=2: no 10108800 contiguous bytes below 4 GiB in XNU RAM; test skipped` (likely: XNU's window starts near 0xF7000000).
- `display: fb_dpu=2: test flip latched|NOT latched`, then `back on the GOP frame: latched`.

## Boot (supervisor only)

Use the CA command from CA-METAL-umi.md and append `fb_dpu=1`. Stats come with `metal_fps=1`.
Check: 529/607 bytes (`tools/check-ca-bootargs.py`).

```sh
cd /Users/xm666/X/umi-ios
R=work/umi-v23 \
XL_LOADER=/Users/xm666/X/umi-ios/work/umi-v23/out/xiaomi-mi-10/flash/loader.efi \
UEFI_FD=/Users/xm666/X/umi-ios/ios-port-research/repos/JopangJovan_hackios-xnu-kernel-umi/work/display-uefi/variants2/SILICIUM_UEFI.mm-vote.fd \
RAMDISK=/Users/xm666/X/umi-ios/work/umi-v23/out/xiaomi-mi-10/flash/flashboot.apfs \
XL_RAMDISK=/Users/xm666/X/umi-ios/work/umi-v23/out/xiaomi-mi-10/flash/root-flash-ui-ca.apfs \
SECS=600 \
BOOTARGS="bootcpu=7 cpus=7 autounlock=0 ufs=2 ufs_native=0 ufs_write=1 root_apfs=ufs_darwin var_hfs=ufs_darwin_data umi_touch=1 umi_keys=1 umi_haptic=5 adsp=1 adsp_umi=1 sound=4 gpu=1 metal=1 metal_fps=1 fb_scan=1 fb_threads=6 fb_filter=0 cpufreq=1 wifi=1 umi_bt=1 torch=3 battery=1 umi_mic=4 touch_lat=1 fb_dpu=1" \
./phone-test.sh umi-ca-dpu1
```

Suggested order:

1. without `fb_dpu` (the new stats plus stage 2 only; compare with v3);
2. `fb_dpu=1`;
3. once, with the user watching the boot, `fb_dpu=2`.

### Log lines

Success:

- `display: fb_dpu: CTL_k intf=0x2 ...`: the snapshot; keep it for the next stage.
- `display: fb_dpu: SSPP DMA0|VIG0 scans the GOP frame: ...`: which pipe(s), and whether a scaler exists.
- `display: fb_dpu: CTL_k flush mask ... N address registers, 3 buffers of 10108800 bytes ...`
- `display: fb_dpu: 3 scanout frames; ...`
- `display: fb_dpu: frame 0..4 flipped to scanout frame 1/2 ...`
- `fbstat: ... dpu-flip: ... latched N ...` with N close to "to the panel", copy 0, and a small flip wait.

Failure, with an automatic fallback to the old path:

- `display: fb_dpu: needs the command-mode scan (fb_scan=1); CPU path kept`
- `display: fb_dpu: no CTL drives INTF_1` / `no source pipe fetches the GOP frame` / `not the UMI splash carve-out`: no register was written.
- `display: fb_dpu: a flip was not latched within 100 ms; back to the CPU copy path`, followed by `pipes back on the GOP frame (CTL_FLUSH=...)`. This means autorefresh does not latch flushes without CTL_START. Report the CTL_FLUSH value.
- `display: fb_dpu: the display block's clock or power went off; flips stop`

## Risks

- **Flush latch semantics under autorefresh** are taken from downstream SDE ("no CTL_START with autorefresh") and from mainline's video-mode "flush register reads 0 when latched".
  - If the bits never clear: 100 ms freeze once, then fallback.
  - If they clear immediately (no double-buffering in this state): at worst one torn frame per flip, never a corrupted buffer.
- The flush also re-latches the staged LMs and the CTL layer config. They are written back unchanged, so this is harmless unless UEFI left unflushed shadow values. Pre-existing CTL_FLUSH is logged in the snapshot.
- The carve-out frames 1–2 are assumed unused by UEFI runtime. The DT reserves the whole 0x2300000 for splash.
- With fb_dpu, anything that draws into the console framebuffer (`vinfo`) is not visible until the restore: panic text, and `MiniIOMobileFramebufferShow` used by the old GL `eagl` path. **Use fb_dpu with the CA payload only.**
- The `fbstat` line and `dpu` flips run in the presenter thread only. Restore runs at `loader_return` with other CPUs stopped.
- Not verified without hardware: everything in stage 3, and the stage-2 speedups (only compiled, plus a host check of the run mapping).

## Next: true zero-copy (no CPU pixel work at all)

There are two ways to remove the remaining CPU scale.

**(a) DPU fetches the IOSurface and scales it.** This needs all of the following:

- the pipe that UEFI uses is a **VIG** (QSEED3LITE; DMA pipes cannot scale). The fb_dpu snapshot says which;
- the DPU reaches the surface (fb_dpu=2), or an apps_smmu stage-1 window is programmed for SID 0x820;
- the CA framebuffer surfaces are physically contiguous below 4 GiB, or mapped through that window. MiniIOSurface currently allocates them pageable and discontiguous (`surfaceMemory()`);
- QSEED3 is programmed with phase steps 1125/1080 and 2436/2340, bilinear without a LUT (mainline `_dpu_plane_setup_scaler3`). With two LMs, it is split into two halves with a 0.5-pixel initial phase on the right half.

**(b) The GPU scales into a carve-out frame.** The A6xx 2D engine (`CP_BLIT`, `BLIT_OP_SCALE`, `SP_PS_2D_SRC_INFO.FILTER`) does this. It is not implemented here because the kernel has no command-stream builder:

- the proven 2D-blit sequence is the userland one (hakios-metal/src/render.cpp `encodeBlit`, with `passStart`/`passTail` CCU mode and timestamp tables);
- a mistake in a kernel-built IB hangs the GPU, which hakios.adreno does not recover. That would freeze the UI with no CPU fallback, against the "fallback, not black screen" rule.

The cleaner route for (b) is userland: the Metal CA plug-in blits its final frame at panel size into a carve-out frame exposed to it. The kernel then only flips (this stage's mechanism).

## 2026-11-10: hardware facts and levels 2/3

The fb_dpu=1 snapshot from the phone:

- CTL_0 drives INTF_1, with DSC_0 active.
- LM_0 is staged alone (out 1080x2340).
- **One pipe, VIG0** (QSEED3LITE available):
  - src = out = 1080x2340, stride 4320, format 0x237ff (ARGB8888 with alpha);
  - op 0x80000000 (SW pixel-extension override), scaler off.
- Flush mask 0x20041 (VIG0 | LM_0 | CTL).

CoreAnimation's frame surface is 1125x2436 at **4608 bytes a row**. CA **reuses** these surfaces (surface 3 carried swaps 1, 2 and 7).

### fb_dpu=2: self-checking fetch test (no eyes needed)

It uses LM_0's MISR, a CRC of every frame the mixer puts out (mainline dpu_hw_lm.c / dpu_hw_util.c `dpu_hw_setup_misr`). The pipe is temporarily set to show the top 64 rows only. The kernel takes signatures of three cases:

- bars in the carve-out (R);
- black in the carve-out (B);
- the same bars in a 64-row contiguous buffer **in XNU RAM below 4 GiB** (T).

The outcome lines:

- `display: fb_dpu=2: FETCH TEST PASS: XNU RAM at ... is fetched by the DPU (MISR T = carve-out R, black B)`
- `display: fb_dpu=2: FETCH TEST FAIL: XNU RAM at ... is not fetched as written ...`: the SMMU translates, or the fetch faults.
- `display: fb_dpu=2: FETCH TEST NO ANSWER: MISR ... does not tell them apart`: the MISR is not usable.
- `display: fb_dpu=2: FETCH TEST SKIPPED: no contiguous buffer below 4 GiB`: XNU's window starts near 0xF7000000.

Before the test it reads apps_smmu (0x15000000) read-only. It prints `fb_dpu=2: reading apps_smmu ...`, then IDRs and every valid SMR matching SID 0x820 with its S2CR (translated, BYPASS or fault). For a translated entry it also prints the context bank's CBAR, SCTLR, TCR, TTBR0, MAIR0, FSR and FAR.

If the boot dies right after `reading apps_smmu`, the hypervisor refused the read. Do not use level 2 again.

Then fb_dpu=1 goes on, as before.

### fb_dpu=3: the DPU scales (VIG0 QSEED3, bilinear), the CPU only copies

It is programmed as mainline does for RGB:

- `_dpu_plane_setup_scaler3`: phase steps (src<<21)/dst, QSEED4 preloads 4/2, filter BIL, alpha BIL, blend, no LUT, no detail enhancer;
- `_dpu_plane_setup_pixel_ext`: REQ_PIXELS = source size, LR/TB = 0;
- SRC_SIZE 1125x2436, stride 4608, OUT_SIZE unchanged.

It is self-checked at start with the MISR. White 1080x2340 unscaled and black are signed, then white 1125x2436 scaled. **PASS only if scaled white == unscaled white != black.**

On PASS:

- the carve-out becomes 3 frames of 1125x2436 (stride 4608, every 0xAC0000);
- the framebuffer driver's "panel" becomes 1125x2436;
- `drawLayer()` takes its unscaled path, a threaded row `memcpy` from CA's surface.

On FAIL, UEFI's geometry is restored and fb_dpu=1 goes on. The loader return and a later fallback also restore UEFI's geometry.

The outcome lines:

- `display: fb_dpu=3: DPU SCALING PASS: QSEED3 1125x2436 -> 1080x2340 (MISR scaled white X = white X, black Y); 3 frames ...`, then `display: fb_dpu=3: frames are 1125x2436 in the carve-out, the DPU scales them to the panel`; fbstat mode `dpu-scale`.
- `display: fb_dpu=3: DPU SCALING FAIL: MISR scaled white ... white ... black ...; UEFI's geometry back, fb_dpu=1 goes on`
- `display: fb_dpu=3: DPU SCALING NOT TRIED: ...`

Gain: bilinear quality at the cost of today's nearest-neighbour path (one copy of about 11 MB), and the CPU no longer scales.

### Why there is no zero-copy yet, and what it needs

**(a) The DPU scans CA's IOSurface.** This needs all of:

- the surface to be DPU-addressable: 32-bit SSPP addresses, while XNU RAM sits mostly above 4 GiB. That means a stage-1 window in the display's apps_smmu context bank (the fb_dpu=2 dump says whether UEFI left SID 0x820 translated and through which CB), or the surface in the carve-out;
- the surface to be contiguous in that space (MiniIOSurface allocates pageable memory; an SMMU window would map its pages);
- **use tracking.** CA reuses surfaces, so the scanned one must stay marked in use (IOSurface useCount in the shared page) until the next flip latches. Otherwise CA renders into the frame on screen.

**(b) The GPU writes the panel frame.** The CA Metal plug-in would point its final pass or blit at a carve-out frame mapped into the GPU (hakios_gpu_map takes any physical page); the kernel then only flips. That is a hakios-metal change, out of scope for this kernel-only step.

With fb_dpu=3 the remaining CPU work is one memcpy per frame (about 1 ms on 6 threads, as the scale costs today).

## 2026-11-10 (later): results of levels 2 and 3, and fb_dpu=4 (zero copy)

### Phone results

**fb_dpu=2** (logs/test-1110-001202-umi-fbdpu2):

- apps_smmu SMR4 0x84020820 → S2CR 0x4: translated through CB4.
- CB4 has SCTLR 0xe0 (M=0: no translation), so the display fetch is effectively bypass. It is limited to 32-bit addresses: only memory below 4 GiB.
- XNU had no contiguous RAM below 4 GiB (FETCH TEST SKIPPED).

**The carve-out is the only DPU-reachable memory.**

**fb_dpu=3** (logs/test-1110-001501-umi-fbdpu3):

- PASS: MISR scaled white 0x4b9dd087 = white 0x4b9dd087, black 0x3bf6648a.
- latched 23/23; CPU copy about 1.13 ms a frame.

### fb_dpu=4: CA's frame surfaces are drawn by the GPU straight into carve-out frames

It does everything fb_dpu=3 does, and it starts only when the fb_dpu=3 self-check passes.

**Binding (MiniIOMobileFramebuffer.cpp `zeroSlot`).** The first time a swap shows a surface that qualifies, the presenter binds it. A surface qualifies when all of these hold:

- one layer only;
- 1125x2436 BGRA, stride 4608 (= the frame stride), offset 0;
- source and destination cover the whole display;
- it is not a wrapped client surface;
- the kernel console has stopped.

To bind it, the presenter:

1. copies its picture into a free carve-out frame (once);
2. takes a reference to it;
3. calls `HakiosGPUSurfaceAlias(s, frame)` (hakios.gpu.cpp).

`HakiosGPUSurfaceAlias` re-points every existing GPU mapping of that surface at the frame. It overwrites the PTEs, then `hakios_gpu_remapped()` (new in hakios.adreno.c) commits the tables and does TLBIASID + sync. Later mappings of the surface are made onto the frame directly. Aliased surfaces get no cache push in submit and no `gpu_written`.

**Limits.** At most 2 of the 3 frames are bound. One frame always stays a copy frame for swaps that cannot be flipped: other surfaces, more layers, black.

**Per swap.** For a bound surface:

1. wait for the GPU (as before);
2. flip to its frame;
3. **wait for the latch**;
4. complete the swap.

`SwapEnd` therefore returns only when the frame is on the panel, so the surface CoreAnimation draws into next is never the one scanned. There is no CPU pixel work.

**Self-checks.**

- Every flip must latch within 100 ms. Otherwise the code falls back to the CPU copy path and every alias is ended with a copy back.
- **ALIAS check:** at bind time, a hash of every 64th row of the surface's own memory (cache invalidated) is taken. On the surface's next 8 swaps the hash must be unchanged, which proves the GPU no longer draws there.
  - If it changed: `ALIAS FAIL`, zero copy off (no copy back), and fb_dpu=3 continues.
  - After 8 clean swaps: `ALIAS PASS`.

**Leaving zero copy.**

- A surface that only this driver still holds is unbound (`surface N is gone; frame k free again`).
- If a copied swap finds the copy frame scanned (for example two non-black copied swaps in a row), zero copy is turned off with a copy back and fb_dpu=3 continues. Repeated black swaps do not trigger this.

**Known limits.**

- While aliased, the CPU view of CA's frame surfaces (their own memory) is stale. This affects `rec_secs` recordings and any CPU reader of those surfaces.
- If CA draws into a surface it swapped before SwapEnd returned (seen once at boot: surface 3 swapped twice in a row), that frame can tear once.
- SwapEnd now waits up to one TE (≤16.8 ms) for the latch instead of about 1 ms of copying. If CA's frame time plus that wait exceeds a refresh, fps can drop compared with fb_dpu=3. Compare the `fbstat` lines.

### Log lines

**Arming (at boot):**

- `display: fb_dpu=3: DPU SCALING PASS ...`
- `display: fb_dpu=4: zero copy armed: CoreAnimation's frame surfaces will be drawn into 2 of the 3 carve-out frames`

**Working:**

- `display: fb_dpu=4: surface 3 aliased onto frame k at 0x...: drawn there by the GPU from now on` (also for surface 4).
- `display: fb_dpu=4: ALIAS PASS: surface N drawn 8 times into frame k, its own memory untouched`
- `fbstat: ... dpu-zero: ... latched N ... ; zero-copy M`, with M close to "to the panel". "scale" covers only the copied swaps.

**Failure (with automatic fallback):**

- `display: fb_dpu=4: needs the DPU scaling (fb_dpu=3) to pass; zero copy off`
- `display: fb_dpu=4: ALIAS FAIL: surface N's own memory changed ...`, then `zero copy off: an alias did not take`
- `display: fb_dpu=4: zero copy off: a surface's GPU mappings could not be moved`, or `... no frame is free for a copied swap`
- `display: fb_dpu: a flip was not latched within 100 ms; back to the CPU copy path`, then `zero copy off: the CPU copy path takes over`

### fb_dpu=4, revision b (after the merged run lost zero copy)

What happened in logs/test-1110-011310-umi-merge1:

1. Surface 3 was aliased onto frame 2.
2. Swap 3, surface 4, was not aliasable for an unknown reason (same in run a), so it was copied into frame 0.
3. Swaps 4 and 5 were black. Swap 5 found frame 0 shown and frame 1 still pending, so there was "no frame free" and zero copy went off.

Changes:

- **Copied swap with no free unaliased frame.** First wait for the pending flip to latch; the frame that was shown is then free. Failing that, demote one aliased surface that is not on the panel: copy its picture back, return its GPU mappings to its own memory, and use its frame for the copy. The surface may be aliased again later. Zero copy ends only after 16 demotions.
  - Log: `display: fb_dpu=4: surface N gives frame k back for a copied swap (M so far)`
- **Diagnosis.** A full-display surface that cannot be aliased is reported once, with its format, stride, offset, size, layers and rects.
  - Log: `display: fb_dpu=4: surface N not aliased: ...`
- **SwapEnd no longer waits for the latch.** A zero-copy swap completes as soon as the flip is programmed. Instead, a GPU submission naming an aliased surface whose frame is still on the panel (with a flip away from it pending) waits outside gGPULock, for at most 50 ms, until CTL_FLUSH clears.
  - Code: `MiniIOMobileFramebufferOffscreen()` → `hakios_display_dpu_leaving()`, a read-only register check.
  - Effect: CoreAnimation does its CPU work for the next frame during the TE wait and blocks only at submission. "flip wait" in fbstat should drop to around 0.

### fb_dpu=4, revision c (after merge2)

The merge2 run (logs/test-1110-012946-umi-merge2) showed two things:

- CA's second frame surface (4) has a 4544-byte stride (1136 px), not 4608, so it was never aliased.
- Swaps alternated between surface 3 (zero copy) and surface 4 (copied):
  - the average "scale" was 551 µs, half of a 1.1 ms copy;
  - every copied swap's flip waited for the zero-copy flip just before it (CA drew surface 4 without any submission wait), which gave about 7.5 ms average flip wait.

Changes:

- **Per-frame SSPP stride.** `hakios_display_dpu_stride()` sets YSTRIDE0 for a frame; it is written before the address and latched by the same flush.
  - An aliased surface keeps its own stride: any 64-byte multiple of at least 4500 bytes, as long as stride × height fits the 0xAC0000 frame.
  - Copied frames use 4608.
  - The partial-cover copy reads the shown frame with that frame's stride.
- **Diagnostics.** "not aliased" lines now give the reason: more than one layer, not one plane of BGRA, row stride, offset, client memory, bigger than a frame, or not shown whole. The first picture (console still on) is no longer reported.
- **fbstat** ends with `zero-copy M (flip wait X us avg Y max)`, the zero-copy swaps' own wait. With both surfaces aliased it should be near 0: the TE wait moves into the GPU submission of the surface that was shown before.

### Revision d: the merge4 panic (Device memory)

logs/test-1110-025107-umi-merge4 panicked with `Unaligned kernel data abort` in `__bzero` (show+3032) on the first black swap (layers 0x0) with fb_dpu=4.

The kernel maps the carve-out write-combined (Normal-NC, MAIR attr 1 = 0x44). The fault is still an alignment fault on a page-aligned address, which is DC ZVA on Device memory. The hypervisor's stage 2 evidently makes the splash region Device for the CPU, so the combined attribute is Device.

The old CPU copy path to the GOP frame has always used memcpy of aligned, 64-byte-multiple blocks, which happens to be legal there. bzero (DC ZVA), unaligned accesses and compiler-vectorised stores at 4-byte alignment are not.

The fix, with fb_dpu only (the default staged path is unchanged):

- **Every CPU access to the carve-out goes through `carveCopy()` / `carveZero()`.** These are volatile, naturally aligned 8-byte (else 4-byte, else byte) loads and stores. They are never vectorised or merged; the disassembly shows only `ldr/str x` and `ldr/str w`.
- **drawRows()** builds each row in a per-drawer RAM row (`fRowOut[]`) and then `carveCopy()`s it into the frame. This covers all scale paths: unscaled, runs, nearest and bilinear.
- **Black frames** use `carveZero()` per row.
- **Other carve-out reads and writes** go through `carveCopy()` too:
  - the partial-cover copy from the shown frame;
  - the bind copy;
  - the demote and leave copy-backs;
  - the fallback copies into and out of the GOP frame.
- **dpu_fill()** (the MISR tests) uses volatile 4-byte stores.
- **What is unchanged:** GPU writes (through the GPU's own SMMU) and the DPU's fetches.

### fb_dpu=4, v11c: SwapWait throttle, and copied swaps of aliased surfaces

v11b (SwapEnd returns at once for a flip of an aliased surface) flickered back and forth, and in Maps flashed the boot logo.

- **Back and forth.** CoreAnimation calls SwapWait(N) before it commits frame N+1, which it draws into the surface of swap N-1. SwapWait returned at once, so in 2266 frames CA drew into a surface whose own swap was still queued or flip-pending (e.g. swap 7085 drawn into slot 0 while 7083's flip to slot 0 was pending: shown 7085, 7084, 7085). The v11 Offscreen() test only covered a surface already *shown*, and read a stale `dpu_shown`.
  - Fix: SwapWait(N) now waits (fb_dpu=4 only, 700 ms limit) until show() is done with swap N (`waitShown()`). Offscreen() is back to the latch test only (`hakios_display_dpu_leaving()`). SwapEnd still returns at once.
- **Boot logo.** Maps' swaps have two layers (its surface, then CA's surface 3/4 on top). They are copied, and the copy read the aliased surface's own memory, last written when it was aliased at boot.
  - Fix: a copied swap first demotes every aliased surface it names (`surface N gives frame k back for a copied swap`).

### fb_dpu=4, v16: an aliased frame is free once this refresh's transfer is done (not at the latch)

v15 (runtime/logs/223031-run.log, `touch_lat=1` lines `tl: F/L/M/w/s`):

- `GPU-submit: ... offscreen` ≈ the whole submit wall: CoreAnimation's final submission waits in `MiniIOMobileFramebufferOffscreen()` for the flip away from the surface it draws into to latch. That is up to one TE per frame (4–13 ms; on-time frames p50 8 ms).
- Changing frames of the SpringBoard UI (home, switcher, Control Center; log lines 100k+): 90.8% on time. Of the 320 misses, 139 had their GPU work held by that latch wait. Their command buffers had been made 16–20 ms before the deadline TE, and that band misses 34–39% (3–7% at 22–34 ms).

Command mode: the panel keeps the picture in its own memory. A frame is fetched only by the transfer that an autorefresh TE starts (DMA idle after line 1221 of 2362, about 8.6 ms). With the flip away pending and this refresh's transfer done, the next TE takes the new frame, so the old one is never fetched again.

`hakios_display_dpu_leaving()` now returns 0 then. It uses the test the staged path already writes the scanned GOP frame by (`hakios_display_wait_copy()`: phase read around the write pointer, `phase >= te_idle_start && written >= shown`). The flush is read first, and a TE after that latches the flip. So a submission waits at most until DMA-done (about TE+8.6 ms) instead of the next TE.

Estimate from v15: 77 of the 320 UI misses make their TE.

Check:

- `GPU-submit:` lines: `offscreen` per window drops, and `max` in UI windows stays under about 10000 us (was 13–16 ms).
- `tl: M ... committed X done Y`: commit-to-done of gated frames is no longer ≈ 16 ms + GPU.
- No tearing or flashes (correctness: a frame is written only after its last fetch).

## Composition v29

Host implementation, 2026-11-11; **no phone access or hardware validation**.
The buffer-management fix is active with existing `fb_dpu=4`. Experimental DPU
multi-layer composition additionally requires the regular file
`/var/db/hakios-display/dpu-compose` **before the first framebuffer client opens**.
Absence means off; no boot arguments or userland payload changes are required.
The existing `fb_dpu=1/2/3/4` meanings are preserved. Composition requires the
successful `fb_dpu=3` scaling handoff (thus =3 or =4); =4 still aliases ordinary
single-layer frame surfaces. The separate 90 Hz experiment remains default off.

### All sixteen v28 demotions

Source: root `logs/test-1111-172537-umi-v28.kernel.log`, byte/newline line numbers
(`rg -an`; do not use Unicode `splitlines()`, which also splits embedded control
characters). No alias-check failure, mapping failure or flip timeout accompanies
these sixteen events. They are explicit calls to `zeroDemote`, which had **two
normal-workload callers**: (A) a copied swap references an aliased source, whose
old IOSurface memory is stale; (B) no unaliased destination is free, because two
of three frames are aliases and the only copied frame is pending/scanned.
Both callers copied the entire alias back and changed its GPU mapping.

| Event | Log line | Surface / slot | Swap (where logged) | Evidence |
| --- | --- | --- | --- | --- |
| 1 | 16497 | 4 / 0 | 5 | copied swap after copied slot 1 |
| 2 | 27053 | 4 / 1 | 29 | copied swap after copied slot 0 |
| 3 | 86303 | 4 / 0 | — | explicit two-layer swap, bottom 264, overlay 4: A |
| 4 | 86418 | 3 / 2 | — | explicit two-layer swap, bottom 266, overlay 3: A |
| 5 | 104933 | 4 / 1 | 3203 | copied swap after slot 0 |
| 6 | 147238 | 4 / 0 | 6020 | copied swap after slot 1 |
| 7 | 187070 | 4 / 0 | 7918 | copied swap after slot 1 |
| 8 | 223315 | 4 / 0 | 10156 | explicit two-layer swap, bottom 612, overlay 4: A |
| 9 | 223376 | 3 / 2 | 10157 | next copied swap |
| 10 | 224764 | 4 / 1 | 10276 | copied swap in rapidly alternating 3/4 ownership |
| 11 | 224774 | 3 / 0 | 10277 | next copied swap |
| 12 | 225045 | 3 / 0 | 10299 | copied swap in rapidly alternating 3/4 ownership |
| 13 | 225057 | 4 / 2 | 10300 | next copied swap |
| 14 | 231689 | 3 / 1 | 10773 | copied swap after slot 0 |
| 15 | 243036 | 4 / 0 | 11553 | copied swap after slot 2 |
| 16 | 246563 | 3 / 1 | 11806 | copied swap after slot 0 |

**Evidence limit:** old `zeroDemote` printed the same message from A and B and
did not print the swap's layers at these later swaps. Except the explicit A
records above, v28 cannot distinguish these two callers per event. The table
records every event without inventing that missing information. All sixteen
are accounted for by this shared buffer-management mechanism; neither caller
is a hardware error. Line 249760 is a *later* copied swap exhausting the
lifetime budget, not a seventeenth hardware failure. Raising that budget would
leave the repeated migrations and CPU work intact.

### Buffer management fix (existing =4, default active)

Keep two persistent aliases and one copied slot. A CPU compositor/recording
reader obtains the current alias pixels from its carve-out into a private
cached snapshot; it never reads the stale IOSurface backing store or changes
GPU mappings just to read. Device memory is read with aligned scalar
`carveCopy`, then normal CPU row workers operate on cached RAM. Fence failure
drops the swap before any pixel read instead of composing unfinished GPU work.

If the copied slot is pending, retire that latch first. If it is then scanned,
compose the new image in a separate cached buffer and copy into that slot only
in the existing measured DMA-idle window, with the existing copy-time margin
and TE-wrap exclusion. A real copy into free frame 1 is timed at startup, so
the initial budget also works with the shorter 90 Hz idle interval. Missing an
idle window preserves the previous picture. Ordinary free copied destinations
and alias flips retain their normal behavior. There is no demotion budget:
normal mixed swaps do not demote. Genuine alias/mapping/hardware fallback can
still turn aliases off and is counted by reason. Two extra frame-sized cached
buffers cost about **22 MiB** with =4.

In-place copied presents increment the existing CPU-present counter, not the
DPU latch count (no flip was issued). Thus `to the panel` may legitimately
exceed `latched`; compare `inplace` as well. Do not interpret a sampled source
as hardware-composed output: hardware composites are counted as unknown by
`fbcontent`.

### Experimental DPU final composition

**Choice:** reuse the proven VIG QSEED3LITE scaling plus one existing LM/DSC/
INTF path, rather than introduce shaders and another GPU submission/fence
path. Two through four layers use VIG0..3, BASE/STAGE0..2 and LM0. Each source
crop/destination is scaled directly to panel coordinates: e.g. logical
1125x2436 becomes 1080x2340. BGRA and RGBA 8-bit interleaved sources use their
respective unpack patterns; overlay blending is premultiplied
`source + destination*(1-source.alpha)`. The bottom layer retains the CPU
path's replace behavior. Alpha is bilinearly scaled along with RGB. This can
have small sampling/rounding differences from CPU composition followed by
whole-image scaling.

This is **hardware final composition with CPU source uploads**, not zero-copy
multi-layer scanout. Two banks of four immutable 12 MiB cached source buffers
cost **96 MiB**, plus about 288 KiB of page tables. The presenter first waits
for all source GPU fences. It copies each source (from its alias, if bound)
into the free bank, cleans those cache lines, then commits all pipe geometry,
blend stages, addresses and flush in one transaction. Only then is SwapEnd
completed. CoreAnimation may reuse/release its original surfaces immediately;
DPU fetches the independent snapshots at every autorefresh. A bank is reused
only after the previous pending configuration latched and the bank is no
longer scanned. This avoids relying on GPU-submission changes to fence a
variable number of original source surfaces.

The splash carve-out is only 35 MiB and cannot hold two banks of four full
frames. A **private unused, disabled apps-SMMU context bank** therefore maps
these source pages to 32-bit IOVAs. Firmware's display-only SMR 0x820/mask
0x402 must match exactly; a broader match, occupied handoff, missing context
or allocation failure refuses the experiment before routing changes. Other
SMMU streams and the GPU domain are untouched. The 0..4 GiB identity mapping
preserves splash addresses; snapshot IOVAs occupy 0x60000000..0x67ffffff,
with unmapped 4 MiB guards after each 12 MiB source. Page tables are published
and the context TLB synchronized before replacing the display stream route.
This new SMMU/VIG configuration is the main reason **the switch is off**.

Eligibility: one observed VIG0/LM0 handoff, successful existing scaling test,
2–4 BGRA/RGBA layers, a full-screen bottom destination, in-bounds source crops
and panel destinations, <=4096 dimensions, <=12 MiB per source, supported
scaling ratios. Offscreen/clipped destinations, other formats/planes, screen
recording and diff/dirty diagnostics retain CPU composition. A later partial
CPU frame reconstructs the previous hardware composition from its retained
immutable snapshots before applying its updates. Hardware-to-single flips
restore the original stage/format and scaled geometry atomically. Loader
return restores UEFI geometry, waits for the GOP source to latch, then returns
the original SMMU route and extra pipe/clock state. If that latch fails,
mappings stay pinned; no still-fetchable bank is freed.

**GPU wait interface coordination:** `hakios_display_dpu_write_busy(phys)` and
its existing submit mutex remain owned by the GPU wait work. Composition
publishes `dpu_pending`, `dpu_flush` and flush programming under that same
`dpu_submit_mtx`; no sleep occurs under it. Slots 0..2 are the original
carve-out frames. Internal shown/pending values 4/5 mean immutable composition
banks and are never GPU alias targets; they must not index three-slot arrays.
The GPU submit implementation and userland Metal files were not edited here.

### Counters and next phone measurement (supervisor only)

All new statistics carry values, not pixel contents or object addresses.
Existing `fb_stats`/`metal_fps` enables the two-second summary. Existing
`touch_lat=1` additionally emits a short record per completed swap:

```
fbcompose-swap: id=12000 layers=4 cpu_us=31 upload_us=2100 hw=1 inplace=0
fbcompose: swaps=60 layers0=0 layers1=40 layers2=12 layers3=6 layers4=2 hw=20 inplace=0 cpu_us=310/1200 upload_us=700/2800
fbzero: enabled=1 demotions=0 map=0 alias=0 fallback=0
```

Numbers above are **illustrative, not measured gains**. `layersN` is the count
of swaps with N actual source surfaces. `cpu_us` is CPU draw/composition wall
time (including alias reads and previous-picture reconstruction), avg/max across
processed swaps in the summary. `upload_us` separately measures snapshot memcpy/cache maintenance,
excluding the bank latch wait. Both are wall intervals across parallel row
workers, not summed CPU cycles. Existing `copy` measures the final in-place
copy; `inplace` counts those successful copies. `hw` counts DPU compositions;
single-layer alias scanout is separately the existing `zero-copy` count.
`demotions` is the cumulative number of aliases retired by a genuine fallback;
`map`, `alias`, `fallback` count reason events. `fbzero-demote reason=1/2/3`
means mapping failure / alias-check failure / display CPU fallback.

Good: `fbzero enabled=1 demotions=0` remains true after repeated Weather,
Home/lock and Maps transitions for **longer than v28's 1083 s**. With the
experiment enabled, expect `fbcompose-init: enabled=1 banks=2 layers=4 ...`,
nonzero `hw`, low `cpu_us`, bounded `upload_us`, matching total panel presents
and issued flips plus in-place copies, and clean `fbcompose-restore: restored=1`.

Bad/refused examples:

```
fbcompose-init: enabled=0 reason=smmu-stream
fbcompose-failure: reason=bank-or-smmu
fbcompose-failure: reason=fence swap=12000
fbzero-demote: total=2 reason=2
fbcompose-restore: restored=0 reason=latch
```

Measure baseline (=4 without the file) first, then =4 with `dpu-compose`, at
60 Hz before combining with the separate 90 Hz experiment. Exercise Weather's
2–4 layers, transparent/half-alpha overlays, Maps, transitions to single-layer
Home, partial/offscreen layers and screen off/on. Check picture correctness,
alpha edges, no stale frames/tearing, `cpu_us + upload_us + copy`, GPU-wait,
actual latched/present rates and thermal behavior. Compare warm animations
separately from shader compilation. Expect removal of the 5.9–10.3 ms CPU
multi-layer blend/scale work for eligible swaps; raw source upload/cache
maintenance remains and must be measured. No claim of sustained 60/90 fps or
optical flicker improvement is made by this host-only change.

### Host validation and artifact

`tools/test-display-compose.py` compiles production geometry, MMIO programming,
SMMU setup/restore, bank retirement and CPU alias-reader/slot-selection code.
It checks default-off causes no MMIO, rejects occupied handoffs/broad streams,
preserves other stream routing and splash identity, checks guarded source
page mappings, 2/3/4 stages, BGRA/RGBA unpack, premultiplied coefficients,
1125/1080 and 2436/2340 phase steps, clipping rejection, pending-bank timeout,
restored single-pipe stride/stages, and 64 copied swaps retaining both aliases.
Existing display, display-session, backlight, display-UEFI-probe and GPU-submit
host tests also pass. These tests cannot validate SMMU virtualization policy,
VIG bandwidth/QoS or electrical/pixel output on the phone.

Builds use only root `build-lock.sh bash devices/xiaomi-mi-10/build-kernel.sh
BUILD_DSYM=0`. Build logs, host-test logs and before snapshots are under
`out/xiaomi-mi-10/perf-compose/`. Packaging status/hash will be appended after
a successful whole-kernel build; a prior kernel is never relabeled as this one.

**Current packaging blocker (19:12 CST):** latest display C/C++ objects compile
successfully, but the whole-kernel build fails in shared sources outside this
task's permitted scope: `iokit/Kernel/hakios.wlan.cpp:195` passes a const pointer
to `setProperty`'s mutable-buffer overload; `bsd/dev/hakios.net.c:555` names
`ifnet_init_eparams.detached`, whereas this XNU header provides `detach`.
These files were not edited. The supervisor was notified while display work
continued. No `kernel-perf-compose` or kernel SHA256 is claimed until the
responsible network work resolves those errors and the locked build succeeds.

## v29f: multi-layer frames no longer lose themselves on a shut copy window

2026-11-12, display path only. Sources edited (the persistent overlay — the
generated tree under `out/xiaomi-mi-10/xnu` is refreshed from it by
`prepare.py kernel`, and the two were byte-identical before and after, so what
runs is what is edited here):

- `overlays/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` →
  `out/xiaomi-mi-10/xnu/iokit/Kernel/MiniIOMobileFramebuffer.cpp`
- `overlays/xnu/files/bsd/dev/hakios.display.c` →
  `out/xiaomi-mi-10/xnu/bsd/dev/umi_display_impl.c` (`hakios.display.c` includes it)
- `overlays/xnu/files/bsd/sys/hakios.display.h`

`xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp` (the older base copy) and
`xnu-device.patch` do not carry this file; nothing else in the tree does.

### What was wrong (from `work/v29e/INVESTIGATION-maps-notes.md`, section A)

With `fb_dpu=4`, a frame with more than one layer cannot be aliased, so it is
CPU-composed and lands in a carve-out frame. When the only frame free for it is
the one the panel is fetching (two of three are aliases, the third was just
flipped to), the frame is copied **in place** and the copy has to wait for the
DMA idle interval of that frame's transfer. When that interval did not come the
frame was counted `fStMissed` and **dropped**: no demotion, no second path, and
the whole 100 ms `DPU_FLIP_LIMIT_NS` was waited out for every frame of the
window (`fbstat: 15 swaps in, 0 to the panel, 15 not in time; flip wait 1508125
us avg 101056 max`). CoreAnimation's drawables are released by a present, so its
`nextDrawable` then waited ~110 ms, Maps ran at 6–8 fps, and the map stood still
while gestures kept running. Single-layer frames alias and flip in ~1 ms and
kept running at 50/s, which is why the same build was smooth elsewhere.

The interval can stay shut for good: the panel's transfers are started by
commits, and a run of in-place copies commits nothing, so the completion the
copy waits for is never produced again ("self-locking"). The contradiction the
investigation listed in its 1.5 (no `display: fb_dpu: ... back to the CPU copy
path` line and `demotions=0` although the flip path would demote) needs no stale
binary to explain: the in-place branch is the one that ran, and it only counted
`fStMissed` — which also matches `flip wait` equal to the 100 ms limit and
`copy 0 us`. Source and generated tree are byte-identical
(`sha256 e0ffb456…` before this change), so what is edited in the overlay is
what `prepare.py kernel` puts into the build tree.

### What changed

1. **A free frame that is not shown is taken first** —
   `MiniIOMobileFramebuffer.cpp:1726` (selection in `show()`, fb_dpu=4 branch).
   The free frame is chosen in two passes: `fBound[i] == NULL && i != pending &&
   i != scanned` first, then any free frame that is not pending (which, when it
   is the scanned one, means the in-place copy). Before, the lowest free index
   won even when the shown frame was the only other candidate, so an avoidable
   in-place copy could be picked. The pending flip is now retired with
   `DPU_INPLACE_LIMIT_NS` instead of 100 ms. The two-frame fallback
   (`:1735`) now refuses a frame an alias is on, so a composed frame can never be
   written over a GPU surface's.
2. **The in-place copy is never dropped** —
   `MiniIOMobileFramebuffer.cpp:1870-1924` (`else if (inPlace)`):
   - the wait is `DPU_INPLACE_LIMIT_NS` (**20 ms**, `:248`), one refresh's worth;
   - on a timeout `fCopyNs` is decayed (a budget learnt too large keeps the
     interval shut) and `hakios_display_dpu_refresh()` is called: the frame that
     is on the panel is committed again, which starts a transfer, and the wait
     is tried once more;
   - if the interval is shut still, the frame is **copied into the panel's frame
     anyway** and committed again after the copy. The panel fetches it on its
     next transfer, so one frame may be seen torn rather than not at all. No
     frame is dropped and `fb_dpu` is not given up (demoting would take the zero
     copy away from the single-layer frames that do reach the panel at 50/s);
   - if a pending flip latched during the wait, the frame copied into is no
     longer the one on the panel: it is flipped to (`:1906`), so the picture is
     shown rather than left in a free frame.
3. **`hakios_display_dpu_refresh()`** — `hakios.display.c:994` (declared in
   `sys/hakios.display.h`). Programs the frame already shown once more and sets
   no pending flip; returns 0 without programming when a flip is outstanding,
   because a commit then would be taken for that flip's latch by
   `dpu_check_locked()` and the frame it goes to would be believed shown.
4. **One state-change line from the wait itself** — `hakios.display.c:227`
   (`hakios_display_wait_copy`, on a timeout, once per run of the same reason,
   cleared when a window comes): which of the five conditions was out, with the
   values it was read with. This closes the investigation's open item (1.4) —
   the failing sub-condition — without a per-frame line.

Nothing per frame is logged. The run counters are folded into the existing 2 s
lines: `fbzero: ... slow=%u` is the run-total of frames copied in place with no
idle interval (`MiniIOMobileFramebuffer.cpp:2509`).

### Log lines to look for

```
display: no copy window (<reason>): phase P of L (idle from I), W of S lines written,
         N us of the frame left, the copy wants M us         <- once per run of a reason
fbzero-slow: frame <id> copied into scanout frame <n> with no idle interval (<count> so far);
         the panel's frame is committed again each time      <- once per run of them
fbzero: enabled=1 demotions=0 map=0 alias=0 fallback=0 slow=<count>
```

What a fixed window looks like: `slow=0` and no `fbzero-slow` at all, with
`fbstat: ... <n> to the panel` in every 2 s window that has swaps, multi-layer
windows included (`fbcompose: ... layers2=<k>`), `0 not in time`, and
`flip-wait` with a max well under the old 100805 us (an in-place frame can still
wait up to one 20 ms window). What a still-shut window looks like: `no copy
window ... the panel's transfer is not done` together with `fbzero-slow` and
`slow=<n>` climbing, `to the panel` now counting those frames, and `scale`
(Maps' composition) rather than `flip wait` dominating the 2 s window. If the
`reason` is `no room left after the transfer` instead, the copy budget is the
problem, not the transfer, and `fCopyNs` is decaying on its own.

### Validation and risk

Host only; **no phone access, no kernel build** in this change. `python3
devices/xiaomi-mi-10/tools/test-display.py` and `test-display-compose.py` pass
(the latter's synthetic `DPU_INPLACE_LIMIT_NS` define was added, and its source
markers moved with the code; it compiles the real selection block, which now
exercises the two-pass choice). The rewritten in-place branch was also compiled
on its own against fakes with `-std=c++17 -Wall -Wextra -Werror` and exercised
for: a window on the first try (one wait, no commit, no flip), a shut window
(two 20 ms waits, copy, commit before and after, `slow` up, line said once), the
window coming back (state cleared), a frame the panel left during the wait
(flipped back), and a flip still pending (nothing programmed). The alias guard
in the two-frame fallback was compiled and exercised over zero/non-zero
`fZero`, every `scanned` value, alias present or not, flip latching or not. All
five display host tests still pass (`test-display`, `-compose`, `-session`,
`test-backlight`, `test-display-uefi-probe`).

Risks: (a) whether a commit of the unchanged address really starts a transfer
on this DPU is unverified — if it does not, the second wait fails too and the
late copy with the commit after it is what carries the frame, which is still
bounded and still counted; (b) the late copy can be seen as one torn frame
during a multi-layer run that has no idle interval at all; (c) in the steady
multi-layer case an in-place frame waits up to one window (~8 ms of the frame
here) as before, so the gain for Maps is bounded by its 26–41 ms CPU
composition as well; (d) the 20 ms wait for the pending flip is one frame plus a
margin, so it can fail under display jitter — the in-place path then runs, which
is what it did before, only faster. Nothing here changes the single-layer
zero-copy path, the boot arguments, or any file outside the display path.

