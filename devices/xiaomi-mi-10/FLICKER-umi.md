# UMI flicker / 90 Hz candidate — 2026-11-12

Host investigation and experimental kernel; no phone operation performed.
90 Hz is **default off and untested on hardware**. Optical PWM frequency and
the perceived flicker's cause still need the measurements below.

## Evidence and likely causes

All log line numbers below refer to
`logs/test-1111-172537-umi-v28.kernel.log` at the repository root. This file
contains NUL bytes: use `rg -an`, not the default binary-file search.

1. **Normal AMOLED pulse modulation is the leading explanation for continuous
   peripheral flicker at a steady picture/brightness.** The vendor normal/DC-off
   sequence explicitly selects **4 Pulse**; DC-on selects **1 Pulse**. Four
   pulses per 60 Hz frame would be about 240 Hz, but that optical frequency has
   not been measured here. XNU only sends normal `53 20` and big-endian
   `51 high low`, never the calibrated DC sequence. This fits the symptom;
   it is not proof, and refresh/brightness/optical modulation are different rates.
2. **UEFI runs approximately 60 Hz, with divisor 1, not 30 Hz.** Log 285:
   TE enable=1, config=`0x180088`, height=`0xffff`, initial/start=`0x924`,
   autorefresh=`0x80000001`. Log 316: measured **59.835 Hz**, 7083 ns per
   synthetic TE tick, 2359 ticks/frame, DMA idle after tick 1218. INTF_1 is
   `0xAE6B800`; autorefresh at +`0x2b4` has enable bit31 and frame count 1.
   Mainline `dpu_hw_intf.c:402`/`:425` confirms this encoding. The boot
   measurement covers four wraps, not the whole run. Runtime divisor/DMA
   observations are added below; there is no evidence justifying a divisor fix.
3. **The large brightness changes are idle dim/off/on, not an ALS oscillation.**
   This full file has **90** `umi-bl: settled` records, not 249: 20 at 1235,
   18 at 62, 18 at 1133, 12 at 57, 16 at zero, and six other slider endpoints.
   `round(1133*0.05)=57`, `round(642*0.05)=32`. For example:
   log 370694 native idle warn; 370704 factor=0.05 with 500 ms fade;
   370785 settled=57; 371519 factor=1 with zero fade; 371522 settled=1133.
   Those timestamps are **17:52:28.759 to 17:52:34.012**, about 5.25 seconds
   apart, not a high frequency loop. Likewise 401247/401312 dim 642 to 32;
   401570/401590 restore it about 1.21 seconds later. Logs 391085/391192
   are actual slider endpoints 646/642. Source `umi_idle_timer.h:1` makes
   `al=1` the **auto-lock hook**, not auto-brightness. There is no implemented
   ambient sensor feedback loop in this display code. Repeated minimum=0
   requests remove a floor and do not retarget an unchanged level. Real fades
   update DCS brightness at approximately 16 ms steps, so transient flicker
   during a fade remains possible. Do not smooth away intentional lock/off.
4. **Scanout content flicker is still possible, with weaker evidence.**
   454 complete `fbstat` windows report 23,974 flips issued, 23,971 latched,
   zero not-in-time and zero dropped. Pending latches can cross windows/end
   of logging; these totals are not proof of three lost frames. Log 34173
   has 108/108 at 52.6/s, 41302 has 114/114 at 56.9/s. Maximum observed
   swap rate is 60.1/s. Log 249760 explicitly disables zero copy after 16
   bound surfaces had to be returned for copied swaps; the run continues as
   fb_dpu=3. This is a copy transition, not an autorefresh-rate transition.
   DMA completion and alias checks do not prove pixel correctness. Compare
   fb_dpu=3 from boot with fb_dpu=4, and compare a static flat field with
   animation before changing buffer ownership code.

## Panel provenance, DC and 90 Hz

