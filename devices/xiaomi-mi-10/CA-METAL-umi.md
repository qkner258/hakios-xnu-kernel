# CoreAnimation Metal image for UMI — flash v2, 2026-11-09

The v2 flash payload addresses the two supervisor runs on Xiaomi Mi 10
(Adreno 650, chip `0x6050002`). UMI now defaults to sysmem rendering and
`/redwood/share/ca-share/QuartzCore-default.metallib`; neither
`metal_gmem_draws=0` nor `ca_metallib=` is needed. Missing CA variants are
compiled from **iOS 16.5's own QuartzCore library** with the values
CoreAnimation set (v3, below). **No phone was accessed; v3 needs device validation.**

## v3 exact path (2026-11-09 evening; replaces v2's cpf-config route)

**Why v2 never worked on the phone.** Every missing pipeline in
`logs/test-1109-175406-umi-ca-flash-v2b` (56 distinct) logged
`missing named constant fc_vertex_layout`. iOS 16.5's CoreAnimation does set it:
it specialises its library's generic uber shaders by NAME (QuartzCore's string
table: `fc_generic`, `fc_vertex_layout`, ... 58 for `fixed_frag_lph_cph`, the
same 58 every pipelinelib record carries). Metal keeps a named value only when
the function declares that constant, and v2's ca-share put pipelinelib
*variants* in place of the uber shaders: they declare none (the phone logged
`functionConstantsDictionary: 0 entries`), so every value was dropped. The
host test passed only because it injected the constants itself.

**The library.** The claim that this image has no `default.metallib` was
wrong: the IPSW's root filesystem has `QuartzCore.framework/default.metallib`
(1,014,208 bytes, 141 functions, sha256 `d7cdaaaf…4eb9`); `select-system.py`
merely leaves it out. `tools/ca-metallib.py` now copies it unchanged into
ca-share (from `sysimage/rootfs.dmg` with 7z, cached in `sysimage/ios-qc/`).
No macOS AIR is used any more: the blur/downsample/home-affordance/
attachment-clear/edr_scale/luma_boost functions (`Tds4`, `Tn11`…`Tn27`,
`Tnb7`, `Tsb3`, `Tvrd`, `Tvrb`, `Thom`, `Tatc`, `Tlub` are *dedicated
functions*, not uber-shader variants — the phone linked e.g.
`downsample_blur_vert_lph+narrow_blur_19_frag_lph`) were a later macOS
CoreAnimation's until now; they are iOS 16.5's own. The `edr_scale →
inplace_copy` stand-ins are removed. `QuartzCore-exact.plist` is gone.

**The exact path** (`HakiosMetalCA.m`, `HakiosMetalCAExact.h`): for a
`com.apple.coreanimation.draw.*` label whose fragment variant the pipelinelib
lacks, the vertex and fragment functions' own AIR is compiled with the values
Metal recorded (`specializationAirScript`, else `baseFunctionConstantValues`).
`hm_ca_exact_why_not` checks that against the constants the AIR itself
declares (`hakios_metal_bitcode_function_constants`, new) and otherwise logs
the precise reason before the old nearest-name fallback.