The matched reference is the **Samsung J2 MP 42 02 0b**, not a guessed
EA8076/AMB655X command set. Mainline DTS selects `xiaomi,umi-samsung-panel`:
`mnt/mainline-src/linux/arch/arm64/boot/dts/qcom/sm8250-xiaomi-umi.dts:246`.
Its panel driver is `drivers/gpu/drm/panel/panel-xiaomi-umi-samsung.c` under
that Linux tree. This is a local port, not an upstream driver with proven
runtime switching: `:618` publishes one fixed mode; `:704` chooses 60/90 at
probe. Timing-switch arrays at `:195`/`:354` are retained but not called.
The mainline project's `work/panel/README.md` explicitly says hardware output
was not validated. Its input came from the phone's Android panel selection.
The new XNU path does not read DCS panel ID; it targets this known MP panel.

Vendor inputs are read-only at:

```
/Users/xm666/X/mainline/umi-linux/work/panel/input/
  dsi-panel-j2-mp-42-02-0b-dsc-cmd.dtsi
  vendor-dsi_panel_mi.c
  vendor-dsi_display.c
  manifest.json
```

Provenance: LineageOS/android_kernel_xiaomi_sm8250 commit
`a04dba03999b36cc8f472c014063395b41fb1022`; MP DTS SHA256
`8acd59b8a3e9b5de527be34fb8570fba621b18c07f24a5ffedc0ce2f6143b92c`.

DC dimming is **not implemented in this candidate**. Vendor DTS `:509`/`:511`
uses `53 28`/`53 20` for panel transition dimming on/off; these are not DC
on/off. DTS `:526`/`:538` specifies DC on/off as page0, B2 remapping/pulse
selection, 37-byte B3 curve, page4, and a 76-byte D2 command. DC-off says
`6F 09; B2 20 /* 4 Pulse */`; DC-on says `6F 09; B2 00 /* 1 Pulse */`.
`vendor-dsi_panel_mi.c:2367` reads the panel's 75-byte D2 LUT twice, checks
it, derives enter-DC groups from it (`:2433`), then patches the outgoing
on/off tables (`:2470` onward). The DTS has `dc-update-flag` at `:361`.
Copying its example D2 values or issuing only B2 would skip calibration and
curve changes. XNU has no implemented DSI read/BTA path for that calibration.
The vendor VI threshold 246 is also absent from the current brightness
implementation. No guessed gamma, HBM, supply, calibration or OTP writes were
added. This remains the direct anti-flicker work after optical confirmation.

Both refresh modes use four DSI lanes, 1080x2340, the same DSC PPS and the
same **682,000,000 bit/s per lane**, with a nominal **9500 us** transfer
window (vendor DTS `:62`/`:64`, `:203`/`:205`). The mode descriptions differ:

| Mode | HFP/HSW/HBP | VFP/VSW/VBP | Logical pixel clock |
| --- | --- | --- | --- |
| 60 | 16/8/8 | 600/32/560 | 235655 kHz |
| 90 | 16/8/8 | 4/4/8 | 235788 kHz |

These are command-mode mode descriptions, not video timing registers to
blindly program into the disabled INTF timing engine. The vendor switch at
DTS `:294` is LP page0 (`F0 55 AA 52 08 00`), then one batch containing:

```
B4 98 88 88 77 66 55 44 00 00 00 60 B8 B8 B8 B8 B8 C8 C8 D8 D8 E8 E8 F8 F8 48 48 48 48 0A
2F 04
26 02
```

The inverse at `:153` uses the 60 Hz B4 table, `2F 01; 26 01`.
The UEFI MP XML itself matches the 60 Hz table and 682 MHz lane clock:
`ios-port-research/repos/JopangJovan_hackios-xnu-kernel-umi/work/display-uefi/variants2/build-tree/Binaries/umi/RawFiles/Panel_J2_MP_42_02_0b_amoled_dsc_cmd.xml:33`,
`:41`, `:45`–`:50`, `:107`–`:109`. Thus this experiment needs no PLL,
DSI PHY, loader, DTBO, boot-argument or persistent panel setting change.

## Implementation and switches

All switches are regular files under **`/var/db/hakios-display/`**, read once
before the first framebuffer user client can draw/register vsync callbacks.
Prepare them before that next session starts; changing/removing a file during
an already-open session takes effect next boot. Absence means off. `bl=1
fb_scan=1` remain required; 90 Hz additionally requires active fb_dpu and the
verified UEFI handoff. Existing loader whitelist accepts bl=1, al=1 and
fb_dpu=1/2/3/4; loader.c was inspected and not edited. **Omit fb_hz**: it is a
software rate override and cannot switch the panel. An incompatible explicit
fb_hz causes 90 Hz to be refused.

| File | Effect |
| --- | --- |
| `90hz` | Experimental MP-panel LP timing switch; verify 87–93 Hz before publishing the measured rounded rate. Default off. |
| `measure` | Read-only TE/DMA sampling and changed brightness-request logs; worker samples 32 TE intervals every ~5.5 s. |
| `no-idle-dim` | Diagnostic: positive factor requests become 1.0. Factor **zero still turns off**, and slider settings are preserved. |
| `no-fade` | Diagnostic: brightness changes use zero fade duration. Does not remove steady-state PWM. |

`umi_display_session.h` contains switches, packet batches, the 90 Hz
transaction, one explicit 60 Hz undo on failure, and read-only measurements.
`umi_backlight.c` shares its existing guarded DMA sender with timing batches;
preserves IRQ/W1C handling and busy-DMA quarantine. Page selection and B4/2F/26
cannot interleave with brightness commands (same mutex). Each batch is at most
44 DMA bytes and retains the vendor end-of-batch bits. FIFO writes are padded
to an even number of DWORDs, while DMA length excludes that final zero. Default brightness
mapping and fade cadence are unchanged. Settled logs now include base, factor,
physical minimum and fade duration.

`hakios.display.c` calibrates into a local candidate before committing it.
Its TE phase ticks and DMA image rows are separate units. The 90 Hz transaction
uses the vendor command-mode rule `vsync_count = 19200000 / (2356 * 90) = 90`:
TE config becomes **`0x18005a`**, tick duration about **4687.5 ns**, and there
are about **2370 phase ticks/frame** for 2340 image rows. DMA completion and
idle margin are measured separately. UEFI's exact 60 Hz config **`0x180088`**
is saved for undo. TE height/init/start/thresholds, DSC, 682 MHz lane clock,
and autorefresh divisor 1 remain the handoff values. The video timing engine
is disabled in this command-mode handoff; video-mode prefetch/porch registers
are not used to pace the panel. Drain the current transfer before reconfiguring,
then enable autorefresh before reconnecting external TE.

`MiniIOMobileFramebuffer.cpp` serializes preparation before the first client.
It republishes `TimingElements`, `IOMFBDisplayRefresh` and the integer Hz
`refresh-rate` property from the resulting scan measurement, including a 60 Hz
undo. Refused preconditions leave the existing mode alone. An optional mode
failure never marks `bl_failed`, clears `scan.ok`, or rejects framebuffer opens.

### 11-12 timeout and undo fix

The two device runs failed in command transport, before the rate measurement:
`result=60` means **ETIMEDOUT**, not a measured 60 Hz. The log shows page select
completed, but the 11-DWORD B4/2F/26 batch ended with `status=0x7`,
`fifo=0x11111310`: DMA remained busy and FIFO underflow bit10 was newly set.
The old sender omitted the extra zero DWORD required for odd FIFO lengths by
vendor `dsi_ctrl_hw_cmn_kickoff_fifo_command()` (lines 717–727). Both 3-DWORD
page and 11-DWORD timing transfers now include that FIFO-only padding; the
on-wire commands, their batching, and DMA lengths remain 12 and 44 bytes.
The source is saved at `out/xiaomi-mi-10/display-90hz/refs/`, from the same
LineageOS commit as the panel DTS.