Compiler work this needed (host and iOS `libair.a` rebuilt):
- `compiler.cpp` `forwardConstantScratch`: the uber shader's `air.static_init`
  writes 31 constants into one zero-initialised struct
  (`fc_fragment_shader_config`) that lives in scratch; constant stores are now
  forwarded to constant-offset loads, so the branches fold (22,000 NIR lines →
  a specialisation the size of Apple's variant).
- `tools/mesa/air/air_to_nir.c`: a render-target output whose
  `air.function_constant` predicate folds to false is not written (Apple's
  compiler drops it; the uber shader otherwise fetched and rewrote every
  attachment). `llvm.compiler.used` no longer blocks folding the
  `__metal_implicit_fc_pred_N` globals.
- `lowerFbRead` picks framebuffer-fetch texture slots from the slots actually
  sampled, not every slot the function declares.

**Host regression** (`make -C hakios-metal ca-exact-test MESA=1`): real macOS
Metal loads the iOS library, makes each function with ONLY the named, typed
constants CoreAnimation sets, and the plug-in's own code reads them back. All
444 labelled pipelinelib fragment functions (139 cph, 305 cpf; 27,462 values
read back) compile from the generic function to the same colour outputs,
texture slots, API samplers and fragment inputs as Apple's precompiled variant;
all 56 device-log pipelines compile and link (cph ones write exactly the
`fc_dest_index` target). Their constants come from a label decoder that first
reproduces all 444 recorded constant sets exactly; `Tvdk` (46) and `Iwht` (31)
are taken from QuartzCore's name tables. A pipelinelib variant standing in as
the library function is refused with the precise reason. Pixels are not compared.

**Expect on the next run:** `CoreAnimation exact configuration: <spec>: fixed_vert_lph_spc (4 constants) + fixed_frag_lph_cph (58 constants), iOS 16.5 AIR`
(dedicated ones with `(0 constants)`), and none of `missing named constant`,
`exact CA configuration impossible`, `stands in for it`, `refused: a render
pipeline`. The stage cache key changed, so the first boot recompiles every
pipeline. `luma_boost` draws are still left out unless `metal_luma_boost=1`.

## Findings and fixes

The inputs were `/Users/xm666/X/umi-ios/logs/ca-debug/ca-flash1-gmem-hang.log`
(and `test-1109-160344-umi-ca-flash1.kernel.log`) and
`/Users/xm666/X/umi-ios/logs/ca-debug/ca-flash2-nogmem-refused.log`.
Run 1 contains 17 missing-variant messages, 17 distinct, and no pipeline
refusal. Run 2 contains 26 such messages, 19 distinct, and two identical
varying refusals across backboardd restarts. The complete extracted lines,
including “nothing of the same kind stands in”, are saved in
`out/xiaomi-mi-10/ca-debug-v2/all-substitutions-and-refusals.txt`.

**GMEM hang.** Run 1's first GMEM activity coincides with submission 43
stalling after 42; run 2, with GMEM disabled, has no GPU hang. Comparison
with local Mesa 25.0.7 freedreno/Turnip FD650 configuration confirms the
existing `a6xx.hpp` values: GMEM `0x120000`, three CCUs, quarter color-cache
reservation at `0x114000`, GMEM `RB_CCU_CNTL=0x8a400000`, 96×16 tile alignment,
and 32 VSC pipes. There is no demonstrated reason to change those constants.
Two definite tile-planner errors in `render.cpp` were corrected:

* Origin rounding used a power-of-two bit mask with A650's non-power-of-two
  alignment of 96. It now uses division and multiplication.
* Rounding bin width to 96 could produce 1056, exceeding the 1024 hardware
  limit. Column planning now uses the largest aligned width within the limit
  (960).

These fixes do not prove the cause of the observed hang; command-stream,
binning/VSC, and resolve behavior still need hardware validation. UMI's
`metal_gmem_draws` default is therefore **0**. Explicit `metal_gmem_draws=`
and `metal_gmem_off=` remain accepted by the loader. Do not enable GMEM in
the first v2 run.

**Crash and incorrect shaders.** Run 2 line 8349 substitutes
`TcimA2S1Xhf_Ialp` for `TcimA2S1Xhf_Idst`; line 8350 refuses the resulting
link because the fragment reads VAR1 and the vertex does not write it.
CoreAnimation treats pipeline creation failure as fatal and exits backboardd.
Other substitutions also change operations: square/circle masks become alpha
masks, and `TimgBcdgA2Xhf_Isrg` becomes `TimgBcdgA2Xhfcx_IalpCcl`.
These are strong evidence for wrong pixels even when the GPU completes.
Shader selection could choose a vertex from its existing label before choosing
a different fragment stand-in. In addition, AIR's declaration-order varying
numbers are not a stable interface when specialization removes declarations.

(v2's cpf-configuration route and its sidecar are superseded by the v3 exact
path above.)

**Flicker and black blocks, ranked by evidence:**

| Rank | Candidate and evidence | Change or diagnostic |
|---|---|---|
| 1 | Semantically different shader substitutions; proven fatal interface mismatch in run 2. | Exact generic compilation, semantic varying slots, safe zero for absent outputs. |
| 2 | Definite deferred-submission race: commit could return and deliver scheduled callbacks before the worker submitted GPU work and published the IOSurface fence. | C API deferred commit now drains recording/submission before returning. This does not wait for GPU completion. The first command also omits `metal_defer=1`. |
| 3 | CPU copies versus GPU completion: framebuffer already waits the IOSurface fence and invalidates caches, but can only wait work whose fence has been published. Six copy threads alone are not evidence of a bug. | `metal_sync=1` waits GPU completion before scheduled delivery; compare `fb_threads=1` separately. |
| 4 | IOSurface/YUV shadow freshness. Run 2 line 5983 reports 480-byte rows, which require a compatible shadow; sampled windows show two shadow reads and zero writebacks. No evidence here proves writable-shadow corruption. | `metal_shadow_refresh=1` refreshes read-only IOSurface/YUV shadows even when the seed is unchanged; writable render targets keep normal synchronization. |
| 5 | No-copy buffers are snapshots, as warned at line 5929. Later caller writes could be stale, but the once-per-process warning and immediate CA upload/blit usage do not establish that this happened. | `metal_nocopy_refresh=1` refreshes borrowed CPU upload bytes at binding. Diagnostic only: it does not provide full bidirectional no-copy coherence. |
| 6 | Purgeable storage. Line 3983 explicitly says buffers remain nonvolatile. | No eviction occurs; this log does not support purge as the cause. |

The giant icons/frozen unlock in run 1 have no separate logged texture-size or
icon-dimension refusal. A stalled animation and incorrect shaders remain
plausible explanations; a distinct geometry bug is unproven.

**`0 presented` is expected on this path.** The Metal FPS counter counts
`presentDrawable:` calls. Backboardd hands IOSurfaces to IOMobileFramebuffer,
so completed command buffers can accompany zero drawable presentations and
`0.0 frames/s`. Use command-buffer completions, GPU timings, framebuffer swap
statistics and visible updates; do not require a nonzero drawable count.

## Build and artifacts

The current CA flash payload was backed up as `.v1` before rebuilding. The
normal `rc.jobs` services and native AIR compiler are retained; GL/OSMesa
remains available to other clients. M2V is unset: this is the direct Adreno
path. Build commands from this checkout:

```sh
source /Users/xm666/X/umi-ios/env.sh
# after a change in tools/mesa/air (v3 changed air_to_nir.c): the tree's copy and its libair.a
cp tools/mesa/air/* xnu-deps/build/mesa/mesa-25.0.7/src/compiler/air/
PATH=/Users/xm666/X/umi-ios/tools/mesa-venv/bin:$PATH /Users/xm666/X/umi-ios/build-lock.sh \
  ninja -C xnu-deps/build/mesa/build-softpipe-freedreno src/compiler/air/libair.a
/Users/xm666/X/umi-ios/build-lock.sh make -C hakios-metal ios MESA=1
/Users/xm666/X/umi-ios/build-lock.sh python3 tools/ca-metallib.py \
  /Users/xm666/X/umi-ios/work/umi-v23/out/xiaomi-mi-10/ca-share
env FLASH_CA=1 METAL_PLUGIN=ca \
  EXTRA_SHARE=/Users/xm666/X/umi-ios/work/umi-v23/out/xiaomi-mi-10/ca-share \
  bash devices/xiaomi-mi-10/flash/build.sh userland
bash devices/xiaomi-mi-10/flash/build.sh loader
```

`flash/build.sh` acquires the build lock itself. The final userland snapshot is
`out/xiaomi-mi-10/flash/build-20261109-172324`; its plug-in sources match the
working tree. The UMI overlay defines `HAKIOS_UMI=1` for the CA bundle. The
rebuilt loader retains the supervisor's GMEM whitelist and truncation warning,
and adds the diagnostic arguments below.

| File | Bytes | SHA-256 |
|---|---:|---|
| `out/xiaomi-mi-10/flash/root-flash-ui-ca.apfs` | 21,655,552 | `4f167b91dfd60bb04bb3c53cab0e42e0dbc5dded0f3438ac52c9a1be0949bd62` |
| `out/xiaomi-mi-10/flash/root-flash-ui-ca.apfs.v1` | 21,487,616 | `14c7b3d55137d6592608ef655d1abe5bd485f2dcc2c59536f0e10f40992478b3` |
| `out/xiaomi-mi-10/flash/root-flash-ui.apfs` | 19,173,376 | `a828165d29e383adb37f89f403fc5e5fa17c7a522f290208202fb26cae4cb874` |
| `out/xiaomi-mi-10/flash/flashboot.apfs` | 34,451,456 | `03a9b8579b470c51c7493f64ad7bdb1cae1bef4e2b62600f6493a01dff033216` |
| `out/xiaomi-mi-10/flash/loader.efi` | 91,136 | `cb1c5b2d0a96138d5a2777a0261a0051123283d39ea6716cbd8d1586fb351579` |
| `out/xiaomi-mi-10/ca-share/QuartzCore-default.metallib` (v3: iOS 16.5 original) | 1,014,208 | `d7cdaaaf3e496bbed67ca83193075e2e0f040aee6567a278587cd81dbd5c4eb9` |

The GL payload and `flashboot.apfs` are byte-for-byte unchanged. The `.v1`
backup hash matches the original CA payload. Offline APFS verification confirms
`lib/HakiosMetal.bundle/HakiosMetal`, the native AIR compiler, both CA library
files, the init bind mount, and the 63 MiB kernel-plus-payload limit.
The flash payload binds its plug-in onto
`/System/Library/Extensions/HakiosMetal.bundle`; stage0, firmware and cryptex
mount points still come from the unchanged `flashboot.apfs`.

## Supervisor command and boot-line budget

**Do not use `var_store=1`: its slots overlap the UMI flash layout.** The
supported persistent `/var` is `var_hfs=ufs_darwin_data` with `ufs_write=1`;
`/private/var/db/hakios-metal-cache` survives when that mount succeeds.
Allow the first run time to recompile shaders.

The arm64 boot buffer holds 608 bytes including NUL. The previous long command
lost `battery=1`, microphone/touch options, and Wi-Fi MAC at truncation. This
command uses **520/607 bytes**, leaving **87 bytes**: loader/XL prefix 198,
BOOTARGS 294, phone-test's trailing space 1, and appended Wi-Fi MAC 27.
This includes the current 2.3.4 kernel version, `loader_return_after=600`,
10-digit epoch and `sb_delay=8`. Pointer values use fixed-width hex.
`keepsyms=1` and the two now-default CA options are omitted.

Supervisor-only command; it was **not executed** during this work:

```sh
cd /Users/xm666/X/umi-ios
R=work/umi-v23 \
XL_LOADER=/Users/xm666/X/umi-ios/work/umi-v23/out/xiaomi-mi-10/flash/loader.efi \
UEFI_FD=/Users/xm666/X/umi-ios/ios-port-research/repos/JopangJovan_hackios-xnu-kernel-umi/work/display-uefi/variants2/SILICIUM_UEFI.mm-vote.fd \
RAMDISK=/Users/xm666/X/umi-ios/work/umi-v23/out/xiaomi-mi-10/flash/flashboot.apfs \
XL_RAMDISK=/Users/xm666/X/umi-ios/work/umi-v23/out/xiaomi-mi-10/flash/root-flash-ui-ca.apfs \
SECS=600 \
BOOTARGS="bootcpu=7 cpus=7 autounlock=0 ufs=2 ufs_native=0 ufs_write=1 root_apfs=ufs_darwin var_hfs=ufs_darwin_data umi_touch=1 umi_keys=1 umi_haptic=5 adsp=1 adsp_umi=1 sound=4 gpu=1 metal=1 metal_fps=1 fb_scan=1 fb_threads=6 fb_filter=0 cpufreq=1 wifi=1 umi_bt=1 torch=3 battery=1 umi_mic=4 touch_lat=1" \
./phone-test.sh umi-ca-metal-flash-v2
```

The host-only length check is
`python3 devices/xiaomi-mi-10/tools/check-ca-bootargs.py`.
Pass a quoted replacement BOOTARGS string to check diagnostics. Override
`--prefix` if the kernel version, loader prefix, duration or XL options change.
The checker uses `out/xiaomi-mi-10/wifi-boot.args` when Wi-Fi is requested.
The loader additionally prints used/available characters and a clear
`WARNING: the kernel command line is TRUNCATED` if it overflows.

Keep `fb_scan=1` and no `fb_hz` override: UMI measures the inherited panel rate,
falling back to 60 Hz if needed. Six copy threads and `fb_filter=0` preserve the
comparison path. The logical screen remains the iPhone X's 1125×2436 rendered
onto UMI's 1080×2340 panel. `fbscale=2` is an optional later performance test,
not a fix for the reported oversized icons.

Change one diagnostic at a time and check the resulting length:

| BOOTARGS change | What it isolates |
|---|---|
| Append `metal_sync=1` | GPU completion before scheduled callbacks. |
| Append `metal_shadow_refresh=1` | Read-only IOSurface/YUV shadow seed freshness. |
| Append `metal_nocopy_refresh=1` | Borrowed CPU upload-source freshness. |
| Append `metal_nospec` | Disable the existing draw-time constant-buffer specialization. The new missing-variant exact compiler remains active. |
| Replace `metal_fps=1` with `metal_fps=timeline` | Existing timing detail. |
| Replace `fb_threads=6` with `fb_threads=1` | Framebuffer copy concurrency. |

Each individual change fits the budget; do not assume all diagnostic arguments
fit together. Existing `metal_all=1` is optional and exposes other clients to
CA Metal. The baseline keeps it off. `gpu_recover`, `fb_measure`, `fb_beam`
and Goodix `touch_poll_us` have no effective UMI reader and are omitted.

Require these log observations in the next run:

* Flash plug-in bind, Darwin root read-only mount, OS/App cryptex mounts on
  `/dev/ufs_darwin,c=1` and `,c=2`, and persistent HFS `/private/var` mount.
* Backboardd reports Adreno chip `0x6050002` with the compiler present, loads
  the CA library, and produces completed command buffers without GPU timeouts.
* Zero GMEM passes under the default settings; exact-configuration messages
  for missing variants, no emergency paired fallback and no pipeline refusal.
* The full boot line retains battery, microphone, touch latency and Wi-Fi MAC;
  the loader reports 520 characters with these exact inputs.
* Stable visible unlock/UI and framebuffer swaps. `0 presented` is not a failure.

## Host validation and limitations

`make -C hakios-metal test` passed **262 tests, zero failed checks**.
The added native-Mesa/Metal exact regression (`ca-exact-test`) passed
**140 configurations, zero failures**, including Idst. This exercises the
real host named-constant API (including bool versus uchar typing), compilation,
linkage, texture-slot counts and MRT masks. It does not execute the A650 GPU.
The wider native-Mesa suite ran 263 tests with 16 failing checks; all 16 were
independently reproduced from the pre-change git-index sources, in five
compiler/fixture tests. This is a baseline fixture mismatch, not a full suite
pass. Fixtures were not rewritten to conceal it.

Loader whitelist tests, the 520-byte checker, `git diff --check`, and the
offline `flash/verify.py` passed. The verifier checks artifact hashes, APFS
contents, payload bounds, and HFS geometry; phone operations are mocked.
Build logs, exact-test results, baseline comparisons, hashes, extracted log
lines and source fingerprints are under `out/xiaomi-mi-10/ca-debug-v2/`.
No adb, fastboot, phone boot, phone write, `run.sh` or `phone-test.sh` was run.
The exact named-constant lookup still needs confirmation against the phone's
Metal runtime, and visual correctness remains unmeasured on device.

## Historical RAM artifacts and GL comparison

The earlier resident RAM CA images were **not rebuilt with the v2 fixes**:
`out/xiaomi-mi-10/ca-metal/root.apfs` (15,548,416 bytes,
`f18f6620dbe46c0c618d10cb1107b4d841bc65f8601f57a5f0f62614d9f141d7`)
and `system.apfs` (2,571,960,320 bytes,
`f7a354748a9185b9aa0aa7f1cdca7a969f39fcdf6a13f8408c18bb2357a70c43`).
They contain the earlier CA implementation and staged X55 bundle, and are not
the supervisor target above. Camera code is present, but vendor tables were
not available. `umi_modem=1` is intentionally absent from this UI test.
The earlier GL RAM pair remains under `out/xiaomi-mi-10/gl-backup/`.
For flash GL fallback, use the preserved `root-flash-ui.apfs` payload.

The three GL symptoms in
`/Users/xm666/X/umi-ios/logs/test-1109-135331-v234-smoke.kernel.log` have
separate likely causes:

* The explicit `CoreAnimation's OpenGL ES renderer has no function 40 ("sb3")`
  line is definitive for the missing switcher/edit-mode shadows. `eagl.c`
  documents `sb3` as a four-sample shadow blur and says the renderer reports
  it absent, after which CoreAnimation does not draw that operation. A real
  GL fix needs a stable `sb3` shader/program and the same uniforms/blend state
  as Apple's renderer; `glyes=40` only lies to CoreAnimation and kills the
  window server.
* Black text boxes are not explained by `sb3`; the log does not identify the
  failing glyph. Candidate causes are incomplete font fallback in the trimmed
  RAM root, glyph-mask channel/alpha conversion, or incorrect blend state.
  `eagl.c` traces glyphs drawn as line spans and has mask/color-matrix fallbacks
  (`c4p`, `vcm`). Compare the same text on the full Apple flash root first;
  if it persists, inspect the glyph mask/atlas before and after upload and
  alpha/blend state on the failing draw. Candidate fixes are restoring a
  missing font when `font_test` confirms one, or correcting mask swizzle,
  premultiplication and blend state once the bad stage is isolated.
* Purple materials have direct upstream-input evidence: lines 7165/7187 show
  `flat=384 flat-HDR=0 purple-LDR=384`, RGBA16 `5959,1414,5959,ffff`, already
  purple **before Mesa**. These sampled blocks are valid LDR solid colors;
  this log does not establish an ASTC HDR-decoder failure. Trace the texture
  producer/asset and the shader/filter feeding it, then compare decompressed
  pixels. For separate framebuffer-fetch artifacts, existing `glfix=1`
  serializes earlier work with `glFinish()`; compare its diagnostic records
  before changing synchronization or blending. Do not recolor pixels merely
  because they match the purple diagnostic heuristic.
* Materials/backdrop blur uses the variable-radius `vrd`/`vrb` chain and the
  downsample stage (`ds8`; `ds4` supplies a sampling reference). The log
  reports no-program draws for function 39 (`vrb`) and 28 (`ds8`). `eagl.c`
  supplies fallback downsampling and a variable-blur mip chain; self-sampling, clear ordering and the mask read can therefore produce a
  black or unblurred strip. `vblur=2` is an upstream diagnostic mask paint (not currently in the
  UMI loader whitelist, so it needs an explicit diagnostic gate change). A complete
  GL fix needs the missing downsample and masked-read shaders, correct mip
  lifetime/clear ordering, and the `sb3` shader. The CA Metal path supplies
  these functions from QuartzCore's metallib and avoids the GL fallback.

## Chained passes (2026-11-09 evening): no flush and wait-for-idle between passes

**Cause.** Every render pass ended with the full tail and began with the
batch start: `render.cpp` `passTail` (WFI, `PC_CCU_FLUSH_COLOR_TS` waited
for, `LRZ_FLUSH`, `CACHE_FLUSH_TS` waited for) and `passStart` (three
invalidates, WFI, about 100 static registers, WFI, `RB_CCU_CNTL`).
`CommandBuffer::closePass` wrote that tail whenever the attachments changed,
so the GPU drained about 16 times per command buffer.

**Change.** A sysmem draw pass that is followed by another draw pass now ends
with `LRZ_FLUSH` only (`render::encodeLightTail`). The next pass opens without
the batch start or the `RB_CCU_CNTL` write (`render::lightHeadOptions`). The
full tail is written once (`CommandBuffer::flushChain`) in these cases:
- at the end of the buffer;
- before a copy, fill, blit, clear-only pass, dispatch, `append`, or GMEM pass;
- (until 2026-11-10 v16 also before a pass whose first draw samples memory that a chained pass wrote; that pass now stays chained and gets `render::encodeBarrier` before the draw, as a joined draw does).

The other hazards are handled like this (turnip `tu_flush_for_access` rules):
- **Joined draw reads chained output.** A draw that joins an open pass and samples what an earlier chained pass wrote gets `render::encodeBarrier`: WFI, colour (and depth) CCU flush waited for, `CACHE_INVALIDATE`, WFI.
- **Write after read.** A pass that writes memory sampled since the last WFI gets one WFI.
- **Writes across caches.** Colour versus depth CCU, or shader/UCHE writes, break the chain.

GMEM passes are never chained.

**Revert.** Boot-arg **`metal_pass_wfi=1`** (17 characters with its space)
restores the old per-pass tail and batch start. It is whitelisted in
`loader/loader.c`, so the **loader must be rebuilt**.

**Diagnostics.** The `fps:` line now reads `N passes (F full flushes with a
wait for idle, B barriers, W WAR waits, C chained)`.

**Tests.**
- The fake GPU now refuses a sampled texture whose memory a draw, fill or copy wrote in the same submission without a CCU flush followed by `CACHE_INVALIDATE`.
- Two tests were added: `chained_passes_flush_once_and_barrier_only_where_a_pass_samples_what_another_wrote` and `the_fake_refuses_a_draw_sampling_what_an_unflushed_pass_wrote`.

**Caveat.** With chaining, `metal_fps` per-pass GPU times are command-processor
stamps without a WFI, so they are approximate. Commit-to-completion and GPU
busy remain exact.

## IOSurface cache-clean A/B (2026-11-10)

The zero-copy morning run showed a variable cache-clean cost: active five-second kernel windows reported about 8 to 4,444 KB cleaned, taking about 4 to 156 us per report window (quiet windows were 0 KB). The submit path conservatively pushed every IOSurface after its first user mapping, even when its seed was unchanged and the command buffer used it only as a GPU read. That is safe but needlessly serialises the CA render thread.

The UMI overlay now has an opt-in `gpu_fc=1` path in `hakios.gpu.cpp`. For an IOSurface whose seed is unchanged and whose submit flags contain no GPU write, it skips the cache clean for the lifetime of that mapping. GPU-write surfaces and all default boots keep the old conservative behavior; aliased `fb_dpu=4` surfaces already skip the clean because their mappings point at the uncached display carve-out. The five-second `HakiosGPU:` line reports `KB skipped by gpu_fc=0/1`, alongside the existing cleaned KB and clean time.

Expected gain is the avoided cache walk over read-only texture surfaces (up to the measured 156 us per five-second window in this run); it does not reduce GPU work. The A/B boot line is `gpu_fc=1`, and the loader whitelist has been updated. If a client writes an IOSurface without changing its seed, this diagnostic mode can show stale pixels, so leave it off for the safe baseline and compare visual output and `KB skipped` against the same `fb_dpu=4` run. The first submission of each mapping always takes the clean, including when its initial seed is zero.

Host checks for this change are `python3 devices/xiaomi-mi-10/prepare.py verify`, `make -C hakios-metal test`, and the kernel build. Rebuild the kernel for the overlay and rebuild the loader for the whitelist; the CA userland payload does not need rebuilding unless it is being refreshed for another change.

## CA caller placement A/B (2026-11-10 morning log)

The final 18-minute run makes the CPU bottleneck explicit. In active windows, the `fps: ... draw calls by processor` lines report CPU0-3 at about 16–31 us per draw, while CPU4-6 take about 77–110 us. GPU busy is generally 17–49%, and `commit to completion` is typically 4–13 ms. `fbstat: ... dpu-zero` reports zero scale/copy time and every issued flip latched. The slow cores therefore account for a large part of the caller-side frame time; a GMEM experiment is not justified by this run.

The CA overlay now accepts opt-in `metal_big=1`. Each thread that creates a CA Metal command buffer is marked `QOS_CLASS_USER_INTERACTIVE`, which lets XNU's AMP scheduler prefer the P cluster while still allowing migration when those cores are occupied. The default leaves the caller's QoS unchanged. The overlay prints one `metal_big=1: CA Metal caller QoS set ...` line per process, including the return code.

For the A/B boot, add `metal_big=1` to the existing `fb_dpu=4 metal_rs=1 metal_fps=1` line and compare the per-CPU draw histogram, `-commit on the caller's thread`, commit-to-completion, and `fbstat` swaps. A successful run should move draw counts away from CPU4-6 and reduce caller commit time without changing GPU busy or zero-copy scanout. The loader whitelist and `configs/boot-args.lst` are updated; this change requires rebuilding the CA userland payload and loader, but no kernel rebuild. With the current prefix and Wi-Fi suffix it is 532/607 bytes; `gpu_fc=1` is 529/607. The existing safe `gpu_fc=1` A/B remains independent.

## Frame pacing / shader-cache checkpoint (2026-11-10, host validated)

This checkpoint changes the graphics-stage cache and makes the CA `fps:` counter use
panel presentations. No new boot arguments. Runtime performance remains for the supervisor
to measure; no phone tools were used. Build logs, a patch limited to this checkpoint,
and extracted evidence are in `out/xiaomi-mi-10/render-perf-checkpoint1/`.

**CPU topology correction.** Both morning1 and 082531 print
`cores for the kernel, in the order it takes them: 700 600 500 400 300 200 100`.
The loader assigns `cpu-id` in that order. Thus XNU CPU0 is physical 7 (A77 prime),
CPU1–3 are physical 6/5/4 (A77), CPU4–6 are physical 3/2/1 (A55); physical 0 stays
with the loader. The slow draw histogram is the little cores, not underclocked big
cores. The 082531 draw-weighted averages are 18.8 us on CPU0, 22.9–23.2 us on CPU1–3,
and 83.5–85.6 us on CPU4–6. New `cpufreq: cpuN -> MPIDR ..., group D` boot lines
expose this mapping directly.

`cpufreq=1` requests the highest LUT rows once (1804/2419/2841 MHz); only
`cpugov=1` starts the load governor. Thermal and display-off caps still apply.
These logs establish the requested frequencies, not measured instantaneous clocks.
BCM2837 uses Clutch without Apple's AMP/Edge policy; the existing `amp=1` enables
this port's slow-secondary pairing separately. Correct the earlier wording about
`metal_big`: user-interactive QoS is a scheduling hint, not a P-core placement guarantee.
No scheduling or frequency policy is changed in this checkpoint.

**v5's missing QoS line was a log-retention issue.** It exists at line 4008 of
`runtime/logs/084751-run.log.live.12`:
`metal_big=1: CA Metal caller QoS set to user-interactive (error 0)`.
The final `.live` no longer contains it. `hm_big_hint()` is called from the command
buffer initializer, and the A/B really did reach the QoS call. `hm_ca_arg`,
`hm_ca_sync` and `hm_boot_arg_set` now match complete whitespace-delimited tokens;
`metal=1` cannot satisfy `al=1`, nor `metal_gpu=1` satisfy `gpu=1`, nor
`metal_big=10` satisfy `metal_big=1`. The shared plain-C parser has host regression tests.

**10 Hz is not evidence of a global animation cap.** The older v3 trace directly
identifies `SBHClockApplicationIconImageView _timerFired:` requesting
`10..120, 10 a second` (merged log lines 14678–14679 and 16575–16576).
Both 082531 and v5 have many 10 Hz windows with one pass, about 22 draws and only
0.33 ms commit-to-completion per buffer, consistent with these desktop clock updates.
Other windows exceed 40 Hz. Filtering merely for nonzero swaps includes idle clock
updates and makes the median 10 Hz; it does not measure gesture smoothness. Do not
force the clock to render at 60 Hz to improve this statistic. The old gap wording
“while something moves” / “stalls” was misleading: the same counter included these
100 ms idle updates. It now explicitly reports interval buckets including idle updates.
The v5 boot line also lacks existing `anim_max=1`; unlike `dl_max=1`, that CAAnimation
hook is opt-in. It is a separate, available animation-cadence A/B, not required for
this checkpoint. `touch_lat=1` can identify callers and requested rates if needed.

**Cache changes (default).**

- Hash function constants in stable index order. The declared-constant path enumerates
  an NSDictionary; its iteration order is not a shader input. Values, presence versus
  explicit zero, AIR, IBO base and known-buffer bytes still determine distinct keys.
  Duplicate indices retain their original relative order.
- Load compiled stages before taking the shared Mesa lock. A foreground disk hit
  cannot wait behind background specialization. Recheck after acquiring the lock on
  a miss to avoid compiling another caller's newly saved stage again. Constant-index
  reflection also bypasses this lock: it only parses AIR into a private ralloc context,
  without NIR, ir3 or the shared compiler object.
- Use a unique temporary inode per cache writer, then publish by rename after a
  successful close; processes no longer share one `.tmp` file. Preserve 0644 read
  access for cached stages and count write failures.
- `stage-cache: build=... writable=... seed=...` names the actual selected cache.
  `fps: ... stage-cache:` reports disk/seed hits, real compilations and failures,
  lookup time, compiler-lock wait, compile time/max, saves and save failures. These
  are process-wide graphics-stage statistics since the previous report; concurrent
  operations count when they finish. Compute compilation is not included.

The existing build fingerprint hashes compiler.cpp, C++ headers and Mesa archives,
not wall-clock time in an iOS MESA build. A pure CA .m rebuild does not invalidate it;
compiler/header changes do. This checkpoint changes compiler.cpp and therefore has
**one cold-cache transition**. Compare repeated boots of the same artifact and identical
UI actions. First-ever unseen variants still compile; no approximate shader, startup
precompile worker, or change to shader semantics is introduced. The counters now tell
whether the next useful step is persistent-cache access, cold prewarming or CPU draw work.

**Real presentations in `fps:`.** `kern.hakios.fb_presented` is a read-only counter of
confirmed DPU latches plus completed CPU copies. Backboardd samples it at the beginning
and end of each reporting window and prints `N presented via panel`, keeping drawable
presents and command buffers as separate counts. Other processes keep their drawable
counter. An old kernel is explicitly labelled `panel count unavailable`. The sysctl
only reads counters; it never polls hardware or waits for TE. DPU latches are observed
by the existing flip polling, so a sample can lag the newest physical latch by one frame;
`fbstat` and `fps` also have different reporting boundaries. The counter is not panel
refreshes when the same frame is repeated. No copy/zero operation or `sid==0 && !zero`
protection was changed.

**Validation.** `prepare.py verify`, the plain host suite (277 tests, zero failed
checks), native-Mesa cache regressions (disk/seed/reordered constants, 128 parallel
hits, independent storage, changed/undefined/zero values and save failure),
`make -C hakios-metal ios MESA=1`, `ca-exact-test MESA=1` (444 labelled fragment
variants, 56 device pipelines, zero differences/failures), and the serialized UMI
kernel build passed. The standalone CA bundle links with **zero non-weak dynamically
looked-up symbols**, checked with `nm -u -m`; `build-ca-bundle.sh` records its recipe.
This is not a claim that the wider Mesa fixture suite was rerun or that hardware
rendering has been validated.

Supervisor: rebuild/package the current CA userland and kernel together. Keep
`metal_fps=1` and the existing baseline flags; no loader rebuild is needed for this
checkpoint. Look for `presented via panel`, `stage-cache: build=...`, the per-window
cache counters, `cpufreq: cpuN -> MPIDR`, and compare `fbstat` during matching gestures.
On a warm repeated run, common stages should hit disk, their compile/lock wait should
fall, and save failures should stay zero. Keep `metal_big=1 gpu_fc=1` unchanged if
comparing against v5; otherwise vary them separately. Do not count a cold boot of this
new compiler fingerprint as a persistent-cache regression.

## v6 diagnosis / allocation and presentation checkpoint (2026-11-10)

This is a measurement checkpoint, with no rendering approximation, new boot argument,
or scheduling/pooling policy change. The supervisor owns phone testing. Artifacts,
source delta, build logs and extracted v6 evidence are under
`out/xiaomi-mi-10/render-perf-checkpoint2/`.

**What v6 actually establishes.**

- `test-1110-091804-umi-v6.kernel.log:3692` explicitly says its middle was dropped.
  Its `960 made (207520 KB), 80 taken again` at line 12830 is surrounded by teardown
  I/O failures at 01:33:40, and that window's old GPU percentage is negative.
  `newBuffer()` incremented the old counters **before** allocation succeeded.
  Counters now increment only after success. This fixes the diagnostic, not allocation
  policy. New `newBo` timing counts calls, including unsuccessful calls; its KB are
  requested bytes. `new-ioctl`/`map` bytes are page rounded.
- Allocation remains a real candidate during normal operation: live snapshot `.176`,
  line 10767, reports 323 new buffers / 1,158,044 KB and 1572 pool reuses over 2 s,
  with 66 panel presentations and 615 command buffers. One command buffer is not
  necessarily one displayed frame. Pool hits/misses now have their own denominator:
  direct texture/API allocations bypass the internal CommandQueue pool. Do not divide
  the old reuse count by all new buffers to infer that pool's hit rate.
- The early `.live.45:8086` report has 21 stage compilations taking 9101.9 ms total,
  max 8902.8 ms; line 9664 has 25 compilations taking 4147.6 ms and 5287 ms compiler
  lock wait. Later zeros mean no compilation in those later windows. These establish
  cold compilation stalls, not the cause of all warm-animation lag. This checkpoint's
  iOS compiler fingerprint remains `8f53482379a4f551b403b51d`, unchanged from v6.
- **Correction to earlier sections:** the old `GPU busy` is a sum of
  `completedAt - committedAt`, and committedAt precedes CPU submission. It includes
  CPU work and waits, can overlap across queues, and is not hardware occupancy.
  The headline now says `commit-to-fence wall`. Per-pass hardware timestamps and
  `HakiosGPU: ... the GPU worked ...` remain separate evidence. The 1524 ms caller
  commit over 120 buffers is about 12.7 ms wall per buffer, not proven CPU execution.
- XNU CPU0 is physical 7 / A77 prime; CPU1–3 are physical 6/5/4 / A77;
  CPU4–6 are physical 3/2/1 / A55. v6's 71–75 us on CPU4/5 versus about 20 us on
  CPU0–3 is consistent with that mapping. `metal_big` QoS does not pin a core.

**New logs with the existing `metal_fps=1`.**

`fps: perf count/wall-ms/cpu-ms/max-ms/KB:` reports process-wide operations since
its previous snapshot. CPU time is the calling thread's CPU clock; wall includes
blocking/descheduling. Each entry has its own count; nested entries overlap, so do
not add parent and child times or divide every entry by panel presentations.

- `newBo`, `new-ioctl`, `iova`, `map`: BO allocation calls and their transport steps,
  including allocations during encoding, outside `-commit`.
- `pool-hit`: lookup plus reuse memset; `pool-miss`: failed lookup only (the ensuing
  allocation is in newBo). Command stream BO reuse is covered by `commandBo`, a
  separate pool from the temporary draw-buffer pool.
- `commit-total`: direct caller duration, with `turn`, `events`, `commit-core`,
  optional `sync`, and `scheduled`. The core contains `queue-submit`, `closePass`,
  `commandBo`, `upload`, `queue-lock`, `fault-query`, `Gpu-submit`; the last contains
  `bo-list` and `submit-ioctl`. Deferred-event buffers still report caller total and
  core queue/transport steps; their worker's plugin wrapper is not a caller span.
- `render-pipeline`, `compute-pipeline`: successful pipeline construction including
  reflection, AIR/compiler and link; `slow-pipeline` names calls over 50 ms with a
  mach-time end timestamp. Graphics `stage-cache` alone never covered compute.
  `cb-slot` measures the potentially blocking superclass command-buffer initializer,
  before the existing made-to-committed timer begins.
- Kernel `GPU-submit`: count, total/max wall, offscreen wait, main GPU-lock wait,
  copyin/allocation, cache clean and ring-submit totals in microseconds. Initial
  alias lookup and later BO bookkeeping are in the total but not separate buckets.
  If offscreen dominates, pooling will not eliminate that part of caller commit.

`fbcontent` samples 32 x 64 fixed RGB pixels of the actual DPU target after GPU
completion, before programming its flip. It compares pictures across changing
surface IDs/strides; alpha is ignored. `changed` proves different sampled content;
`same` means equal samples, **not proof of identical full images**. `broad(>=1%)`
counts changes to at least 21 samples and helps separate small icon changes from
large scene changes; it is not a CA transaction counter. These are successful flip
requests, potentially one frame ahead of confirmed `presented via panel` latches.
The first image/changed dimensions establish a baseline; timed-out GPU images are
`unknown`. Sampling is diagnostic-only with framebuffer stats; total/max sampling
cost is printed. It uses 2048 aligned volatile 32-bit carve-out reads per frame,
16 KiB ordinary-RAM scratch for the single display presenter, and no new GPU wait.
All carve-out writes and the `sid==0` / `!zero` clearing guard remain unchanged.

`fb-latency` exposes GPU-fence wait, SwapEnd-entry-to-flip-return, and flip wait
(avg/max), plus source queue capacity. The old combined `fbstat` exceeded the
192-byte kernel trace buffer and silently lost these trailing fields.

**Touch to panel, and the next supervised run.**

The source queue is at most 2 (default 2); zero-copy uses 2 aliased render surfaces
among 3 carve-out slots. SwapEnd holds the source until its flip is programmed.
The next GPU submit naming an old scanned alias waits in
`MiniIOMobileFramebufferOffscreen()` **before** taking the main GPU lock, polling
at 250 us intervals for up to 50 ms. DPU flip itself waits for any preceding pending
flip, then programs the next one. Thus there is a display dependency inside caller
commit even though scanout performs no CPU copy.

Keep v6 args (`metal_fps=1 metal_big=1 gpu_fc=1 fb_dpu=4 metal_rs=1`). For a short
latency run add the existing whitelisted `touch_lat=1`; v6's recorded full boot line
is 559 characters, 571 with this addition, below 607. No loader change needed.
Rebuild/package CA userland and the kernel together. Preserve early live snapshots
and repeat the same gesture cold and warm with identical artifacts.

Existing `tl: p/i/M/s` mark touch posting/delivery, CA and SwapEnd. New `tl: F` gives
swap ID, DPU slot, entry/ready/flip-return times and changed-sample count; `tl: L`
gives slot, flip-programmed time and when existing polling observed the latch.
Match L's programmed time to F's entry..flip-return interval and slot. Observation
is an **upper bound** on latch time, not an exact vsync/scanout/photon timestamp;
idle polling can be late. The first changed image after a touch is only a response
proxy (a clock can also change), not proof that the touch caused it. v6 had no
`touch_lat`, so a numerical end-to-end touch latency cannot be recovered from it.

Compare `presented via panel` against `fbcontent changed/broad` during the gesture;
inspect commit wall/CPU and `GPU-submit offscreen`; then compare allocation cost,
pool misses and stage/pipeline/slot maxima. This checkpoint deliberately establishes
which cost to remove next rather than claiming an unmeasured FPS improvement.

**Host validation.** `prepare.py verify`, all 279 host tests (including real fake-GPU
allocation/reuse/submission, failed-allocation counts and RGB sampling across strides),
iOS `MESA=1`, and `ca-exact-test MESA=1` passed (444 variants, 56 pipelines, zero
mismatches). The CA bundle has zero non-weak dynamically looked-up symbols under
`nm -u -m`. The final serialized kernel build passed; its four staged inputs match
the overlays (display is staged as `umi_display_impl.c`). Sampler arm64 assembly
uses `ldr w` for carve-out loads. No phone operations were performed.

## perf3 2026-11-10

This is the first pacing change after checkpoint2. It adds the display refresh interval
that CoreAnimation uses for its frame clock, keeps the existing hazard-tracked pass
chaining, and raises the framebuffer producer queue to three swaps. No boot argument was
added and no phone tools were used.

**A — refresh source.** `MiniIOMobileFramebuffer` now publishes `IOMFBDisplayRefresh`
with one fixed 60 Hz interval in both 32.32-second units and mach-absolute-time ticks.
The mode's `SyncRate` remains the measured panel rate. This gives CoreAnimation the same
clock interval as the `tl: y` refresh notifications instead of making it infer a built-in
panel clock. During a moving gesture, expect `fps:` panel presentations to remain near
the existing 55–60/s while `fbcontent`'s `same` count falls sharply from the 50–85%
windows in v7; `changed` should approach the number of panel latches. With `touch_lat=1`,
`tl: y COUNT due ...` should advance by about 16,666 us per notification, and the `tl: M`
render/commit timestamps should fall between adjacent due times rather than repeating one
animation-time interval.

**B — pass synchronization.** The CA path continues to leave `metal_pass_wfi=1` off.
Draw passes stay chained with the light LRZ tail; a full CCU/cache flush and WFI are
emitted only at a tracked read-after-write hazard or the command-buffer boundary. In
`fps:` compare `buffer holds P passes`, `F full flushes`, `B barriers`, and `C chained`:
for a multi-pass buffer, `F` should be close to one boundary flush instead of one flush
for most passes, `B` should count only real hazards, and `C` should increase. The
`commit-to-fence wall` and per-frame GPU time should fall with the removed idle waits.

**C — producer queue.** `SWAP_QUEUE` is three, so `fb-latency` now reports `source queue
3`. `SwapEnd` can queue one additional frame while the presenter waits for the next latch;
the expected result is fewer producer stalls and dropped swaps, while `to the panel` and
`latched` stay equal during a healthy run. `flip-wait` still reports the physical latch
interval and is checked separately from queue occupancy.

**Phone log lines to check.** The supervisor should capture these exact prefixes:

```
MiniIOMobileFramebuffer: IOMFBDisplayRefresh: 60 Hz,
fps: ... (N presented via panel, ... command buffers)
fbcontent: ... sampled flips, ... changed, ... same
fb-latency: ... source queue 3
tl: y ... due ... at ...
```

**Host validation and artifacts.** `make -C hakios-metal test` passed (287 tests),
`python3 devices/xiaomi-mi-10/prepare.py verify` passed, the kernel build passed, and
the CA bundle's non-weak dynamically looked-up symbol count is zero. Deliverables:

* `out/xiaomi-mi-10/umi-perf3/kernel-mach.development.bcm2837` — SHA256
  `646d0e2959c3d1656d633c4ac5b2b9b0d282348b7d965487eca050467aa02fa1`
* `out/xiaomi-mi-10/umi-perf3/root-flash-ui-ca.apfs` — SHA256
  `dcf8f1e48942b8e2349d43961a23853d811b9901de4f51a64a6dce3d67e79ed7`

The payload was built from `flash/build-20261110-115205`; its `HakiosMetal` bundle
passes the required `nm -u -m ... | grep "dynamically looked up" | grep -vc weak` check.

## Shader stalls: stable cache key, first-draw cache lookup, seed (2026-11-11)

Evidence (v21 `test-1111-100715`, v22a `test-1111-112319`): windows with specialisations had 33–39 ms passes (the
31 040-word `fixed_frag_lph_cpf` uber shader drawing while its specialisation compiled); foreground pipeline creation
waited 7.6 s in all on the compiler lock behind background specialisations (`render-pipeline` wall minus CPU equals
`compiler wait`); and the stage-cache key changed four times in one day (`build=` v17/v18/v22a), because it hashed every
header, so each GMEM/draw edit threw the phone's cache away. v22a/v23 ran `metal_gmem_draws=1` from metal-gmem/args:
their 6.6 full flushes a buffer and 34–43 us draws are GMEM passes (never chained) and dual recording, not a
regression of the default path (v21: 19–29 us a draw on CPU0–3, which are the A77s; 1 flush per 11-pass buffer).

Changes:
- Key = compiler.cpp + compiler.hpp + kernel.hpp + Mesa archives (Makefile `IOS_CACHE_BUILD`), plus
  `kStageCacheVersion` (compiler.cpp; bump it when Stage's filling, makeSampler or kChipId change) and sizeof(Sampler).
  This build: `caf82c5e60f64881e09382db` (one cold boot, then stable across non-compiler builds).
- `compileStage(..., cachedOnly)` / `hakios_metal_program_specialize_cached`: a content seen first is looked up in the
  cache on the draw's thread and used at once; only real misses go to the serial specialise queue.
- `HakiosMetalRenderPipelineState` initialises `_specBinding = -1`: at the allocator's 0 every draw of an exact CA
  pipeline queued a specialisation that failed (611 `specialise: fixed_vert_lph_spc+fixed_frag_lph_cph ... needs the
  program` lines in v21).
- Seed: `devices/xiaomi-mi-10/tools/metal-cache-pull.py --android` (supervisor, phone in Android; read-only bounded dd
  through read-usb-hfs.py) copies `/private/var/db/hakios-metal-cache/*.stage` into `userland/hakios-metal-cache`; the
  next userland build puts it at `/redwood/share/hakios-metal-cache` (build.sh prints "N compiled Metal stages seeded").

Next run, compare: `stage-cache: build=caf82c5e...`; on the second boot `N disk hits ... 0 compiled`, `compiler wait`
near 0 and `render-pipeline=` wall close to its CPU; `specialise: ... from the stage cache at its first draw`; no
`needs the program` lines; fewer passes over 20 ms in `passes timed by the GPU` at animation starts; with a seed,
`seed=/redwood/share/hakios-metal-cache` and nonzero `seed hits`.

### v25 frame pacing (2026-11-11 afternoon)

`tl:` analysis of runtime/logs/125318-run.log: every swap reached the panel (fbstat: swaps in = to the panel, 0
dropped); 83% of latches one TE apart. Of 2458 late frames, 1737 were CA's frame CPU (made→commit 10–22 ms, ~4.3 ms of
it our 127 draws at 34 us), 707 were commit→done over 16 ms (GPU passes 13–15 ms a buffer in those windows: full-screen
GMEM passes of only 18–25 draws at 10–11.5 ms). With `metal_gmem_draws=1` every draw was recorded twice (GMEM and the
sysmem fallback that bar can never pick): now a bar of 1 records GMEM only (render.cpp; host bench 2.54 → 1.48 us a
draw). `metal_fps=draws` is now also read from the gmem-args file: `metal_gmem_draws=0 metal_fps=draws` there names
the programs behind the 10 ms passes (`fps: ... draws of <program>: N, X ms on the GPU`).


## Submit wait v29

Scope: kernel submit/reuse gate and GPU telemetry. No phone operations, userland
payload build, new boot arguments, Metal encoder changes, or framebuffer composition
changes were performed by the submit agent. The small display-side change in
`hakios.display.c` is necessary: its existing `dpu_leaving()` only describes an
outgoing frame and cannot identify an incoming pending target or report whether a
wait timed out.

**Change.** `hakios.gpu.cpp` now consults the DPU only for BO entries carrying
`MSM_SUBMIT_BO_WRITE` (including READ|WRITE). `hakios-metal/src/gpu.cpp` already
sets distinct READ/WRITE bits and ORs uses of duplicate handles; it was inspected,
not changed. Read-only aliases and writes to free aliases pass immediately. All
write aliases are checked, with no two-surface limit or unlocked alias-table peek.
A blocking submit sleeps in 250 us intervals outside `gGPULock` and re-resolves its
handles and aliases after each sleep. The successful check retains `gGPULock`
through ring submission and fence publication. After 50 ms of an unresolved hazard,
the submit returns `kIOReturnTimeout` without writing the ring; the old void wait
silently proceeded after its limit. There is no new switch file: skipping a
read/read dependency is safe under the existing BO access contract, and conflicting
writes retain or strengthen the existing protection.

**Ordering argument.** The existing CA protocol is still required: `SwapWait(N)`
orders reuse after the presenter has programmed swap N, and the presenter waits
`MiniIOSurfaceDrawn()` before flipping a source. This is the existing protection
against a queued, not-yet-programmed swap being overwritten. The new display query
checks the *physical alias frame*, rather than reading framebuffer binding pointers:

- A pending target is busy even after CTL_FLUSH clears but before software notices
  the latch; after software notices it, it remains busy as the current frame.
- A current frame without a programmed successor remains busy. A missing clock or
  invalid scan cannot establish that a current frame is free.
- An outgoing current frame becomes free when its successor's CTL_FLUSH clears,
  or, in command mode, when the existing DMA-done test confirms this transfer is
  finished. The flush is read first. If a TE occurs afterwards it latches the
  successor; otherwise the two phase reads must not wrap, phase must be in the
  idle interval, and the write pointer must cover every image row. Thus the old
  frame has had its final fetch before the GPU can write it. Video mode keeps
  waiting for the successor's latch.

A small display mutex serializes normal flip programming, latch-state updates,
and the hazard snapshot. Lock order is `gGPULock` then the display mutex; the
latter never waits for a GPU fence or a TE and never acquires `gGPULock`.
Startup self-tests run before aliases exist; loader restore retains its existing
quiesced-CPU path. Alias remapping and BO retirement still use `gGPULock`, so a
surface cannot change physical backing between the successful check and fence
publication. This change does not introduce an asynchronous submit queue: that
would also require fence reservation, BO retention, and per-queue ordering before
returning to userland. Only the conflicting caller waits; the global GPU lock is
released while it sleeps.

**Counters and live log.** The five-second summary now uses `loader_log_line()`
plus `IOLog()`, following `minisandbox_say()`, with a 1024-byte formatter so the
long original “the GPU worked” line is preserved. `GPU-submit:` uses the same
formatter to retain its trailing values. Two short live-ring lines accompany it:

```
GPU-busy: 30.0%; the GPU worked 1500000 us / 5000 ms, 900 timed submissions
GPU-waits: scan 80 pending 0 timeout 0; scanout 410 ms; fence 20, 9 ms total per window
```

Numbers above are illustrative. Scan/pending count submits that encountered each
reason, once per reason per submit (a submit can count in both). Scanout milliseconds
sum wait/check time excluding GPU-lock acquisition, including rejected submits;
fence count/time retain the existing completed GPU-fence-wait definition. Counters
are global across clients, reported and reset under `gGPULock`; a wait crossing a
window boundary is charged when it ends. Existing `offscreen` remains in us.
`GPU-busy` is completed submissions' Adreno timestamp duration divided by elapsed
wall time, including idle gaps between windows. It is submission-time duty across
the shared ring, not the hardware busy-cycle counter, and excludes uncompleted or
lost timestamp samples; `gpu_exact=0` weakens its interpretation. Do not treat it
as shader-unit utilization or sum it with caller `commit-to-fence wall`.

**Good/bad supervisor checks.** Good: the complete `HakiosGPU: ... the GPU worked`
line and `GPU-busy:` are visible in the live kernel log; `GPU-waits: ... timeout 0`;
read-heavy windows have lower `GPU-submit: ... offscreen ...` and caller commit
wall, with no tearing, corruption, or display fallback. Genuine render-target
reuse can still wait until DMA-done. Bad:

```
GPU-submit: scanout write wait timed out; submission refused (reasons 1)
GPU-waits: scan 10 pending 2 timeout 1; scanout 90 ms; fence 0, 0 ms total per window
```

The first error logs once per reporting window; reasons are bits 1=current fetch,
2=pending target. A timeout means the submit did not execute, and can surface as a
client command-buffer failure; investigate repeated reuse/SwapWait ordering or a
stalled presenter instead of permitting a write into scanout. Also reject tearing,
`ALIAS FAIL`, flip timeout/fallback, GPU recovery, or sustained pending waits in an
otherwise normal alternating-frame sequence. The v28 313-submit sample spent
912/920 ms in offscreen waits, but its log does not identify write flags: the
removable fraction of those 912 ms is unknown until the supervised comparison.

**Host validation.** `test-gpu-submit.py` executes the actual submit gate and DPU
query/latch/flip functions with fake registers: read-only bypass, READ|WRITE,
current/pending/free slots, DMA-done, busy DMA, TE wrap, video mode, stale latch
observation, missing successor/clock, remap while sleeping, and timeout refusing
ring writes. It checks that GPU sleeps hold neither mutex and that a >700-byte
summary reaches both logs without truncation. Existing `test-display.py`,
`test-display-session.py`, `test-display-uefi-probe.py`, `test-backlight.py`,
`test-cpu-heat.py`, `test-sleep.py`, `test-loader-metal.py`, `test-upstream-sync.py`,
and `prepare.py verify` passed. Hardware behavior remains for the supervisor.

Build status: the required locked build initially stopped in unrelated shared
network code (`hakios.net.c:555`, `.detached` vs XNU's `.detach`). No network file
was changed by this agent. Build log: `out/xiaomi-mi-10/perf-submit-v29/build-kernel.log`.

## BO reuse v29

**Design.** `gpu.cpp` now retains a resource BO's kernel handle, IOVA and CPU
mapping together. The allocation transport owns the cache and drains it before
closing the GPU connection. The plug-in scopes a thread-local allocation key
around `newBufferWithLength:options:` and ordinary texture creation (including
rotated drawable backing); bytes/no-copy and heap entry points forward to these.
The key contains page-rounded size, GEM flags, pixel format, usage, texture type
and sample count. Buffer options and memoryless storage use separate keys.
Objective-C/C++ resource wrappers are still made normally. IOSurface handles and
aliases are never cached; queue command/slab pools keep their existing policy.
The only `gpu.hpp` change for this implementation moves the unchanged constructor
out of line to install the allocation transport; no renderer, command, compiler,
kernel or pipeline interface change was needed. `bo_cache.hpp` is C-compatible
and covered by the existing build dependencies on `*.hpp`.

Every successful submit records the BO's last fence **on every queue that used
it**, for both reads and writes. Existing successful `Gpu::wait` calls publish
completion. A released BO is reusable only after all its recorded fences have
been observed complete. Allocation does not poll or wait for a GPU fence; a busy
candidate is skipped and a new allocation is attempted. This also avoids the
kernel's zero-deadline WAIT_FENCE "over before it began" diagnostic. A successful
queue close retires already-completed fence records without forgetting pending
ones. No kernel call or zero-fill holds the cache lock during allocation/mapping.

The release LRU is bounded per Gpu/device at **256 MiB and 512 BOs**, including
pending BOs. Memoryless backing has a **128 MiB sublimit within those 256 MiB**,
enough for two approximately 56 MiB full-screen MSAA attachments.
Oversized resources bypass retention. Eviction unmaps/closes the oldest BO; the
existing kernel deferred-close path retains GPU backing until its last use
retires. Live resources are additional memory, and the kernel's existing 160 MiB
pool is additional retention. Metal's currentAllocatedSize keeps its previous
live-resource meaning; the new cached-byte gauge accounts for retained BOs.
Multiple devices/processes have separate budgets.

Ordinary buffers/textures retain GEM_NEW's zero-initialization behavior on hits.
Existing `kBoUncleared` attachments and memoryless backing skip that CPU clear.
**Memoryless still needs backing:** MAPS-umi.md's whole-pass replay can turn a
GMEM pass into sysmem even when its original store action was DontCare. Eliding
backing at allocation time would break replay, stencil and resolves. Single-sample
memoryless attachments now also request the existing uncleared allocation flag.

Default is enabled. To compare with previous allocation behavior, the supervisor
can create `/var/db/hakios-metal-cache/bo-cache-off` before starting the process;
remove it and restart to enable reuse. The file is sampled once per process.
There is no new boot-arg.

**Near-size reuse (v29i).** Exact-size keys left the cache unable to serve the
same window one animation row later: the Control Center window allocated 68 BOs
with 225 MiB cached. An idle BO with the same key and flags may now serve a
request up to one page (or an eighth, for large requests) smaller; the smallest
such BO is chosen, and nothing is allocated larger, so retained bytes do not
grow. The `bo-cache:` line gains `fit`/`waste` (fit hits and the summed slack
handed out, not memory). `/var/db/hakios-metal-cache/bo-cache-fit-off` turns it
off, sampled once per process. Analysis: `work/v29i/IMPL-bo-cache-hitch.md`.

**Value-only counters.** With existing `metal_fps=1`, resource allocation emits
`bo-cache:` at approximately two-second allocation-triggered intervals. Counters
are process-wide; a quiet interval is reported on the next allocation. It gives
allocation requests, hits, successful backend GEM_NEW requests (`new`), real
map/unmap attempts, LRU evictions, skipped matching busy candidates, memoryless
hits, and backend allocation/map/unmap/zero milliseconds. Cached bytes,
memoryless bytes and object count are gauges and do not reset. Concurrent work
can straddle a reporting boundary. Existing `fps: perf ... newBo` still gives
total wall/thread CPU; `new-ioctl`, `iova`, `map` now count actual backend calls,
so a cache hit adds none of those calls. Map/IOVA counters also include imported
surfaces; no surface identity or pixel value is logged.

`new` is **not proof of a new physical allocation**: GEM_NEW may hit the kernel's
160 MiB pool. Use the existing kernel line
`HakiosGPU: buffers: N made in T us each, R kept ones given again (M MB kept)`
for physical allocation misses (`made`) and kernel reuse. The current userland
ABI does not expose that distinction per process; no new kernel query was added.

**Good/bad lines** (numbers illustrative):

```
bo-cache: window 2001.0 ms allocations 80 hits 78 new 2 maps 2 unmaps 0 evictions 0 busy 0 memoryless-hits 40; new 1.0 map 0.4 unmap 0.0 zero 2.0 ms; cached 114688 KiB memoryless 114688 KiB objects 4
bo-cache: window 2002.0 ms allocations 80 hits 0 new 80 maps 80 unmaps 80 evictions 80 busy 160 memoryless-hits 0; new 500.0 map 600.0 unmap 10.0 zero 0.0 ms; cached 245760 KiB memoryless 114688 KiB objects 8
```

The first is useful warm reuse; confirm existing `newBo` wall/thread CPU and
kernel `made` decrease on the same workload. The second indicates pressure,
keys changing, or completion observations lagging; it does not justify reusing
busy memory. A busy count is candidate skips, not unique allocation misses.
Reject GPU faults/timeouts, bad Maps clears/resolves or stale Weather frames.

**Expected gain and limits.** A hit eliminates GEM_NEW, IOVA lookup, map and
eventual unmap/close for that lifetime; attachment hits also avoid zeroing.
At hit fraction h, the v28 Weather window's 621.1 ms mapping wall is an upper
reference of roughly `h * 621.1 ms` avoidable mapping work. The enclosing
1649.8 ms `newBo` span overlaps it and must not be added to it. Actual improvement
needs the supervisor's workload comparison; retained ordinary-texture zeroing,
live working sets above budget, late completion observations and new keys reduce
the benefit. The 3.93 GB/80-attachment figure is cumulative allocation traffic,
not evidence that a 3.93 GB cache is needed. Shader compilation, submit/scanout
waits and Objective-C wrapper creation are separate costs.

**Validation.** Ten new `bo_cache_test.cpp` regressions exercise handle/IOVA/map
identity, rounded-size/format/usage/flags separation, ordinary zeroing, undefined
memoryless contents, last read/write use across queues, paused asynchronous GPU
work, failed creation/map/submit/wait, queue close, LRU byte/object/memoryless
limits, counter reset, thread-local scope, IOSurface duplicate-import disown and
cache teardown. Required commands all run through root `build-lock.sh` from
`work/umi-v23`:

- `build-lock.sh make -C hakios-metal test`: latest shared-tree run **321 tests,
  0 failed checks** (`/tmp/bo-cache-host-test.log`).
- `PATH=/opt/homebrew/opt/llvm@16/bin:$PATH build-lock.sh make -C hakios-metal test MESA=1`:
  **321 tests, 16 known failed checks**, all ten BO-cache tests pass
  (`/tmp/bo-cache-mesa-test.log`). Counts include concurrent agents' additions.
- `build-lock.sh make -C hakios-metal ios MESA=1`: core archive and probe pass
  (`/tmp/bo-cache-ios-build.log`).
- `build-lock.sh bash /tmp/hakios-bo-cache-check/compile-plugin.sh`: passes,
  using the overlay build.sh SDK/framework/Mesa link flags including
  `HAKIOS_UMI=1`; `nm -u -m` reports **0 non-weak dynamically looked up symbols**.
  Artifact, script, undefined-symbol listing and compile.log are in
  `/tmp/hakios-bo-cache-check/`.

No phone access or full flash/userland payload build was performed. Hardware
rendering and the Weather timing comparison remain for the supervisor.

## Encode/GMEM v29

本次范围：`hakios-metal/src/{render,command}.cpp`、对应 headers、两份回归测试。
未访问手机，未修改 GPU/BO 分配、shader compiler、插件或内核，未运行 userland payload build。

**策略与开关。** 继续通过已有 `/var/db/hakios-metal-cache/gmem-args` 的整数解析传值：

| 文件中的值 | 行为 |
| --- | --- |
| `metal_gmem_draws=0` | UMI 现有默认，sysmem（同时不要设置 `metal_gmem=1`） |
| `metal_gmem_draws=1` | 所有合格 draw pass 优先 GMEM，作为 v28 对照 |
| `metal_gmem_draws=999999` | 新的 adaptive 模式；不是 draw 数阈值 |
| 其他正整数 | 保持原有 draw 数阈值策略 |

Adaptive 首先遵守原有 `gmemPlan` / `gmemDrawOk` 的资格检查及 `metal_gmem_off`
位。合格 MSAA pass 优先 GMEM；单采样目标面积不超过 256×256，直接 sysmem。
其余 pass 第一次保留两种 recording，结束时只有 **至少 64 draws 且累计
viewport/scissor 面积达到整个目标面积的两倍** 才选 GMEM。这是覆盖量的上界代理，
不是实际 primitive coverage / overdraw 测量。全屏 22-draw pass、单层 copy 和 corner
pass 因此留在 sysmem。按 target 格式/尺寸及同类 pass 在 CB 中的 ordinal 保留近期
peak draws：已经见过且 peak 小于 64 的 pass 只编码 sysmem；若本次增长到 64，
下一次重新评估。沿用已有 16 次下降后更新 peak 的机制。

默认保留 0：v28 的 22 draws / 8.22 ms 说明强制 GMEM 有明确代价，但没有本次真机
验证足以把新启发式设为默认。Adaptive 不覆盖 MSAA fetch 等明确的资格拒绝。
Maps 的修复保留：depth/stencil 或 DontCare store 的 GMEM pass，始终从**原始 load
和 Clear** 保留整个 sysmem recording；后续 draw 不能进 tile 时整 pass 重放，
包括此前几何、colour clear 和 Z/S clear。新增 adaptive 回归覆盖这个路径。

**同步。** GMEM pass 已经在 `encodeGmem` 内完成 resolve、CCU invalidate 和切回
sysmem 的 WFI，可以用 light tail 接入原来的 hazard chain。独立的后续 sysmem、
GMEM 或双 recording pass 可省完整 tail 和 batch start；有跨 cache 写依赖，或
incoming GMEM 的首 draw 读取 chain 输出，仍结束 chain。后续 draw 才读取 chain
输出时也发 barrier，包含 GMEM body 执行前的情形。resolve 目标计入 dirty spans；
WAR 仍等读完成。copy、blit、dispatch、CB 结束仍完成完整 tail。
**没有删除** per-tile framebuffer-fetch invalidate/WFI、tile store WFI、CCU mode
切换 WFI 或 `RM6_BIN_END_OF_DRAWS`。`metal_pass_wfi=1` 的原有全同步对照仍有效。

**编码与提交。** 每 CB 缓存 pipeline 的 VS/FS preload、寄存器和立即数 packet
blocks；同一 pipeline 无 IBO 重新配置需求时不重复发 preload/invalidate。
GPU program 上传地址和 private-memory 地址/布局都属于状态键，shader 内容改变
或 scratch 增长会使状态变脏。texture/sampler 按实际 descriptor bytes 比较，
VFD 按实际 fetch/decode、offset、restart index 比较，viewport/scissor 独立跟踪；
indirect draw 后丢弃 VFD shadow。buffer pushed constants、UBO descriptors 和
VS driver params 仍逐 draw 加载；同一绑定地址上的新常量不能被当成未变化。
BO 使用列表按 BO 与 READ/WRITE 位各保留一次，texture buffer retention 去重，
避免列表随 draw 数增长；下游 GPU submit 的 handle 合并代码未修改。

同队列**已经 ready 的并发 commit** 可以一次提交：每 CB ≤8192 words、≤16 draws，
每批最多 8 CB / 32768 words。保留各 CB 的末尾等待，顺序串接命令、合并 BO uses，
共享真实 fence，按原顺序回调；每个 commit 返回前 fence 已发布。失败批次全部
返回错误，下一批仍可提交。没有定时攒批、虚拟 fence 或提前 scheduled。
**原同步路径的串行 commit 无法合并**：该结论现已由下文 Batched serial commit v29
的依赖驱动 staging 接入取代。旧 C API barrier 仍在返回前发布真实 fence；插件的私有 CB
改用独立 stage API，surface/CPU/跨队列依赖、上限和 idle 路径负责 drain。

**值计数。** 保持现有 `metal_fps` profiling 配置，queue 每两秒（有后续提交时）
在 stderr 输出一行；没有 pipeline 名称或逐 draw trace。示例值仅说明格式：

```
encode-v29: cb=120 submit=110 merged=10 sysmem=600 gmem=80 wfi=2400 wfi_per_cb=20.00 chain=400 draws=10000 encode_us_per_draw=24.00
```

`sysmem/gmem` 只计 draw pass 最终执行的模式；`chain` 是省 batch start 的 pass 数，
`merged` 是成功合并掉的 submit 数，两者不同。`wfi` 按实际 PM4 计数，递归计算
GMEM IB 在各 tile 和 binning 中的重复调用；包含必要的 tile/fetch 等待，不仅是
full-tail 数。`encode_us_per_draw` 是核心 draw() 成功调用的 CPU 侧 wall time，包含
其中分配/锁等待，排除插件及 deferrer 排队、closePass 和 submit。WFI 与 encode
计数在 profiling 开启时采集；它们也通过 C++ `CommandBuffer::counts()` 可读。
文件里的 `metal_fps=draws` 会故意逐 draw 加 WFI，不适合正常性能对比。

**supervisor A/B（本 agent 未执行）。** 保留原有 profiling boot 配置，不增 boot-args；
分别把 gmem-args 中的 `metal_gmem_draws` 改为 0、1、999999，保留其他开关一致，
每次重启受测进程（文件按进程缓存；backboardd 由 supervisor 安排重启/测试启动）。
同样的 unlock、Control Center、Maps 平移/缩放操作，热 shader cache 后比较
`encode-v29`、原有 fps/panel presents、GPU-busy 和 kernel wait 统计。

好信号：adaptive 的轻 pass 变 sysmem，重 pass / 合格 MSAA 仍有 GMEM；独立 pass
的 `chain` 上升、`wfi_per_cb` 下降，encode 时间下降；Maps clear/填充及 resolve
正确，无旧帧累积。`merged=0` 在串行 deferrer 上正常，不代表实现失效。
坏信号示例（数值为示意）：

```
encode-v29: cb=120 submit=120 merged=0 sysmem=0 gmem=1080 wfi=12000 wfi_per_cb=100.00 chain=0 draws=2640 encode_us_per_draw=45.00
```

若在 adaptive 下仍出现这样的全 GMEM 22-draw 分布，检查文件内容、进程是否重启，
以及是否运行了新 core。GPU fault/timeout、gmem-trace 停止、binning overflow、
Maps 缺面/clear 丢失、旧帧累积或 tearing 都是拒绝本次真机结果的信号；不要删
必要 WFI 来掩盖。MSAA-fetch-on 的独立设备验证要求仍见 MAPS-umi.md。

**主机验证与预计收益。** 所有构建/测试经过根目录 build-lock.sh；新增功能回归
覆盖 adaptive 的尺寸/draw/coverage 组合、混合模式 chaining 和后续 RAW、mutable
sampler/VFD、相同绑定的新常量，以及 gated ioctl 下小 CB 合并、READ|WRITE、
错误传播、fence 返回时机和回调顺序。Maps 原回归扩展到 1/2/4 samples 与 adaptive。
主机微基准（50×200 draws，-O2，同一份其他 agent 的 gpu.cpp，比较本次改动前后
的 render/command）：纯色 1.243→1.162 us/draw，纹理 1.264→1.196；200 draws
的流分别 5128→1548 和 7531→1563 words。约 5–7% 主机编码收益，不能外推为
真机 FPS；手机上减少 descriptor 写、BO-list 膨胀及 GMEM 轻 pass 更可能占主要收益。
基准脚本、源码、输出与构建测试日志在 `/tmp/encode-v29/`。

最终锁定验证：普通 **321 tests, 0 failed checks**；Mesa **322 tests, 16 failed checks**，
16 条均与本次此前记录的已知 compiler/fixture 失败相同，新增回归全部通过；
`ios MESA=1` archive 和 probe 重编译/链接通过，exit 0。
日志：`/tmp/encode-v29/{host-final,mesa-final,ios-final}.log`。
普通测试首次遇到并行 BO agent 新计时回归 `HM_BO_ZERO_NS > 0` 的一次失败，
最终普通全量运行通过；本 agent 没有修改该测试或 BO 代码。

## Pipeline latency v29

本节仅涉及 `hakios-metal/src/compiler.cpp`、编译器头文件及
`HakiosMetalCA.m` 的 pipeline/library/function 创建与 stage-cache 路径。
保留 MAPS-umi.md 的 `fmedian3` 和进程内失败 stage 负缓存。没有访问手机、
修改 kernel/render/command/gpu 或分配路径，没有新增 boot-arg，也没有构建完整 userland payload。

**异步创建（默认启用）。** render/compute 的 completion-handler 形式现在只在调用线程
复制 descriptor，然后把实际创建和 callback 提交到 `QOS_CLASS_USER_INITIATED` 的后台队列。
compute 的 function 形式保留不可变 function；source library 复制 source/options。
同时覆盖 source/stitched library、`_MTLLibrary` 的 named-constant function、function
与 intersection descriptor 异步形式，以及 mesh/tile 的异步拒绝。每个任务有 autorelease
pool；GCD 保留 device、输入和 handler，结果的 +1 引用在 callback 后释放。回调恰好一次，
成功/错误/reflection 语义沿用同步入口。回调可能在调用尚未完全返回时开始，但不在调用线程执行。

**前台优先（默认启用）。** 不并行执行 Mesa。已核对 `ir3_compiler::shader_count` 的
共享可变状态、`compileStage` 临时改写 `has_preamble`，以及已有 argument-buffer 并发
损坏回归；原 mutex 的单次执行约束仍然必要。`CompilerQueue` 是优先准入门：每次只允许
一个 Mesa 调用，运行中的调用结束后，所有已等待的前台请求先于 background 请求准入。
compute、graphics stage、argument-buffer layout 都经过同一门；磁盘/seed/RAM 命中和
只读 constant-index 元数据解析继续绕过编译门。取得门后再次查 stage/负缓存，避免重复编译。
这是调用线程上的串行执行，不依赖 Mesa 的跨线程可重入性，也没有同时运行多个 compiler。

API 请求（包括异步 API）算前台；draw-time specialisation 是 utility 队列上的 background
工作，并恢复线程原来的优先类别；prewarm 也算 background。优先级改变的是**排队顺序**，
不能中断已开始的 203–450 ms 编译。前台不再排在一串特化/预热后，但仍可能等一个正在
执行的编译及其他前台请求；持续前台负载也可能延迟预热。每个进程分别仲裁，未增加跨进程锁。

**进程预热（显式启用）。** supervisor 可在进程启动前创建
`/var/db/hakios-metal-cache/prewarm-on`，并重启目标进程；去掉文件并重启即可关闭。
没有该文件时不记录 recipe、不运行预热；正常 stage cache、异步创建和前台优先照常工作。
此开关是因为推测编译会增加启动 CPU/热量、磁盘与 RAM 占用。路径使用当前
`HAKIOS_METAL_CACHE`；通常就是现有 `/private/var/db/hakios-metal-cache`。

开启后，设备首次初始化立即启动 utility 预热任务。成功的常规 stage 请求（包括磁盘命中）
记录完整 AIR、按 index 稳定排序的函数常量、IBO base、known-buffer binding/colour outputs/
已知字节与 mask、sample count；未知字节归零。每进程最多记录 256 个不同 stage，recipe
位于 `warm-<uid>-<executable-path-hash>/*.warm`，使用独立临时 inode + rename 发布。
cache-only 的 first-draw 探测不会写 recipe；真正的 background specialisation 编译会记录。
失败 stage 不写 recipe，负缓存仍只在进程内生效。

首次使用没有历史 recipe，先完成一次学习运行，下一次进程启动才能提前预热。recipe 文件名
独立于 compiler build，重放时使用**现有 stageCacheName** 重新计算当前 build 的键，仍先查
writable/seed cache，真实 miss 才编译。没有另造 `.stage` 格式或修改原键语义；compiler
fingerprint 改变时旧 stage 不会被误用，旧 recipe 可以生成新键的 stage。不同 executable 的
使用记录隔离，实际 `.stage` 文件照旧共享。此 compiler.cpp 修改自身会造成一次缓存键转换。

预热成功的最多 256 个 stage 留在进程 RAM 中；创建 pipeline 或 first-draw cache-only 请求
取得拥有独立 vectors/pointers 的副本，不等待 compiler。预热只做 CPU 编译/文件访问，
不创建 GPU pipeline/BO、不提交 GPU 工作。RAM 大小取决于 stage，recipe/stage 文件持续留在
磁盘；应用更新后的旧 recipe 可能做无用编译，开关关闭后可由 supervisor 清理对应 warm 目录。
不能预测从未用过的变体，也不能保证进程立刻收到请求前所有预热均已完成。

**值计数。** 随现有 `metal_fps=1` 的 stage-cache 窗口输出一行（数值仅示例）：

```
pipeline-queue: foreground-wait-ms=0.2 queue-depth=0 queue-peak=1 prewarm-hits=24 prewarm-loaded=18 prewarm-compiles=0 prewarm-failures=0 recipe-write-failures=0
```

`foreground-wait-ms` 是该进程所有前台 Mesa 请求的准入等待总和，包括 compute/layout；
`queue-depth` 是报告时等待准入的请求数（不包括正在执行者，也不包括尚未进入编译器的
GCD 队列），`queue-peak` 是窗口内准入等待数峰值。其余计数为 RAM 预热命中、成功装入、
需请求编译的 recipe、预热失败和 recipe 写失败。预热批次的 loaded/compiles/failures 在任务
完成时发布；命中及普通 stage 计数在调用完成时计入。等待跨窗口时记在取得准入的窗口。
统计是进程范围，不输出 shader/像素/对象内容；原 `stage-cache` 的磁盘/seed/miss/compile
计数保持原定义。

**好/坏日志与预期收益。** 开启时出现 `stage-prewarm: enabled, replay queued`；第二次启动
同样 UI 操作预期 `prewarm-hits` 非零、`prewarm-compiles` 接近零、前台等待下降。
保留 `specialise: ... from the stage cache at its first draw`。
下面提示预热未完成、compiler build 刚变化、I/O 或 recipe 问题，需要结合原 stage-cache 行定位：

```
pipeline-queue: foreground-wait-ms=421.6 queue-depth=4 queue-peak=7 prewarm-hits=0 prewarm-loaded=0 prewarm-compiles=20 prewarm-failures=2 recipe-write-failures=1
```

单个已开始的 background compile 仍可让前台等待约一个 stage 的时间；不能仅凭该示例断言
优先级失效。`slow-pipeline: render ... 421.6 ms` 现在也可能来自后台 async API，表示创建耗时，
**本身不再证明 caller 被阻塞**。对使用 completion-handler 的 v28 240.8/421.6 ms 创建，
预期从调用线程移走整个 build 时段；总编译 CPU 时间未减少。优先门消除已排队后台工作的
累积等待；预热在请求前完成时，把对应首用变成 RAM 命中。需要 supervisor 对比相同动作的
caller pipeline wall、foreground wait 和首开长帧，不能宣称已测得 FPS 或真机延迟提升。
拒绝 GPU fault/timeout、callback 丢失/重复、pipeline 结果错误或持续 recipe/prewarm failures。

**主机验证。** 所有构建/运行经根目录 `build-lock.sh` 串行化。最终完整普通套件
**321 tests, 0 failed checks**；Mesa **322 tests, 16 failed checks**，仍是已知的五个
compiler/fixture 测试中的 16 个检查。数目较任务基线增长来自其他并行 agent 的测试；
本 agent 没有修改它们。早期曾遇到并行修改尚未完成的 BO 计时与 render 测试编译失败，
最终重跑已消失。`ios MESA=1` 通过，单独 CA bundle 编译链接通过，
`nm -u -m` 的 nonweak dynamic lookup symbols **0**。未运行已知不能编译的 pencil 测试。

独立回归保存在 `/tmp/hakios-pipeline-v29/`，避免改动其他 agent 拥有的测试文件：
`gen-async.py` 提取实际异步实现，Foundation/Metal host harness 验证 15 个入口的非调用线程
callback、恰好一次、可变输入快照、device/input 保留、成功/错误/reflection；
`core-test.cpp`/`tests.mk`/`run-core.py` 使用真实 compiler，验证 100 轮前台越过 3 个已排队
后台请求、单次编译执行、开关关闭、进程隔离、跨进程记录/预热、改变 compiler build 后
保留并忽略旧键、删除 stage 后重编译、常量顺序/known mask/sample count 的键及副本存储。
全部通过。日志为 `test.log`、`test-mesa.log`、`compiler-tests.log`、`core-tests.log`、
`async-tests.log`、`ios.log`、`plugin.log`；bundle 为 `/tmp/hakios-pipeline-v29/HakiosMetal`。
未测手机上的热量、RAM、callback 调度和首开实际延迟；需 supervisor 打包核心和插件一起验证。

## Batched serial commit v29

**最终规则（2026-11-11）。** 已实现普通串行 CA commit 的依赖驱动合批，无需 CA 帧边界。
插件仍按 enqueue reservation 交付 committed ready prefix；新增
`hakios_metal_queue_stage_batch` 同步排完该前缀的 deferred recording，随后将已关闭 pass
的 CB 保留在核心队列的 staged 列表。私有资源 commit 返回时可以只有 Committed 状态；
没有虚拟 fence，也不提前报告 Scheduled。以下边界按原顺序 drain 整个待提交前缀：

- `surface`：实际 BO write list 指向 IOSurface backing，或 CB 注册了 presentDrawable。
  surface 来源保存在共享 Buffer 上，纹理 view/别名和 parallel append 仍使用同一 backing。
  全部 IOSurface 写入均作为外部可见边界，不猜测跨进程观察者；触发 CB 的 commit 返回前
  必须完成真实 submit，kernel 已发布 surface fence。present 延续原来的 GPU 完成后调用 drawable
  present，防止显示半 resolve 的 surface。
- `cpu_sync`：waitUntilScheduled / waitUntilCompleted、scheduled/completed handler、
  buffer contents / didModifyRange、texture contents / getBytes / replace、插件 IOSurfaceLock、
  resource waitUntilComplete、SharedEvent 信号/等待及 encoder fence 入口。
  commit 前的 handler/present/event/fence 只标记该 CB；交付时把它和前面的私有 CB 一起 drain。
  commit 后增加 handler 或 wait 会立即 drain。标记与 queue staging 用同一插件队列锁排序，
  避免 wait 落在“已经快照 idle reason、尚未 stage”之间而漏交付。
  资源的 CPU 访问先 drain 引用它的队列，再等待真实 GPU fence；错误不作为同步成功。
  CPU 解引用 argument set、非对齐或短常量的复制也先同步其 GPU writer；完整且对齐的常量
  保持 GPU 读取，可以与同队列 producer 合批。
- `cross_queue`：编码时首次记录 BO use，或已预录制 CB 被交付时，检查其他队列的 BO 读写依赖。
  RAW/WAR/WAW 都先 drain producer 并等待其真实 GPU fence，使另一队列读到最新数据；
  同队列 GPU 依赖保持 CB 顺序，可留在一批。只读/只读不会触发跨队列同步。
- `limit`：最多保留 8 CB / 32768 words；words 包含 closePass、每 CB 必要尾部和串接 NOP
  的预算。下一 CB 会超预算时先 drain 当前前缀，达到上限时立即 drain。
  原 merger 的单 CB ≤8192 words / ≤16 draws 门槛不变，大 CB 仍单独真实提交。
- `idle`：提交线程注册一次性 CFRunLoop observer，在 `kCFRunLoopBeforeWaiting` 或
  `kCFRunLoopExit` drain，覆盖 commit 后没有下一 CB 的空闲情形；不是定时器。
  event wait 使用的 ordered-submissions worker 在每个 ready batch 交付末尾显式 drain，
  不依赖 dispatch worker 运行 CFRunLoop。销毁核心队列也先 drain。

**提交、回调与资源生命期。** 原 `hakios_metal_queue_commit_batch` / 单 CB commit
仍是同步真实 fence barrier，继续服务已有立即提交/等待路径；包括 YCbCr GPU 转换等路径。
Maps 整 pass 的记录/重放逻辑未改：staging 先运行原 closePass，保持原 PM4 尾部和 WFI，
需要 completion 的既有 wait 现在也会 drain。scheduled 回调由核心队列 worker 在真实提交
成功后、等待 GPU 完成前按队列顺序交付；完成回调在真实 fence 完成后按同一顺序交付。
录制/submit 错误仍在 ordered completion 队列退休，不发 scheduled handler，也不生成 fence；
插件会唤醒等待者并保留 Error 状态。handler 在原指定 completion queue 上运行的行为保留。

待提交列表强持有 CommandBuffer，CB 强持有其 backing Buffer/texture/上传数据。
即使应用释放资源，BO 也不会进入 cache idle list；提交后仍由原真实末次 fence 规则限制复用。
queue-owned slab/command BO 也继续只在 completion 后归还。没有改变 BO 分配或 cache 算法。
回退文件沿用 `/var/db/hakios-metal-cache/batch-commit-off`，存在时插件逐个同步提交；
按进程缓存，切换后重启受测进程。没有新增 boot-args。

**计数器。** 沿用 `metal_fps` 与两秒 `encode-v29` 窗口：

- `batched_commits=`：一次非空 barrier/drain 交付多个 CB 时计入该组 CB 总数，包含错误项；
  单 CB 不增加。实际少掉的 submit 数仍看 `merged=`。
- `cb_per_drain=`：非空 barrier/drain 平均交付 CB 数，包含错误项；不计内部 recording 同步。
- `drain_reason=surface:N,cpu_sync:N,cross_queue:N,limit:N,idle:N`：每类非空 drain/barrier 次数，
  一次只计触发它的原因。旧同步 commit/barrier 计 cpu_sync。交付计数归入交付窗口，
  `cb/submit` 仍按实际成功 submit 统计，日志跨窗口时两者可能不同。

格式示例（不是实测数据）：

```
encode-v29: cb=120 submit=15 merged=105 sysmem=120 gmem=0 wfi=240 wfi_per_cb=2.00 chain=0 draws=120 encode_us_per_draw=1.00 batched_commits=120 cb_per_drain=8.00 drain_reason=surface:10,cpu_sync:0,cross_queue:0,limit:5,idle:0
```

**A/B 方法和预期收益。** 使用相同核心/插件、shader cache、原 metal_fps 配置和交互序列；
分别测试回退文件存在/不存在，每次重启受测进程。记录 unlock、Control Center、Maps 平移/缩放、
旋转和 Weather 的 `batched_commits`、`cb_per_drain`、各 drain_reason、`merged`、submit 次数，
并对照 commit wall、panel presents、GPU-busy、GPU wait。没有手机操作或新增 boot 配置。
8 个合格私有小 CB 可由 8 次真实 submit 降为 1 次，减少该组 87.5% submit；
surface/CPU/跨队列边界更多时收益相应减小。`cpu_sync` 或每 CB surface 占主导时，
`cb_per_drain=1` 仍可能正确；用原因计数解释，不能凭此声称 FPS 已提高。
必要的逐 CB flush/WFI 未删除，不能由 `merged` 推断 WFI/CB 已下降。

**主机验证。** 原 7 个 batch 底座测试保留；新增 9 个核心/C API 测试覆盖串行私有 CB
的 8+3 合批、words/NOP 边界、IOSurface/present 的 fence-before-return、跨队列读、
CPU resource sync/wait、真实 submit 之后才 scheduled、deferred recording/错误的回调顺序、
contents/modify/getBytes 读回、未提交 BO 不复用及完成后可复用，以及 GPU 写入的非对齐
常量在 CPU 编码复制前同步。FakeGpu 暂停期间仍能返回私有 commit，且不发 scheduled。
`tests/batch_plugin_test.py` 提取实际插件队列、wait、handler、present、event/fence 方法，
在 Foundation host harness 验证串行合批、enqueue/event 顺序、回调顺序、真实 CFRunLoop
空闲 observer、wait 与 staging 的确定性竞争、scheduled handler 重入 waitUntilScheduled/追加完成回调、
失败项不发 scheduled，以及回退开/关两组。

工作目录 `work/umi-v23`，构建经根目录 `build-lock.sh` 串行化：

- `build-lock.sh make -C hakios-metal test`：**337 tests, 0 failed checks**（原基线 328/0）。
- `PATH=/opt/homebrew/opt/llvm@16/bin:$PATH build-lock.sh make -C hakios-metal test MESA=1`：
  **338 tests, 16 failed checks**；逐条与原 329/16 基线相同，均为既有 compiler/fixture 失败。
- `build-lock.sh python3 hakios-metal/tests/batch_plugin_test.py`：开/关两组通过。
- `build-lock.sh make -C hakios-metal ios MESA=1`：archive/probe 构建通过。
- `build-lock.sh bash /tmp/hakios-dependency-v29/compile-plugin.sh`：沿用 BO reuse v29 的
  SDK/framework/Mesa flags（含 `HAKIOS_UMI=1`），插件编译链接通过；`nm -u -m` 检查
  **0 non-weak dynamically looked up symbols**。

日志、构建/符号检查脚本和 bundle：`/tmp/hakios-dependency-v29/` 下的
`host.log`、`mesa.log`、`plugin-tests.log`、`ios.log`、`plugin.log`、`compile-plugin.sh`、
`undefined.txt`、`HakiosMetal`。核心与插件需要一起打包。

**真机风险与待验证。** 确认实际 CA commit 线程的 runloop idle drain、surface fence/SwapEnd
顺序、handler 重入/调度、跨队列依赖与 SharedEvent/fence、错误后继续提交、缓存资源稳定性。
跨队列真实 fence 等待可能增加 CPU 阻塞，需结合 cross_queue 计数评估。
重点看 Maps 重放/clear/resolve、旋转和 Weather 是否有旧帧、撕裂、GPU fault/timeout；
同时确认实际 cb_per_drain 与 submit/commit wall 收益。未访问手机。

## v29-pencil（2026-11-12）

**已修的代码缺陷。** 私有 CB 的 idle drain 原先只注册 CFRunLoop observer；dispatch
工作线程没有运行中的 runloop，最后一批可以一直停在 Committed。现在这类线程在原
`ordered-submissions` 串行队列上合并一次异步 drain；运行中的 runloop 仍走 observer，
默认 batching 保持开启。另一个缺陷是 texture `isComplete` 在非 GL 进程恒为 YES，
`waitUntilComplete` 只 drain，没有等待 GPU。现在两者查询/同步核心 BO 的 committed
uses，覆盖 render-target 写入、texture view 和同设备 IOSurface 重复导入。

**Notes 与 Freeform。** 使用仓库中 20F66 完整 dyld cache 反汇编：
`PKMetalRenderState commitAndPurgeResourceSet:`（`0x1b72133b8`）按顺序提交 mask、compute、
main CB，最后一个 CB 的 completion 退休资源。实时路径
`finishLiveStrokeAndPresentDrawable:waitUntilScheduled:`（`0x1b72c5af8`）展示 drawable；
完成笔画由 `updateTile:strokes:clear:waitUntilCompleted:offscreen:stopBlock:`
（`0x1b72d1608`）重绘，controller 在 `0x1b72221a8..1b72221b4` 明确传入
`clear=YES, waitUntilCompleted=YES`。tile framebuffer 是非 memoryless、单采样 IOSurface；
`PKRendererTile _lockAndSetFramebuffer:` 在 `0x1b733bf84` 把其 surface 设为 CALayer contents。
QuartzCore `CA::OGL::MetalContext::finish_image` 在 `0x188362058` 确实调用 texture
`waitUntilComplete`，因此该 SPI 不能假报完成。Freeform 的 ObjC metadata 则有
`CRLBrushStrokeDirectMetalRenderingContext`（持有 CAMetalDrawable）、`CRLMetalContext`
和自己的 freehand renderer，不能把它的正确结果当作 Notes tiled renderer 的验证。

**418 抬手消失尚未定位闭环。** 09:30 已复制主机上的完整运行日志至
`logs/v29-snap/v29-pencil-0930-085506-run.log{,.live}`。408/418 都有 sixChannelBlend、
paint、erase、copy_framebuffer pipeline；418 的三个关闭开关已生效。
R8/memoryless 两句日志是 GMEM 资格与 sysmem backing 说明，不是拒绝执行；统计为 sysmem。
shadow 统计只有读入、零写回，没有证明写回覆盖。上述两个同步缺陷由主机回归证实，
但 tile 路径本来就要求 CB completion，现有日志没有逐 tile 的像素/提交证据，不能声称
这两处修复已经解释或消除了 418 的持久笔画丢失；也没有据此更改 load/store、R8 或 drawable reuse。

**回归与构建。** 修复原本不能编译的 `tools/test-metal-pencil.py`，现在运行实际插件方法的
batch 与 texture SPI harness。dispatch 无 runloop commit 和暂停 GPU 的 resource wait
均覆盖；恢复旧决策的两个对照 harness 分别在“应已 submit”和“仍应 incomplete”断言失败。
核心测试覆盖无 sampled binding 的 RT 写、view/IOSurface alias、真实 fence 等待，以及
R8 Clear/DontCare 辅助附件与持久 canvas 增量复制后旧/新像素同时保留。所有构建经根目录
`build-lock.sh`：普通 **338 tests / 0 failed checks**；MESA **339 / 16**，保持既有
compiler/fixture 失败；pencil wrapper 全通过；`make -C hakios-metal ios MESA=1` 成功。
独立 bundle 的 `nm -u -m` 为 **0 non-weak dynamically looked-up symbols**。
日志、反汇编、对照 harness、构建脚本、bundle 位于 `/tmp/hakios-pencil-v29/`；核心 archive
为 `hakios-metal/out/ios/libhakios-metal.a`。核心与插件需一起打包；未构建完整 payload。

**主管验证。** 默认 batching 开启、fresh MobileNotes 进程，检查指下 trail 稳定、抬手后
笔画保留、下一笔不抹掉上一笔、画布不黑；再对照 batch-off 和 Freeform。保留 408/418
这两个症状的区分与完整日志。现有开关按进程采样；本任务没有访问手机或修改内核。

## v29-maps（2026-11-12）

**定位结论。** `v29kva-maps-0909.log` L16465 的 `0 drawable presents / 45 command buffers`
不能证明 drawable 枯竭：原计数只在插件 CB 的 `presentDrawable:` 完成回调中增加。
20F66 VectorKit 的 `MetalLayer _updateDrawable`（`0x19f21f484`）调用 `nextDrawable`；
`_onTimerFired:withPresent:` 在 `0x19f21f2c0..2d0` 直接执行 present block，
普通 block `0x19f2a8ad8` 直接发 `present`，另两种 block 在 `0x19fb2030c/368`
发 `presentAtTime:` / `presentAfterMinimumDuration:`，均绕过该计数。
旧 v28 Maps 305/417 同样持续显示零。VectorKit `MetalRenderer::frame` 的
`0x19f25dc00..44` 提交三个 CB 后只等 Scheduled；实际展示/回收由 QuartzCore 管理。
CA `get_unused_drawable` 在 `0x18837d0ec..f8` 比较 allocated/maximum 数量并限制分配，
`nextDrawable` 在 `0x18829e1cc` 等信号、`0x18829e4a4` 报 timeout；本插件未实现或重写池。
输入日志没有这些调用、直接展示或 presented handler 的记录，**尚不能确认冻结的具体根因，
也没有证据把它归为丢 completion 或 nextDrawable 阻塞**。L16465 的平均 completion 13 ms、
L16478 的 GPU pass 26.05 ms 及后续持续 completion 排除了整个 Maps GPU queue 停止退休的说法。

**附件与修复。** L16475 `30 attachments / 1474800 KB` 是两秒内逻辑请求总和，
`Texture::make` / `newBuffer` 即使命中 BO cache 仍累加；不是 1.4 GB 存活量。
L16461 `allocations 64 hits 53 new 11 ... memoryless-hits 21; cached 68720 KiB`
已经证明有复用。1125×2436 的 4× BGRA8 与 Z32F/S8 backing 逻辑大小分别为 43240/55080 KiB，
一组 98320 KiB，15 组恰好 1474800 KiB。旧 128 MiB memoryless 子上限容不下两组
页对齐后的 196672 KiB，主机可重现交替释放时的驱逐/重分配；现删除该子上限，共用原有
**256 MiB / 512 BO 总上限**，保留全部真实 fence 复用条件。此处取代 BO reuse v29 的
128 MiB 子上限说明。MSAA framebuffer fetch 会转 sysmem，故仍需要 backing，不能省掉。
改正 fps 文案为 `logical buffer requests / attachment requests ... not live memory`；
缓存改善后该累计量仍可为 1.4 GB，应检查实际 `bo-cache new/maps/evictions`。

**补齐观测。** 新 `HakiosMetalDrawables.h` 仅在现有 `metal_fps` 开启时观察 CA 原方法，
记录直接 present、应用注册的 presented handler 及其交付、nextDrawable 成功/nil/耗时和进行中的调用；
不改 maximumDrawableCount、调用参数、返回值或同步策略，不额外注册 handler，观察 block 不捕获 drawable。
两秒 `ca-drawables:` 中 `next-active` 跨窗口持续非零而 `returned=0` 才是 nextDrawable
阻塞证据；`nil`/慢返回还单独打印 layer、maximumDrawableCount 与 wait ms。
`handlers` 表示应用注册次数；`pending` 持续增长而 `delivered` 不增长可定位其展示回调未交付；
`present-active` 则区分卡在 present 本身。`pending` 是跨窗口 gauge，其他计数是窗口增量。
VectorKit 未注册 handler 时 `handlers=delivered=0` 正常。若 next/present 继续增长而地图仍静止，
保留日志，转查场景/像素更新，不能继续按
“没有 drawable”诊断。主管同时检查大幅缩放能恢复、clear/resolve 正确及无 GPU fault/timeout。

**主机与构建。** 新 core 回归在旧缓存代码下 **35 failed checks**，修后 **0**；
两组全尺寸附件反复使用 30 次仅首次 map 4 BO、随后 120 hits / 0 new。
`tests/maps_drawable_test.py` 运行实际观察器，覆盖三槽直接展示、延迟 presented/池耗尽、
nil、阻塞中调用、跨窗口 pending 及不延长 drawable 生命周期；batch/pencil plugin 回归通过。
经根目录 `build-lock.sh`：普通 **339 / 0**，MESA **340 / 16**，失败检查和失败测试名称
与 pencil 的 339/16 完全相同；要求的 `make -C work/umi-v23/hakios-metal ios MESA=1`
及独立插件构建通过，`nm -u -m` **0 non-weak dynamically looked-up symbols**。
最终 archive/bundle、测试/构建日志、旧代码对照、VectorKit 反汇编和 SHA256 位于
`/tmp/hakios-maps-v29/`。保留 v29-pencil 的 drain 与 texture-completion SPI；
未构建完整 payload，未修改内核、inject_sb.c 或通知代码，未访问手机。

## v29c（2026-11-12）：drawable 的 CA 提交与画布 shadow blit

**本轮边界。** 基于刚测试的 v29b，保留上面 v29-pencil 的 dispatch idle drain、真实
texture completion，以及 v29-maps 的 BO cache 改动。核心 GPU/PM4 代码没有修改。
本轮修复了可由代码和主机回归证明的两个交接缺陷；尚无 v29c 真机结果，不能把下面的
主机断言当作 Notes 症状已消失或 Maps 延迟已下降的实测结论。

**Maps：渲染在继续，缺的是 foreground layer handoff。** 输入为
`logs/v29-snap/v29b-nomodem-full-1133.log`（有 NUL，使用 `rg -an`）。
L122809 的 45 CB 在 2 秒内继续完成，completion 平均 20.9 / 最大 36.5 ms；
L122810 是 `next=15 returned=15 nil=0 next-active=0 next-max=122.5 ms maximumDrawableCount=3; present=14 handlers=0 delivered=0 pending=0 ... present-max=0.1 ms`。
L123662 是 `next=16 returned=16 nil=0 ... next-max=127.3 ms ... present=16 handlers=0 delivered=0 pending=0`；
L124165 是 `nextDrawable layer=0x283d55a00 maximumDrawableCount=3 returned=0x2823d8c60 wait=734.4 ms`。
L123663 的 `0 waits for completion, 0 ms in them` 只统计 CB wait。
`fps: 0.0` 的分子仍只来自 CB present；VectorKit 的直接 present 不进入该分子。
`handlers=delivered=0` 是它没有注册回调，不能解释成回调丢失。
用户补充的“上滑切后台那一刻立即显示正确新地图”把问题进一步限定在 CA 层内容发布，
而非 VectorKit 停止绘制。

20F66 QuartzCore `-[CAMetalDrawable present]` 的 Apple bundle 分支
`0x18832095c..998` 可置 image-queue-only flag；
`layer_private_present_impl` 在 `0x188294fe0` 插入图片，在 `0x188295208/20c` 可跳过
`set_commit_needed`（后者在 `0x188295284`）。插件原先只观察 present，没有补齐这条
没有 AGX display fence 的路径。现在三种直接 present 都先等待该 texture 的 committed GPU
uses，然后用 `setSharedEvent:nil` 选择 CA 原有的同步插入分支，调用原 present，再调用
layer `setContentsChanged`；非 `presentsWithTransaction` layer 紧接 `CATransaction flush`，
transactional layer 保留 caller 的事务边界。time/duration 原参数保持。hook 不再依赖 fps 开关。
必须在原 present 前保留 layer，因为 CA 会释放 drawable 的 private references。
不能只在原方法返回后 flush：`0x188295724..774` 的 IOSurfaceSharedEvent listener
可以延后图片插入；这里的 CPU GPU-completion 边界取代本驱动没有的 surface/display event。
没有改成 `layer.contents = IOSurface`，该做法会破坏 CA 原来的 image queue。

**三槽回收与 120 ms。** `nextDrawable` 先 collect（`0x18829e0e4`），取 unused
（`0x18829e12c`），再等 semaphore（`0x18829e1cc`）。满队列的
`CAImageQueueCollect_` 会同步向 server 取释放状态（`0x1882ef0dc` 的 mach_msg）；
`release_sample`（`0x1884b35f0`）在 sample 引用归零时还回 unused，drawable dealloc
（`0x18835ae80`）唤醒池等待者。未提交的新 queue 内容会让消费者继续持有旧 sample，
使后续获取落到 collect/release 慢路。修复让消费者在 foreground 收到新队列，主机回归
覆盖三槽连续 30 轮复用。现有日志未分拆 collect、IPC、semaphore 的耗时，**不能声称
已测得其中某个调用正好占 120 ms**；应在 v29c 日志确认慢 nextDrawable 是否随提交修复消失。

**Notes：已证实的 shadow blit 缺陷及证据边界。** v29b L34075/35956 的 completion
为 3.2/3.1 ms，L34076 的 `next-max=2.7 ms; present=22 handlers=11 delivered=11 pending=0`，
不支持把它归成 Maps 一样的 drawable 耗尽。L32366 明确使用 4512-byte-row IOSurface
shadow；L34091 是 `surface copies on the processor: 2 taken from their surfaces in 13 ms, 0 written back in 0 ms`，
L35972 是 `2 taken ... 8 ms, 0 written back`。这些行证明使用了 shadow，但不能单独证明
哪张是已完成笔画的背景，也不能从 `0 written back` 推断那两张一定应写回。

PencilKit `renderTexture:intoFramebuffer:sourceRect:destinationRect:renderState:blendMode:`
的同尺寸优化在 `0x1b72c8fc8` 调用 texture blit，将背景送入 original-back framebuffer。
插件 render/compute binding 会刷新 shadow，原 `hm_blit_texture` 却只返回 core texture：
复用 snapshot 的 surface seed 变化后，blit 仍可复制旧的空白 shadow；blit 写入 shadow
也没有 completion writeback。现在源/目标都先按 seed 刷新，只有写目标注册完成写回，
保留 partial copy 外的像素；texture view 的 `hakiosIsShadow` 沿 parent 判断，与既有
refresh/writeback 的 parent 规则一致。普通纹理和只读 blit 不新增 completion handler。
这修复了背景复制通路中的确定错误；**输入日志没有逐次 blit/source seed/像素关联，
尚不能闭环证明该优化就是本次 Notes 每笔暂时隐藏旧笔画的唯一根因**。没有据此添加
drawable 复制、改 Load/Clear、R8/memoryless 或 sparse attachment 的处理。

**等待审计。** v29-pencil 的 idle/surface drain 负责 submit，不直接等待 GPU；
texture SPI 的等待仍只在显式 `waitUntilComplete` 内，按 backing BO 的 committed uses
同步，不等待任意应用 completion callback。普通 render bind、commit 没有新增 GPU wait。
本轮仅对 drawable 的显示交接增加资源完成边界；CB present 本来在 GPU completion 后
调用，所以那里无需再次等未完成的同一 CB。shadow blit 仅 seed 变化的 CPU 导入需要
既有 CPU-access 同步，写回由完成回调执行。
L129764 的 backboardd completion 平均 2.6 / 最大 2.9 ms、caller commit 总计 20 ms，
L129766 的 CB wait 为 0；旧统计没有 texture SPI 耗时，不能据此排除其帧率影响。
新增单独两秒计数，关闭 fps 时不读时钟、不更新统计。

**v29c 精确检查项。** 下列行是格式/检查规则，不是真机测量值；PID 和数字按新进程替换：

```
HakiosMetal[Maps PID]: ca-drawables: next=N returned=N nil=0 next-active=0 next-max=X ms maximumDrawableCount=3; present=P handlers=0 delivered=0 pending=0 present-active=0 present-max=Y ms contents-changed=P flush=P
HakiosMetal[MobileNotes PID]: texture-completion: calls=N wall=X ms max=Y ms; shadow-blit: sources=S destinations=D
HakiosMetal[backboardd PID]: texture-completion: calls=N wall=X ms max=Y ms; shadow-blit: sources=S destinations=D
```

Maps 非事务层的 `contents-changed`/`flush` 应跟 present 同步增长；边界跨报告窗口可差一帧。
transactional layer 的 flush 由 caller 决定，因此可以为 0。继续出现每次约 120 ms 的
`ca-drawables: nextDrawable ... wait=` 或只有切后台才更新，表示 Maps 延迟/发布问题尚未解决，
不能用 hook 调用计数代替屏幕结果。`fps: 0.0 ... command-buffer presentDrawable only`
在 VectorKit 仍可能正常。Notes 连画至少三笔，指下 trail、旧笔画和抬手后的笔画应同时正确；
`shadow-blit: sources` 可确认是否实际走了这次修复的背景通路；若始终为 0 且症状仍在，
该缺陷不能解释本轮 Notes，需保留该证据继续追踪 render-blit/CA snapshot。
对照 backboardd 的 texture-completion wall/max、原 CB wait 和 encode-v29 drain_reason，
不把 API 必需的完成等待恢复成恒真。

**主机回归与产物。** `tests/maps_drawable_test.py` 运行实际 header，覆盖 fps 关闭、
GPU-before-insert、禁用异步 surface event、插入后的 layer dirtiness/flush、caller transaction、
精确 timed 参数、三槽复用、应用 handler 交付、阻塞计数与对象生命期；分别去掉 commit、
event 选择、GPU completion 的三个对照都在对应断言失败。
`tests/shadow_blit_plugin_test.py` 提取实际 blit、refresh、writeback、view 方法，使用延后 GPU
copy 与可变 surface seed，验证三笔背景、局部目标写回、view、完成前不可见及只读不注册回调；
恢复旧 source、destination、view 行为的三个对照均失败。它已加入现有 pencil wrapper。
原 batch/pencil completion 回归通过。以上使用主机模型，不执行 iOS 的 CA server/GPU。

根目录 `build-lock.sh` 下最终普通测试 **339 / 0**；Mesa **340 / 16**，逐条比对
v29-maps 基线的 16 个失败检查和 5 个失败测试名称完全相同。
`/Users/xm666/X/umi-ios/build-lock.sh make -C work/umi-v23/hakios-metal ios MESA=1`
成功（核心未改，archive 已是最新）；独立 CA bundle 编译链接成功，`nm -u -m`
检查 **0 non-weak dynamically looked-up symbols**。日志、对照输出、构建脚本、
`undefined.txt` 和 arm64 bundle `/tmp/hakios-v29c/HakiosMetal` 均在 `/tmp/hakios-v29c/`；
核心 archive 仍为 `hakios-metal/out/ios/libhakios-metal.a`。未构建完整 payload，未访问手机。


## v29d（2026-11-12）：修正事务策略、shadow 发布顺序及 GMEM fetch 重复同步

保留 v29-pencil、v29-maps、v29c 的 idle/surface drain、backing texture completion、
BO cache、同步 image queue insertion 和 shadow blit。输入为用户 12:29–12:35 的
`logs/v29-snap/v29c-1235.log`；下列 L 行号按物理 LF 计算（文件含 NUL，使用 `rg -an`）。
本节修正 v29c 把 `presentsWithTransaction` 当成当前线程显式事务边界的解释。
只做代码、主机回归和构建；以下没有 v29d 真机成功或帧率提升的测量结论。

**Maps：layer policy 错当成线程事务，flush 分支始终未执行。**

```text
L45703: HakiosMetal[Maps 233]: ca-drawables: next=18 returned=18 nil=0 next-active=0 next-max=6.8 ms maximumDrawableCount=3; present=18 handlers=0 delivered=0 pending=0 present-active=1 present-max=53.2 ms contents-changed=17 flush=0
L47009: HakiosMetal[Maps 233]: ca-drawables: next=12 returned=12 nil=0 next-active=0 next-max=205.7 ms maximumDrawableCount=3; present=12 handlers=0 delivered=0 pending=0 present-active=1 present-max=446.4 ms contents-changed=12 flush=0
L56754: HakiosMetal[Maps 233]: fps: 0.0 frames/s over 2.2 s (0 presented via command-buffer presentDrawable only (panel count unavailable), 0 drawable presents, 55 command buffers); commit-to-fence wall 1786 ms (81% of window, not GPU occupancy), -commit on the caller's thread 53 ms, commit to completion 32.5 ms on average, 258.2 at most; a buffer holds 72.2 passes (0.3 full flushes with a wait for idle, 0.0 barriers, 0.0 WAR waits, 0.0 chained), 90.5 draws, 0.0 dispatches, 71.8 copies or clears; 0.0 passes in GMEM (0.0 tiles)
L56755: HakiosMetal[Maps 233]: ca-drawables: next=20 returned=20 nil=0 next-active=0 next-max=97.1 ms maximumDrawableCount=3; present=19 handlers=0 delivered=0 pending=0 present-active=0 present-max=52.1 ms contents-changed=20 flush=0
L56756: HakiosMetal[Maps 233]: texture-completion: calls=20 wall=620.6 ms max=52.0 ms; shadow-blit: sources=0 destinations=0
```

GPU/CB 仍在完成，原生 direct present 与 contentsChanged 也继续发生，flush 却一直为 0。
`0 drawable presents` 只统计 command-buffer API；`handlers=0 delivered=0` 是 VectorKit
未注册 handler，不能解释成 handler 丢失。`next-max=205.7 ms` 与
`texture-completion max=52.0 ms` 是不同窗口/不同边界，不能把全部 nextDrawable 时间归给 GPU。

20F66 VectorKit 在 `0x19f259724` 明确设置 `presentsWithTransaction:YES`。
`+[CATransaction flush]`（`0x188346364`）和 `CA::Transaction::flush`
（`0x18845b5dc`）操作当前线程的 TLS transaction；layer policy 不表示该线程正处于
`begin/commit` 内。原 v29c 按 layer policy 跳过 flush，使没有主 runloop 的绘制线程的
implicit transaction 未提交，新 image queue 留在客户端；切到 app switch 时另一次 CA
提交才显示正确地图，与用户观察一致。

`hm_drawable_commit` 现在每次同步插入后都 `setContentsChanged`、`CATransaction flush`。
显式 begin/commit 的边界由 CA 自己处理，未额外 commit caller 的事务。policy YES/NO、
后台线程无 runloop、显式嵌套事务均有主机对照。保留 GPU-before-insert 和
`setSharedEvent:nil`；不将 CAMetalLayer.contents 替换为裸 IOSurface。
QuartzCore 的 `_display`（`0x1884b3b44`）把既有 image queue 交给 CALayer.setContents，
CAMetalLayer 自己的 setContents（`0x1884b3bb8`）警告任意赋值为 undefined behavior。
原生 `layer_private_present_impl` 的成功 insertion 在 `0x188294fe0`，随后
`0x188295000 setInsertSeed:`；`_didCommitLayer:` 在 `0x188280fb8` 发出
CA::Context::commit_command(14)。诊断保留这条原生路径，不自行模拟 CA server。

**Notes：live renderer 的背景来自 CA snapshot + tile，不是每一笔重走 shadow blit。**

```text
L75125: HakiosMetal[MobileNotes 314]: a texture over an IOSurface of rows the GPU cannot take (4512 bytes) is a copy of them, made again when the surface changes
L76113: HakiosMetal[backboardd 24]: fps: ... surface copies on the processor: 1 taken from their surfaces in 12 ms, 1 written back in 34 ms; YCbCr frames: 0 converted
L77077: HakiosMetal[MobileNotes 314]: fps: 12.4 frames/s over 2.3 s (29 presented via command-buffer presentDrawable only (panel count unavailable), 29 drawable presents, 67 command buffers); commit-to-fence wall 239 ms (10% of window, not GPU occupancy), -commit on the caller's thread 50 ms, commit to completion 3.6 ms on average, 13.5 at most; a buffer holds 0.9 passes (0.9 full flushes with a wait for idle, 0.0 barriers, 0.0 WAR waits, 0.0 chained), 2.0 draws, 0.0 dispatches, 0.0 copies or clears; 0.1 passes in GMEM (0.8 tiles)
L77078: HakiosMetal[MobileNotes 314]: ca-drawables: next=29 returned=29 nil=0 next-active=0 next-max=3.8 ms maximumDrawableCount=3; present=29 handlers=9 delivered=9 pending=0 present-active=0 present-max=1.0 ms contents-changed=29 flush=29
L77079: HakiosMetal[MobileNotes 314]: texture-completion: calls=29 wall=0.5 ms max=0.5 ms; shadow-blit: sources=0 destinations=0
L77094: HakiosMetal[MobileNotes 314]: fps: ... surface copies on the processor: 3 taken from their surfaces in 20 ms, 0 written back in 0 ms; YCbCr frames: 0 converted
L77618: HakiosMetal[backboardd 24]: fps: ... surface copies on the processor: 2 taken from their surfaces in 11 ms, 2 written back in 56 ms; YCbCr frames: 0 converted
```

Notes 的 texture wait 最大 0.5 ms，present 最大 1.0 ms，handler 9/9；这不支持把旧笔画隐藏
归为 Maps 同样的 pool/提交停滞。`shadow-blit sources=destinations=0` 排除了用 v29c blit
修复解释本次症状。4512-byte-row shadow 确实存在；backboardd 的 CPU 回写一次 34 ms、
两次 56 ms，而 Notes 快速完成 texture wait，显示出必须检查跨进程 snapshot 发布的路径。
这些汇总不能单独证明哪张 IOSurface 是本笔背景。

20F66 PencilKit `PKTiledView` 在 `0x1b71a4744` 调用 CARenderServerSnapshot，
`0x1b71a495c` 将 snapshotTexture 与 tiles 交给 live drawing；six-channel renderer 在
`0x1b72d2d3c` 把 snapshot 渲染到 originalBack，随后在 `0x1b72d2e3c/60` 绑定 tile
与 multiply texture。`renderTiles`/`renderTile`（`0x1b72d3114`/`0x1b72d2464`）
默认走 render-blit shader，PKUseBlitEncoder 默认 false。tile 在 `0x1b733bf84`
把 framebuffer IOSurface 设为 CALayer contents。live layer 可以是不透明的；空白/过期
snapshot 会在指下遮住已完成 tiles，抬手移除 live layer 又露出旧笔画。
不应以强制透明、去掉 Clear 或修改 six-channel blend 掩盖这个发布问题。

驱动原来的 shadow writeback 是普通 application completed handler：先将 use 标 DONE、
signal event，再异步到 client completion queue 写 IOSurface。CA `finish_image`
（`0x188362058`）通过 texture.waitUntilComplete 只等 GPU backing fence，因而可在
CPU shadow 尚未发布时返回；该 client queue 自己又可能正在等待 snapshot。这个完成
顺序缺陷由代码与主机暂停完成队列的回归证明。

现在 render/blit/compute 的 driver shadow targets 在 CB 内归一到 root texture、去重并保活；
GPU completion 内先同步发布 shadow，随后标 DONE、signal events、交付应用 callbacks。
同一 CB 多个 view/attachment 只发布一次。shadow 的 isComplete/waitUntilComplete 还包含
此 driver publication 的完成状态；普通纹理仍按 backing BO 的 committed uses 同步。
不等待任意 application callback，不增加普通 render bind/commit 的 GPU wait。
现有日志尚未逐 surface 闭环证明这是 Notes 症状的唯一来源；下一次启动用下面的完整
texture/layer 探针确认 snapshot、tile 与 live layer 的实际内容/顺序。

**Blur：已有数据证明 pass/WFI 放大，尚不能给 Gaussian/downsample/variable blur 排名。**

```text
L90084: encode-v29: cb=120 submit=120 merged=0 sysmem=0 gmem=120 wfi=11760 wfi_per_cb=98.00 chain=0 draws=1740 encode_us_per_draw=22.48 batched_commits=0 cb_per_drain=1.00 drain_reason=surface:120,cpu_sync:0,cross_queue:0,limit:0,idle:0
L96814: encode-v29: cb=107 submit=107 merged=0 sysmem=0 gmem=933 wfi=96374 wfi_per_cb=900.69 chain=392 draws=9866 encode_us_per_draw=27.89 batched_commits=0 cb_per_drain=1.00 drain_reason=surface:107,cpu_sync:0,cross_queue:0,limit:0,idle:0
L96843: HakiosMetal[backboardd 24]: fps: ... passes timed by the GPU: 1384 ms in all; the slowest: draw 1125x2436 draws 18 in 22 tiles 10.45 ms; draw 1125x2436 draws 18 in 22 tiles 10.44 ms; draw 1125x2436 draws 16 in 22 tiles 10.40 ms; draw 1125x2436 draws 16 in 22 tiles 10.38 ms
L103804: HakiosMetal[backboardd 24]: fps: 44.9 frames/s over 2.0 s (90 presented via panel, 0 drawable presents, 93 command buffers); commit-to-fence wall 1480 ms (74% of window, not GPU occupancy), -commit on the caller's thread 205 ms, commit to completion 16.0 ms on average, 24.1 at most; a buffer holds 16.9 passes (10.1 full flushes with a wait for idle, 6.8 barriers, 3.0 WAR waits, 6.8 chained), 140.0 draws, 0.0 dispatches, 0.0 copies or clears; 13.8 passes in GMEM (107.4 tiles)
L103819: HakiosMetal[backboardd 24]: fps: ... passes timed by the GPU: 1251 ms in all; the slowest: draw 1125x2436 draws 85 in 22 tiles 9.26 ms; draw 1125x2436 draws 85 in 22 tiles 9.21 ms; draw 1125x2436 draws 85 in 22 tiles 9.19 ms; draw 1125x2436 draws 85 in 22 tiles 9.18 ms
L103820: HakiosMetal[backboardd 24]: fps: ... a pass left in sysmem: a texture over colour attachment 0's memory
L103821: HakiosMetal[backboardd 24]: fps: ... surface copies on the processor: 2 taken from their surfaces in 12 ms, 1 written back in 25 ms; YCbCr frames: 0 converted
L103822: encode-v29: cb=94 submit=94 merged=0 sysmem=286 gmem=1303 wfi=141775 wfi_per_cb=1508.24 chain=642 draws=13207 encode_us_per_draw=25.14 batched_commits=0 cb_per_drain=1.00 drain_reason=surface:93,cpu_sync:1,cross_queue:0,limit:0,idle:0
```

静态窗口约 98 WFI/CB，重窗口约 1508；44.9 fps 窗口每 CB 16.9 passes、13.8 GMEM passes、
107.4 tiles，最慢全屏 22-tile pass 约 9.2 ms；另一窗口约 10.4 ms。原日志只有最慢 pass
尺寸/draws，没有和 GPU interval 关联的 shader 名、MSAA/load/store 信息，不能从 shader
编译行臆断哪种 blur 主导。sysmem reason 是采样纹理与 attachment 0 物理内存重叠；这是
正确性约束，不强制绕过。保留已有 intermediate/BO 复用，不改变应用请求的 load/store、
resolve 或 blur shader 输出。

本轮删除已证实重复的同步：无 visibility streams 时，GMEM 每 tile 的首个 framebuffer
fetch draw body 已有 WFI/cache invalidate/WFI；tile prefix 再 WFI+invalidate 完全重复。
只在 body barrier 存在且无分箱可跳过该 draw 时省去 prefix 的一组操作，每 tile 少 1 WFI
和 1 CACHE_INVALIDATE。有分箱、未启用 body barrier 或无 fetch 的路径保持原编码；
首 fetch、写后 fetch、store/resolve 等同步保留。主机逐包比较除这组操作外的所有 PM4
不变，并覆盖先画普通 background 再 fetch。未更改视觉算法，实际设备收益待测。

**一次启动的诊断：无 lifetime/total-count 日志预算。**

现有 `metal_fps` 开关同时启用本轮 handoff 探针；限定 Maps、MobileNotes、backboardd。
生命周期转换逐次记；texture role 在每 CB 内去重；CA queue/context 与 pass 成本每 2 秒
汇总/采样，应用停止出帧时 CA 定时采样仍继续。没有“前 N 次后不再打印”的新探针。
既有 `fps`、`ca-drawables`、`texture-completion`、`encode-v29` 继续输出。

- `ca-life`：step、layer、drawable、drawableID、texture、IOSurface ID、status、insert-seed、
  tid/main、原 time/duration 参数、单调时间。覆盖 next-request/return（arg 为实际等待秒数）、
  present-call、texture-ready、原生 image-queue-insert-seed、present-return、contents-dirty、
  flush-return、_display begin/end、_didCommitLayer、server didPresentAtTime/composite、
  client handler 注册/交付、releasePrivateReferences、drawable dealloc（slot semaphore 释放边界）。
  不为 Maps 人工注册 handler；`didPresentAtTime arg=0/status=3` 可是 disposal，不能视作显示成功。
- `ca-queue`：layer/root/queue、unconsumed、server-update-host、displayed-pixels。在交接状态
  变化和每 2 秒读原生 CAImageQueue 的共享 server 状态。`ca-context` 同时记录 context
  pointer/ID、root layer、commit ID；`ca-layer` 的 server-layer 是 `_didCommitLayer:` 的
  CA::Layer 参数，不是 CATransaction。用 layer/root 将这三组关联；flush 返回后需看到 server
  update 和 backboardd 的后续读，不能用一次 flush 计数替代消费证据。
- `ca-pool`：next-return 时同一 IOSurface 上次 producer pid/layer/drawableID，证明 slot
  被重新获取；与 dealloc/next wait 分开读。CA 的私有 C release_sample 不改写 callback；
  server 共享队列更新、drawable slot 释放与同 surface 再获取共同包围 sample/pool 回收。
- `surface-life`：texture/root/label、surface ID/seed、尺寸/format、shadow、GPU complete、
  CB useSeq、producer pid/layer/drawableID、thread/time。覆盖 import/seed refresh、CPU
  replace/getBytes begin/return、GPU encode/submit read/write、GPU fence、shadow publication
  begin/end、write-visible、read fence、texture wait begin/return、signal event。
  IOSurface 的 `HakiosMetalProducer` 元数据跨进程关联 Maps/Notes layer 与 backboardd
  的真正采样读取；同一 CB 内的 encode 角色去重，不把 GPU fence 当 CPU 发布完成。
- `surface-pass`：每个 color attachment 的 texture/load/store/resolve/level/slice 与 CB seq；
  render sampled texture、blit source/destination 和 compute 的 kernel 声明 read/write 都记录。
  attachment 记录表示绑定角色（含 fetch-only），不宣称 shader 一定写过该附件。
- `notes-layer`：CALayer contents 改变前后/contents-dirty、hidden/opacity/opaque 改变、ancestor
  状态；含 class、layer、parent、contents/IOSurface ID、是否 CAMetalLayer、bounds。
  结合 surface-life 区分“tile 真被 clear/重绘”与“不透明 live layer 挡住正确 tile”。
- `pass-v29d`：pid、2 秒窗口、kind、尺寸、GMEM/sysmem/reason、tiles、samples、实际
  load/store/clear masks、去重 program names、次数、GPU 总/最大 ms、draws、展开 IB replay
  后的 WFI、resolve ops、profile_wfi。一个 pass 的多种 shader 共享同一 GPU interval，
  不伪称逐 shader 的独占耗时；按签名总耗时找到真正重的 blur passes。

原 pass profiler 有 64 passes/CB 固定上限；Maps L56754 已有 72.2 passes/CB。
现在 timestamp BO 按每 64 passes 的 block 增长，所有 pass 都有独立区间，避免上限后反复
覆盖最后一项。chained sysmem 的 light tail 原来没有完成等待，CP_REG_TO_MEM 会记成 CP
enqueue 时间；只在 profile 开启时给该结束时间戳加 WFI（参照 Mesa tu_CmdWriteTimestamp2），
单列 `profile_wfi`，避免与原有 WFI 混算。profiling 关闭时不增加这些等待。
该诊断会增加日志和 profiling 的开销；旧/新 GPU 数字应结合 profile_wfi 看，不能将其当
无探针性能测量，也不能把 backboardd 输入读完成当成某一物理 panel scanout 的证明。

**主机回归、构建和证据产物。**

`maps_drawable_test.py` 运行实际 header；新增 policy YES 的无 runloop 后台线程及真实显式
事务深度模型。commit/event/completion/policy 四个旧行为对照均在对应断言失败。
`batch_plugin_test.py` 暂停 client completion queue 后验证 shadow 已发布、event 已 signal，
应用 callback 仍未交付；root/view 多次注册只写回一次。恢复 deferred publication 的对照
在 event 的 surface-visible 断言失败。`pencil_plugin_test.py` 在 GPU fence 已到而 driver
publication 暂停时验证 shadow/view incomplete 且 wait 阻塞，发布后唤醒。
`shadow_blit_plugin_test.py` 的 source/destination/view 三个旧行为对照继续通过。
`devices/xiaomi-mi-10/tools/test-metal-pencil.py` 整体通过。

新增核心回归验证安全条件下每 tile 精确减少一组同步，其他 PM4 一致，以及 130 passes
跨 3 个 timestamp block 全有有效时间。FakeGpu 不执行 fragment rasterization，像素/真机
显示正确性不能由它证明。

```text
make -C work/umi-v23/hakios-metal test
341 tests, 0 failed checks
PATH=/opt/homebrew/opt/llvm@16/bin:$PATH ./build-lock.sh make -C work/umi-v23/hakios-metal test MESA=1
342 tests, 16 failed checks
/Users/xm666/X/umi-ios/build-lock.sh make -C work/umi-v23/hakios-metal ios MESA=1
exit 0
./build-lock.sh /tmp/hakios-v29d/compile-plugin.sh
nonweak dynamic lookup symbols: 0
```

相比 339/0、340/16 基线增加 2 个核心测试；Mesa 16 个失败检查和 5 个失败测试名称逐条
与 `/tmp/hakios-v29c/test-mesa.log` 完全一致（`mesa-baseline-comparison.txt`）。
最终 archive 为 `hakios-metal/out/ios/libhakios-metal.a`，独立 arm64 CA bundle 为
`/tmp/hakios-v29d/HakiosMetal`。全部正/负回归、构建日志、精确 `nm -u -m`
输出 `undefined.txt`、独立链接脚本保存在 `/tmp/hakios-v29d/`。
没有构建完整 payload，没有执行 adb/fastboot/ssh，没有访问手机或修改 kernel/inject_sb/通知。

## v29e（2026-11-12）：探针默认关闭、逐帧日志改计数、私有 swizzle 全部撤除

依据 `work/v29e/INVESTIGATION-metal.md` 对 v29d 崩溃与卡顿的调查。保留 v29-pencil、
v29-maps、v29c、v29d 的全部功能改动（每次 present 都 `setContentsChanged` +
`CATransaction flush`、shadow 发布顺序、GMEM per-tile WFI 移除）。只做代码、主机回归
和独立编译检查；没有构建 payload，没有 adb/fastboot/ssh，没有接触手机。

**开关：`/var/db/hakios-metal-cache/handoff-diag`（文件存在才开）。** 不新增 boot-arg
（607 字符已满）。`hm_handoff_on()` 每进程读一次，写法与 `bo-cache-off`、
`batch-commit-off`、`gmem-args` 相同。没有该文件时：

- 不安装任何 v29d 探针 swizzle（CALayer setContents:/setContentsChanged/setHidden:/
  setOpaque:/setOpacity:、CATransaction +begin/+commit/+flush 都不 hook，周期定时器也不建）；
- 没有 `surface-life`、`ca-life`、`ca-queue`、`ca-transaction`、`notes-layer`、`ca-pool`、
  `surface-pass` 输出；
- 每次 present 不再 `IOSurfaceSetValue`（D7）；
- 既有 `fps:`、`encode-v29`、pass 汇总原样保留。

**逐帧/逐 pass/逐纹理日志改为计数器**，只在 ~2 s 的 `fps:` 窗口里多打一行：

```text
handoff: cb=21 encode=63/14 submit=63/14 fence=63/14 visible=63 shadow=2/2 pass-attach=168 imports=2/0/1 cpu=8/1 waits=20 notes=5/40/2 tx=3/3/0 signals=1 pool=0 drawable=20/20/20/20/20/20/20/0/0/0
```

字段依次为：完成的 command buffer 数；encode 读/写纹理；提交时读/写；GPU 完成时写/读
fence；写可见；shadow 入队/发布；pass 的 color attachment 数；纹理导入总/平面/shadow；
CPU 写/读；`waitUntilComplete` 次数；Notes 的 contents 变化/contentsChanged/外观变化；
事务 begin/commit/flush；信号事件；drawable 池复用；最后 10 个 drawable 步骤计数
（next-request、next-return、present-call、texture-ready、present-return-inserted、
contents-dirty、transaction-flush-return、handler-register、handler-deliver、其它）。
状态变化仍各留一行：`ca-life` 只在 drawable 的 status 改变时打；`ca-queue` 每 2 s
每 layer 至多一行且仅在 queue 状态变化时打；`ca-context` 每 2 s 每 context 一行；
`handoff: -[Class sel] encoding` 每个 hook 安装时一行。

**D2：swizzle 只接受该类自己实现的方法。** 新 `hm_hook(Class, name, imp)` 用
`class_copyMethodList` 找方法，打一次真实 type encoding，再
`method_setImplementation`；不在本类方法表里就打印一行并跳过（不再用会沿继承链查的
`class_getInstanceMethod`，也不再把 NULL 交给 `method_setImplementation`）。wrapper 在
原 IMP 为 NULL 时直接返回。

**私有方法签名无法用 20F66 材料证实，因此全部撤除。** `tools/tmp/dsc-20F66` 的
QuartzCore 是 dyld shared cache 的预优化 method list：type encoding 能按 entry 相对
偏移解出，但 name offset 指向 cache 全局 selector 池（不在该提取镜像的
`__objc_methname` 内），无法把选择子与 encoding 对上。故 `_display`、
`_didCommitLayer:`、`setInsertSeed:`、`didPresentAtTime:`、`releasePrivateReferences:`、
`setDidComposite:` 六个 hook 全部删除（H1 的主要嫌疑 `hm_ca_insert`/`hm_ca_release`
也在内）；`hm_ca_dealloc` 一并删除（D6）。保留的 hook 只有 Apple 公开签名的方法
（CALayer 的 setContents:/setHidden:/setOpaque:/setOpacity:，CATransaction 的
+begin/+commit/+flush），`setContentsChanged` 仅当 CALayer 自己实现时才装（否则打印
一行跳过），其 encoding 在安装时打印。

**D3：** `-waitUntilComplete` 里新加的 `pthread_cond_wait` 无超时循环换回
`hm_use_wait(root->_uses)`（先 drain，2 s `pthread_cond_timedwait`，超时只说一次）。

**D4/D5：** `hakiosTrackTexture:` 忽略非 `HakiosMetalTexture` 对象；
`hm_shadow_targets` 先做 `isKindOfClass` 再跟踪（resolveTexture 同样检查）。
`hm_drawable_trace` 不再把记录里的裸地址当 layer；`status`/`insertSeed` 只在
`next-return`/`present-call`/`texture-ready` 这三个"layer 方法传入的活对象"步骤上读取。

**第 3 节 GMEM 说明（未改代码）：** `render.cpp` 的 `encodeGmem` 与 body 屏障用的是
同一个 `Options` 实例——`work.fetchBarrierInBody` 取自 `open_->options.fbReadBarrier`
（render.cpp:3864），`encodeGmem` 收到的 `o` 也是 `open_->options`（render.cpp:3879），
当前代码两者相同，不存在调查里担心的不一致；`render.cpp` 不在本次允许修改的文件范围内，
未改。

**主机回归与构建。**

```text
make -C work/umi-v23/hakios-metal ios MESA=1
exit 0（无变更，Nothing to be done for `ios'）
make -C work/umi-v23/hakios-metal test
341 tests, 0 failed checks
make -C work/umi-v23/hakios-metal test MESA=1
342 tests, 16 failed checks（既有基线，未增加）
python3 devices/xiaomi-mi-10/tools/test-metal-pencil.py
batch/pencil/shadow-blit 三项通过
python3 devices/xiaomi-mi-10/tools/test-metal-iosurface-planes.py
PASS
```

`hakios-metal/tests/{batch,pencil,shadow_blit}_plugin_test.py` 与
`devices/xiaomi-mi-10/tools/test-metal-iosurface-planes.py` 的 host stub 补了
`hm_diag`/`hm_diag_add`/`hm_use_wait`/`hm_handoff_on` 声明（提取片段现在引用它们）。
独立 CA bundle 编译检查（`xcrun clang -fsyntax-only -DHAKIOS_METAL_GPU`）exit 0，7 条
warning 与 v29d 相同。

## v29g per-draw cost（2026-11-12）：编码侧每 draw 的 malloc 与 WC 写

依据 `work/v29e/INVESTIGATION-fps.md` 第 3 节（每 draw 27–45 µs）与第 7 节 P1。只改
`hakios-metal/` 的源码与头文件；未改 GMEM 逻辑（`metal_gmem_draws=0`/sysmem 仍是设备上
的默认路线）、未改 `userland/src/HakiosMetalCA.m`、未碰内核；未新增 boot-arg、未加逐
draw/逐帧日志。PM4 字节不变：所有改动只动 CPU 侧的临时对象和表内存里"从不被
`CP_LOAD_STATE6` 读"的槽。

**改了什么（file:function）**

1. `include/hakios-metal/command.hpp`（`CommandBuffer`）：新增 4 个成员 scratch
   `drawHead_/drawBody_/drawHeadSys_/drawBodySys_`；`render.cpp:CommandBuffer::draw`
   每次 draw 开头 `clear()` 并只在容量不足时 `reserve`。此前每 draw 新建 4 个
   `pm4::Stream`（空 vector），`body` 一路扩容到 350–600 words，约 20 次
   realloc+拷贝。`pm4.hpp:Stream` 加了 `reserve`/`capacity`。
2. `render.cpp:encodeInPass` 描述符表：只 `memcpy` CP 实际会读的槽
   （`numTex*64`、`numSamp*16`），不再把每个 stage 的整张 1024+256 B 表写进
   write-combined 内存（每 draw 2560 B，其中通常只有 1–4 个描述符被读）。GPU 侧字节
   完全一致：`ST6_CONSTANTS`/`ST6_SHADER` 的 NUM_UNIT 就是 numTex/numSamp。
3. `render.cpp:CommandBuffer::draw` 的 pipeline key：`std::vector<uint8_t>` +
   `std::string`（reserve 320、每 draw 两次 malloc、320 B 拷贝 + 320 B 哈希）换成
   `command.hpp` 的定长 POD `StaticKey`（512 B，`size` + `memcmp`，FNV-1a 哈希），
   `pipelineStates_` 的 key 类型随之改变；`open_->staticKey` 同型。
4. `render.cpp:loadDirect`：3+dwords 个 word 先走 64-word 栈缓冲（driver params、
   UBO 描述符、immediates 都在内），超长才用 vector。PM4 字节不变。
5. `render.cpp:encodeInPass` 的 `changed()` 影子：vertex fetch 与 viewport 的字节从
   两个 `std::vector<uint8_t>`（每 draw 两次 malloc）改为栈数组（+static_assert 尺寸）。
6. `render.cpp:vertexFetch`/`stageTextures`：`const std::string name = stageName(...)`
   （每 draw 2–3 次长名字 malloc）改成 `const char*`，只在拒绝分支里
   `std::string(name) + ...`；拒绝文案逐字不变（对应单测全过）。
7. `render.cpp:encodeDraw` 的 `Stage fsDrawn = program.fs;`（1.5 KB 拷贝）只在真的
   丢掉某个 `[[color(n)]]` 槽时才拷贝；普通 draw 直接用 `program.fs`。

**主机微基准（临时程序，未入库）**：1125×2436、`end_only` 手写 VS/FS、每帧 120 draw
（首个开 pass，其余 join 并走 dedup），40 帧，只计 `CommandBuffer::draw` 本身；
用全局 `operator new` 计数。在同一台机器、同一 `-O2` 下对比改动前后：

| 模式 | 改动前 | 改动后 |
|---|---|---|
| sysmem（设备当前） | 1.099 µs/draw，13.65 allocs/draw | 0.972 µs/draw（−11.6 %），0.58 allocs/draw（−96 %） |
| GMEM | 1.126 µs/draw，13.73 allocs/draw | 0.977 µs/draw，0.67 allocs/draw |

稳态 join draw 的分配数：**13 → 0**（改后每帧只剩首个 draw 与开 pass 的固定分配）。
主机不会模拟 write-combined 内存，所以第 2 项（每 draw 少写约 2.3 KB WC）的收益不在
上表里；按 A77 上 27–45 µs/draw 的既有实测，保守估计 **−8 % 至 −15 %**（39 → 约
33–36 µs/draw，110 draw/帧省约 0.5–1.0 ms，220 draw/帧省 1–2 ms）。真正的确认看设备
上原有的 `encode-v29: … encode_us_per_draw=` 两秒行（`counts_.encodeNs` 只包
`CommandBuffer::draw`），不需要新日志。

**未做（留给下一次）**：`appendDeduped` 对 body 的逐包 `pm4::decode` 再重发（调查里
的第 2 项，也是剩下最大的一块）需要把 encode 的包边界一起记下来，改动面大、风险高；
`command.cpp:resolveUse` 的无竞争快路径（每 BO 每 CB 一次，不是每 draw）也没动。

**主机回归/构建**

```text
make -C hakios-metal test                      341 tests, 0 failed checks
make -C hakios-metal test MESA=1               342 tests, 16 failed checks（既有 5 个 Mesa 夹具用例，与基线相同）
build-lock.sh make -C hakios-metal ios MESA=1  exit 0（libhakios-metal.a + hakios-metal-probe）
python3 -I devices/xiaomi-mi-10/tools/test-metal-pencil.py            exit 0（batch/pencil/shadow-blit 全过）
python3 -I devices/xiaomi-mi-10/tools/test-metal-iosurface-planes.py  PASS
```

`render_test` 的逐字 PM4 断言、`render_refuses_*` 的拒绝文案断言都过：编码输出与拒绝
文案均与改动前一致。

## v29h black shadows（2026-11-12）：同一 pass 内的 RAW 屏障、无 alpha 目标的混合因子

依据 `work/v29g/INVESTIGATION-black-shadow.md`（Maps 里柔和阴影被画成实心黑圈）。调查已证伪 A8
解释（v29g 的 CA 阴影流水线全部是 BGRA8Unorm、没有 A8 附件、没有 exact 回退），落到三处代码：
**同一 open pass 内"后一笔采样前一笔写的附件内存"没有任何屏障（H7，代码级硬洞）**、无 alpha 格式上
`blendWords` 照抄 `DST_ALPHA`/`ONE_MINUS_DST_ALPHA`（H2'）、以及 framebuffer fetch 的
`levelDescriptor` 与 `sampledDescriptor` 能力不一致（H6 的潜在雷）。只改 `hakios-metal/`；未碰内核、
未改 `userland/src/HakiosMetalCA.m`、未加 boot-arg、未加逐 draw/逐帧日志。GMEM 默认仍是关，本次改动
不改变任何已有工作的编码字节（拒绝文案与 PM4 只在新的拒绝/新的屏障处出现）。

**改了什么（file:function）**

1. `render.cpp:CommandBuffer::draw`（H7）：在跨 pass 的 `dirty_` 判断之后，再判一次 **本 pass 内**的
   RAW——若该 draw 加入的 open pass 已经画过某个颜色附件（`OpenPass::written`），而它采样的某个
   `kTexSampled` 槽的纹理与该附件的 BO 相同，就置 `raw`，走已有的 `encodeBarrier`
   （`PC_CCU_FLUSH_COLOR_TS` + 等值 + `CACHE_INVALIDATE` + WFI），屏障在该 draw 之前写入。
   - 只看 sysmem 且 `!passWfi_`；GMEM pass 里 `gmemDrawOk` 本就拒绝这种 draw（tile 会整体重放，
     纹理读到的会是 pass 之前的内存），所以这条只可能命中 sysmem。
   - `OpenPass::written` 现在对 sysmem pass 也维护（原来只在 GMEM 分支里记，`closePass` 的
     tile load/store 仍只读它）；新增 `OpenPass::barriered`（command.hpp），屏障之后把
     `written` 记进去，之后没有新写入的采样 draw 不再重复付 WFI。
   - 代价：只有 pass 已经写过附件、且当前 draw 真的绑了同 BO 的采样纹理时才进入；每个这样的 draw
     只多几次 BO 比较，命中才多一次屏障（正确性优先，与 v29g 之前"一 pass 一全刷"同量级）。
2. `render.cpp:blendWords`（H2'）：目标格式没有 alpha 通道（`FormatInfo::hasAlpha == false`，R8、
   RG8 等）时，把 `MTLBlendFactor` 的 DestinationAlpha(8) → `FACTOR_ONE`、OneMinusDestinationAlpha(9)
   → `FACTOR_ZERO`——"没有 alpha 的目标读出来是 1.0"与纹理侧 `texSwizzle` 的 SWIZ_W=ONE 同一条规则；
   RB 的因子寄存器里没有格式信息（turnip/freedreno 也只是把 Vulkan 的因子原样写入，树内没有这层翻译），
   所以翻译只能在驱动里做。有四通道的格式原样写入。
3. `render.cpp:stageTextures` 的 `fbTarget` 分支（H6 加固，不改当前可工作的形态）：
   - 单采样 2D fetch 打到多采样附件上 → 拒绝（`levelDescriptor` 是一采样视图，会把交织的样本读成更窄
     的行，属于"静默读错内存"）；多采样 fetch 单采样附件原本就有拒绝。
   - 压缩（UBWC）附件 → 拒绝（今天不可达：压缩纹理不能当颜色附件；只是防将来）。
   - 数组 / cube / 3D 附件的 slice 0 仍走 `levelDescriptor`（它本就是该 level 的 2D 行），已在
     `texture.cpp:Texture::levelDescriptor` 与 `texture.hpp` 的注释里写明它与 `sampledDescriptor` 的
     关系，以及两种它表达不了的形态由 fetch 拒绝。
4. 探针（`command.cpp:CommandBuffer::reportPasses`，只在原有的两秒 `pass-v29d` 行里加字段，无逐 draw
   日志）：`fmt=[BGRA8Unorm,R8Unorm,…]`（该 pass 颜色附件的格式名，`render::attachedFormats`，按槽位
   顺序）、`fetch=N`（本 pass 内声明 `[[color(n)]]` 的 draw 数，`PassTime::fetchDraws`）、
   `rawBarrier=N`（本 pass 内命中的同 pass RAW 屏障数，`PassTime::inPassBarriers`）。`PassTime` 新增
   `attachedFmt/fetchDraws/inPassBarriers`，sysmem pass 的 `touched` 也补上（原来只有 GMEM pass 有）。

**主机测试（新增/加强）**

- `a_clear_packs_the_colour_as_each_format_holds_it`：`cases[]` 加两条 A8（`{0.25,0.5,0.75,0.5}` →
  `{128}`、全 1 → `{255}`），补上 A8 Clear 从未被覆盖的空缺（实现原本就对）。
- `a_draw_into_an_a8_target_names_alpha_in_the_rb`（新）：A8 目标上 `RB_MRT_BUF_INFO[0]` /
  `SP_FS_MRT_REG[0]` 是 `FMT6_A8_UNORM`、swap WZYX、无 SINT/UINT；`RB_MRT_CONTROL` 的组件使能跟
  `MTLColorWriteMask`（0xf 与 0xe 两例）；Clear 的 `RB_2D_SRC_SOLID_C0..3` 只有 C3=128 非零、目标
  信息是 A8；命令缓冲里 fake 逐项校验后完成。注释写明"A8 里画 0.5 读回 0.5"仍只能靠真机。
- `a_draw_that_samples_what_the_open_pass_wrote_gets_a_barrier`（新，修前为红）：一个 pass 两笔 draw，
  第二笔绑一个 `Texture::overBuffer` 在附件 0 同一 BO 上的 R8 视图；断言 `counts().barriers == 1`、
  pass 汇总是 `inPassBarriers == 1`、fake 无 fault；换到别的 buffer 的纹理时 0 屏障；framebuffer fetch
  的两笔只记 `fetchDraws == 2`、`inPassBarriers == 0`（fetch 的屏障不算同 pass RAW）。
  已验证：去掉 H7 判断后本测试变红（fake 报 "sampled at … while what a draw … wrote … is still in the CCU"）。
- `blending_into_a_target_without_alpha_translates_the_destination_alpha_factors`（新，修前为红）：
  R8 目标 + `ColorBlend{... dstRgb=DestinationAlpha, dstAlpha=OneMinusDestinationAlpha}` → 编码后
  `RGB_DEST_FACTOR == FACTOR_ONE`、`ALPHA_DEST_FACTOR == FACTOR_ZERO`，RGB 源因子（SourceAlpha）不变；
  BGRA8 目标的同一条 pipeline 仍是 `FACTOR_DST_ALPHA`/`FACTOR_ONE_MINUS_DST_ALPHA`。
- `a_second_attachment_of_one_channel_encodes_and_stores_as_its_own_target`（新）：BGRA8 + R8 两个附件，
  `MRT 2`、`mrts[1]` 的格式/swap/pitch/base、`fs.colorRegid[1]`，以及 pass 汇总
  `attached == 0x3`、`stores & 0x3`、`attachedFmt == "BGRA8Unorm,R8Unorm"`。

**主机回归/构建**

```text
make -C hakios-metal test                      345 tests, 0 failed checks（基线 341 + 4 个新测试；A8 Clear 是表内新增两行）
make -C hakios-metal test MESA=1               346 tests, 16 failed checks（与基线 342/16 相同，未增长）
build-lock.sh make -C hakios-metal ios MESA=1  exit 0（libhakios-metal.a + hakios-metal-probe）
python3 -I devices/xiaomi-mi-10/tools/test-metal-pencil.py            exit 0（batch/pencil/shadow-blit 全过）
python3 -I devices/xiaomi-mi-10/tools/test-metal-iosurface-planes.py  PASS
```

**风险 / 未证**

- 黑圈到底是不是 H7 造成，仍要真机定：本次改动只保证"同 pass 内 RAW 有正确的屏障"和"无 alpha 目标
  的 DST_ALPHA 语义正确"。真机上用新的 `pass-v29d` 行看阴影 pass 的 `fmt=`（是不是 BGRA8/R8 两附件）、
  `fetch=`、`rawBarrier=`；若 `rawBarrier` 在 Maps 阴影时非零，说明命中的正是 H7。
- 命中时会多一次 WFI（与 v29g 之前每 pass 结束时的那次同量级）；`barriered` 保证同一 pass 里连续采样
  不重复付。
- H6 的两条新拒绝：多采样附件的 2D fetch、压缩附件的 fetch。按调查今天都不可达（MSAA fetch 走 MS
  形态、压缩纹理不能当附件），但一旦出现就是"拒绝 + 名字"，不是静默读错。`barriered` 的"不重复屏障"
  路径没有测试能覆盖：fake 的模型把任何带 MRT base 的 draw 都记为写过该 buffer，与写掩码无关。