`undo=35` means **EAGAIN**. The old undo entered the ordinary brightness
copy-window check while the preceding DMA was stuck; that check could never
admit another command. Even a completed physical mode change could make the
old software scan window stale. Each timing transaction now disconnects
external TE, disables autorefresh, and drains DSI CMD/MDP before submitting.
With autorefresh paused, the sender checks hardware idle directly instead of
waiting on a software copy window. Every exit restores TE/autorefresh; the 60 Hz
inverse restores the exact UEFI counter config and recalibrates. No DSI host,
PHY, panel reset, supply or PLL changes are used.

Expected log formats (`<...>` denotes a measured value, not literal text):

```text
display-session: 90hz=1 no-idle-dim=0 no-fade=0 measure=1 (files sampled before first client)
umi-bl: DCS #1 cmd=0xf0 value=0 result=0 irq=<irq> status=<status> fifo=<fifo> ack=0
umi-bl: DCS #2 cmd=0x2f value=90 result=0 irq=<irq> status=<status> fifo=<fifo> ack=0
display: UMI command TE: <Hz> Hz, <ns> ns/line, <ticks> phase lines; DMA idle after <tick>; staged presentation enabled
display-session: 90 Hz measured=<mHz> mHz period_ns=<ns> phase_ticks=<ticks> DMA-idle=<tick> TE-config=0x18005a auto=0x80000001; lane clock/DSC unchanged
MiniIOMobileFramebuffer: IOMFBDisplayRefresh: 90 Hz, <ticks> ticks
display-measure: TE intervals=32 mean_us=<us> min_us=<us> max_us=<us> poll_max_us=<us> DMA-restarts=<count> INTF-frames=<count> auto=0x80000001 TE-en=0x1 config=0x18005a init=0x924 height=0xffff DSI-status=<status>
```

Success requires 87,000–93,000 mHz, period near **11,111,111 ns**, runtime
`mean_us` near **11,111**, and about 32 DMA restarts per 32 intervals.
`refresh-rate` must read 90 and mode SyncRate/PreciseSyncRate must be
`90 << 16` (5898240). If the switch is rejected by the panel or measurement,
the inverse must finish with:

```text
display-session: 90 Hz failed result=<errno>; explicit 60 Hz undo=0
MiniIOMobileFramebuffer: IOMFBDisplayRefresh: 60 Hz, <ticks> ticks
```

The runtime measurement then has `mean_us` near **16,667** (the observed UEFI
rate was nearer 16,745), `config=0x180088`, and `auto=0x80000001`.
A nonzero undo is still an error and must not be interpreted as confirmed 60 Hz;
it is logged without removing the display service or disabling brightness.

The panel commands write volatile normal-mode settings. The 90 Hz mode stays
until firmware/Android reinitializes the panel; this candidate does not add a
late shutdown DMA transaction with other CPUs stopped. No files or settings
on the phone were changed by this work.

## iOS pacing and realistic expectations

`MiniIOMobileFramebuffer.cpp:486` publishes min/max/step intervals in 32.32
seconds plus Mach ticks; `:513` publishes mode SyncRate/PreciseSyncRate in
16.16 Hz. Its scan-derived timing and vsync callbacks use `refreshHz()`.
At a measured panel 90, these report 90 and about 11.11 ms; `refresh-rate`
is updated in the same publication. Merely
setting fb_hz=90 would pace software without changing the panel and is not
this implementation.

The spoofed userland is iPhone X/iPhone10,3, which normally has a 60 Hz app
limit. The existing `userland/mkhighrate.py` adds
`CADisableMinimumFrameDurationOnPhone=YES`; the UMI build invokes it at
`overlays/userland/build.sh:792`. Verify the installed system image actually
contains that key in SpringBoard and test apps: a new kernel cannot update
an older system image's Info.plists. Do not spoof a different device model
or guess MobileGestalt capability keys just to force 90.

`/redwood/bin/fps_info` already exists in v28 (log 2653) and reads CADisplay's
maximumFramesPerSecond/minimumFrameDuration/current/preferred/available modes,
UIScreen limits and several MobileGestalt answers. Run it after startup.
CADisplayLink's explicit preferred FPS/range, CoreAnimation animation ranges,
Low Power Mode and application opt-in can still limit content. Existing
SpringBoard hooks at `overlays/userland/src/inject_sb.c:1920`/`:1967` observe
or adjust certain links/animation ranges; no changes to those hooks were made.
The v28 command line does not include anim_max=1. Do not add args for this test.

There is a startup cache question to verify: this driver publishes 60 during
early kernel service registration and republishes 90 on first client open,
after /var is mounted. A consumer that cached registry modes **before** that
open might still report 60. Physical 90 is independently verified by TE;
`fps_info`, registry readback and a display-link timestamp trace distinguish
this from successful software pacing. If needed, restart the consuming service
after the confirmed switch using the supervisor's existing tooling; this
candidate does not assume an undocumented IOMFB mode-change notification ABI.
No new userland payload is required for the physical-rate experiment.

90 fps requires CPU+GPU work to fit **11.11 ms/frame**. GPU utilization of
9–44% does not establish that the CPU, submission serialization or first-time
shader compilation can meet this. v28's measured content spans roughly
10–60 fps and peaks near 60; there is no sustained 90 fps evidence. Test warm
animations separately from first use. A 90 Hz panel with slower content may
reduce panel-synchronous emission flicker if its pulse cadence follows refresh,
but cannot guarantee a PWM cure. Exactly 60 fps on fixed 90 Hz also has uneven
1/2-refresh frame holds (11.1/22.2 ms), so compare motion judder as well.

## Supervisor-only measurement sequence

No phone commands were run by this agent. Use the existing phone-test workflow
and kernel-flicker; keep all existing boot arguments within the 607-byte limit.

1. **Baseline 60:** only `measure` present. Keep a static medium-gray/white
   screen at fixed slider brightness; compare low/mid/high levels. Film with
   locked exposure at 120/240 fps (faster if available) and short shutter, then
   vary shutter/frame rate to distinguish camera aliases. Bands alone do not
   uniquely measure PWM frequency. A photodiode/scope is the direct frequency
   measurement. Compare Android 60/90 with MIUI anti-flicker on/off at matched
   luminance if the supervisor chooses, without changing persistent calibration.
2. **Separate idle policy:** repeat with `no-idle-dim` only added. Confirm
   settled logs keep factor=1000 while awake, but lock still reaches level=0.
   Repeat separately with `no-fade` to isolate transient ramps. Do not call
   these controls DC dimming or an ambient-loop fix.
3. **Separate scanout:** use already-allowed fb_dpu=3, then =4, same pattern
   and brightness. Compare static picture and scrolling. Keep fbstat/alias
   and exact symptom timestamps. Persistent flicker with no flips and stable
   brightness weighs toward panel modulation; movement-only artifacts weigh
   toward rendering/ownership/content.
4. **90 experiment:** add `90hz`, retain `measure`, start a fresh session.
   Expect `display-session: 90hz=1`, followed by `90 Hz measured=...`,
   roughly 90,000 mHz/11,111,111 ns, ~2370 phase ticks and DMA idle before
   the next TE. Then `IOMFBDisplayRefresh: 90 Hz`. An explicit failed/refused
   line is not success; a successful 60 Hz undo means it stayed at 60.
5. Runtime `display-measure:` should show 32 intervals with mean near
   **16,713 us at the v28 rate** or **11,111 us at 90**. With auto=`0x80000001`,
   approximately 32 DMA restarts are expected (±1 at sampling boundaries).
   Half as many DMA restarts suggests a divisor/transfer issue even if TE
   remains 60/90. INTF-frames is an additional raw counter, not assumed to
   advance identically on command-mode hardware. Reject frequency conclusions
   from INCOMPLETE samples or poll_max comparable to a frame; sampling jitter
   widens min/max. Capture all config words, settled brightness and fbstat.
6. Run fps_info and a warm CADisplayLink/animation timestamp trace. Check
   available/current/maximum rate and ~11.1 ms callbacks versus content and
   latched-frame rates. Film the same flat field again; do not infer an optical
   anti-flicker improvement from a software 90 Hz property alone. Remove the
   files and start a fresh session to disable the experiments.

## Validation and artifacts

Meaningful host tests compile extracted production code with fake registers:
`tools/test-backlight.py` (packet transport, timeout/quarantine, W1C/restore,
fades and off-preserving diagnostic policy), `tools/test-display.py` (copy
windows at 60/90, DMA exclusion and TE-wrap rejection), and
`tools/test-display-session.py` (exact vendor batches, synthetic 60/90 TE,
mode admission, stopped-autorefresh transport, vendor TE counter, successful undo,
framebuffer retention after a failed recalibration, and failed calibration
leaving the previous scan intact). An independent direct parse of the original
vendor DTS matched both 56-byte combined transfers byte for byte. Host tests
cannot establish electrical panel behavior or iOS startup cache behavior.

Kernel builds use only build-lock.sh + devices/xiaomi-mi-10/build-kernel.sh.
The normal dSYM recipe hit sandbox `/dev/fd/... Operation not permitted`;
the successful build uses **BUILD_DSYM=0**, which omits debug-bundle generation.
The unstripped kernel is retained separately. A concurrent cpufreq missing
declaration briefly blocked the first build and was resolved in its own source
update; this agent did not edit cpufreq. This is a shared-source kernel,
including other agents' changes, not a display-only binary patch.

Build/test logs, source before/after snapshots, source hashes and the unstripped
kernel are under `out/xiaomi-mi-10/flicker/`. Delivery/hash are recorded there
in SHA256SUMS and appended below after the final build.

Delivered kernel: `out/xiaomi-mi-10/flash/kernel-flicker`

SHA256: `aa1b89ba738cacb7eb8192eb5ecd87d4bf0a2f5ff3b5967edc678e8cfeef8f59`

Final build/package: `out/xiaomi-mi-10/flicker/build-package.log` (exit 0).
All three host tests and prepare.py verify passed. The packaged kernel contains
prepareDisplaySession, hakios_display_session_prepare, the timing tables and
measurement strings; generated implementation/header sources matched the
archived overlays at packaging time.

Files changed by this task (relative to this device directory):

- `overlays/xnu/files/bsd/dev/hakios.display.c`
- `overlays/xnu/files/bsd/dev/umi_backlight.c`
- `overlays/xnu/files/bsd/dev/umi_display_session.h` (new)
- `overlays/xnu/files/bsd/sys/hakios.display.h`
- `overlays/xnu/files/iokit/Kernel/MiniIOMobileFramebuffer.cpp`
- `tools/test-backlight.py`, `tools/test-display.py`, `tools/test-display-session.py` (new)
- `DISPLAY-BRIGHTNESS-umi.md`, `FLICKER-umi.md` (new)

## 2026-11-12 v29 90 Hz delivery

Built with the requested `build-lock.sh` / `BUILD_DSYM=0` / `-j8` command;
final copy performed inside the same build lock. Persistent five-object `-Os`
settings remain in `xnu-device.patch` and the prepared `bsd/conf/Makefile.arm64`.
`__TEXT_EXEC` remains `0x94c000`. The four display host tests passed:
`test-backlight.py`, `test-display-session.py`, `test-display.py`, and
`test-display-compose.py`. Final binary diagnostics and prepared display
source hashes were checked against the overlays. No phone operations were run;
physical 90 Hz and CADisplay behavior await the next device run.

Artifact: `out/xiaomi-mi-10/flash/kernel-v29-90hz`

SHA256: `5296411ff761a2acd6c26ecbe283ed425fa798da47291ee8e8a5381b4eb0fdf7`

Build logs, regression logs, display-only source snapshots/diff, segment table,
vendor display references and unstripped image:
`out/xiaomi-mi-10/display-90hz/`.
