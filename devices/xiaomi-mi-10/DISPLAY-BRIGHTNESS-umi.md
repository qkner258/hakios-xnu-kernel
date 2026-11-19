# UMI brightness and automatic-lock candidate — 2026-11-10

2026-11-11 flicker / experimental 90 Hz investigation and switch-file controls:
see [FLICKER-umi.md](FLICKER-umi.md). `al=1` is auto-lock, not an ambient sensor
loop. The 90 Hz experiment is default off and requires hardware validation.

Host-only implementation/build. No phone commands, partition writes, dtbo changes,
or SYSSLEEP changes. Hardware outcome is **not yet tested**. The automatic-lock
root cause is **not established by the available logs**; al=1 supplies diagnostics
and a native-policy-preserving timeout fallback, not a claim that the underlying
AttentionAwareness failure has been proven.

## Findings with evidence

The final morning log is `/Users/xm666/X/umi-ios/logs/test-1110-morning1.merged.log`.

* Line 10208: `hakios: UMI brightness hooks installed (initial 0.60)`.
* Lines 13543–13549: `269 permille -> DCS 0x51 level 562 (ok)` and both
  `BKDisplayBrightnessController` and `BrightnessSystemInternal` slider hooks.
  Thus the existing transport did complete at least one DMA; it was not absent.
  DMA completion alone does not prove panel luminance changed.
* Lines 24903–24906: lock requests factor=0 with 0.185 s fade, through
  `DisplayBrightnessFadePeriod` / `DisplayBrightnessFactorWithFade`.
  Line 24930: `Set Blanked: YES`. Lines 25812–25816: unblank, factor=1 / 0.185 s.
* Lines 17961 onward repeatedly set minimum physical brightness to zero with
  0.4 s fade. This **removes a floor**, and is not itself an off/dim target.

The existing overlay `inject.c` only bridged `DisplayBrightness` and
`setBrightnessLevel:reason:options:` to `kern.hakios.brightness`; it ignored
factor, fade period and physical minimum. The kernel synchronously sent one
level per sysctl with no fade, mapped slider zero to panel level 16, and called
screen-off policy based on slider zero. The old sender ignored a failed TE idle
wait and mistakenly used INT_CTRL bit 24 (ERROR status) as DMA interrupt mask;
the correct DMA mask is bit 1. It also rewrote unrelated W1C status bits.

`MiniIOMobileFramebuffer.cpp` has no brightness implementation or AppleARMBacklight
service. RequestPowerChange selector 12 is accepted without action; unknown
selectors are accepted with zero outputs. There is no implemented
IOMFBBrightnessLevel/IODisplayParameters brightness sink here. The existing
visible off path includes black frame presentation and logical blanking;
there was no DCS 0x28/0x10 panel power cycle. The patch uses the already-observed
backboardd bridge, and does not guess private IOMFB selector/float encodings.
MiniIOMobileFramebuffer was inspected and **not edited**.

The panel reference is mainline
`mnt/mainline-src/linux/drivers/gpu/drm/panel/panel-xiaomi-umi-samsung.c`, selected
by `xiaomi,umi-samsung-panel` in `sm8250-xiaomi-umi.dts`. It uses the big-endian
`mipi_dsi_dcs_set_display_brightness_large()`, max 2047, initial 536, normal-mode
control `53 20`. Vendor source provenance is recorded in
`/Users/xm666/X/mainline/umi-linux/work/panel/input/manifest.json` (LineageOS
SM8250 commit a04dba03999b36cc8f472c014063395b41fb1022). Register semantics were
cross-checked against mainline `msm/registers/display/dsi.xml` and `dsi_host.c`;
DSI6G has the leading HW_VERSION word (+4 relative to XML register offsets).

## bl=1 implementation

Default boot retains the prior path. `fb_scan=1 bl=1` enables the replacement
transport and fade worker only after command-mode TE calibration. It reuses the
existing 0x51 packet encoder and DSI command DMA FIFO. No display DMA buffer or
SMMU mapping is allocated.

* `53 20` once, then long writes `51 high low`, levels 0..2047. Both transactions
  independently require a safe TE idle window and CMD DMA/MDP idle.
* TRIG_CTRL BLOCK_DMA_WITHIN_FRAME provides hardware exclusion if autorefresh
  starts between the software check and DMA trigger. The MDP trigger and TE bits
  are preserved; SSPP, CTL, pixel DMA and render/present code are untouched.
* Save/restore command DMA control, trigger, length, TPG state and IRQ masks.
  Acknowledge only DMA_DONE, preserving unrelated W1C statuses. Observe new
  FIFO/ACK errors. 40 ms bounded waits; repeated missed TE windows or a transport
  error quarantine the new path. Do not reset a still-busy DMA or the whole DSI.
* A kernel worker performs elapsed-time linear fades, coalesces duplicate
  requests and retargets from the last successfully sent level. Requests do not
  wait for the whole fade. Slider transitions use 120 ms; factor/minimum WithFade
  requests use the latest CoreBrightness fade period (e.g. 185 or 400 ms).
* Slider 0..1 maps to visible levels 16..2047. Factor 0..1 multiplies the result;
  factor zero writes **panel level zero**. Physical minimum is applied before
  the factor and never replaces the stored slider setting. Physical values use
  a **nominal 500 nit normal range**, not measured calibration; no HBM support.
* The existing `kern.hakios.brightness` permille ABI remains usable. With bl=1
  it queues a slider update and reads back the slider setting. The new atomic
  `brightness_command` ABI is four uint32 words: version=1, kind (1 slider
  permille / 2 factor permille / 3 minimum milli-nits), value, fade_ms (0..10000).
  `kern.hakios.brightness_mode` tells the new userland hook whether to use it.
* Publish screen-off PM policy after zero-brightness completion; release it on
  an on request. Brightness changes while off preserve the desired slider value.
  Existing iOS blanking remains in charge of black frames. TE/DSI/rails stay up;
  no 0x28/0x10, no system sleep, and wake is simply the factor rising again.

The first 96 submitted DCS writes are logged, plus every transport failure.
`result=0` means command DMA completed without newly observed FIFO/ACK errors;
these unacknowledged writes cannot prove panel receipt or optical brightness.

## Automatic-lock investigation and al=1 fallback

Confirmed exclusions from source/current logs:

* Shipped `userland/var/mobile/Library/Preferences/com.apple.springboard.plist`
  contains watchdog scaling and battery percentage only, not a Never setting.
  It does not establish what is in the phone's **persistent** /var.
* `autounlock=0` only suppresses a one-shot unlock at startup; there is no existing
  idle-timer-disable hook in inject.c/inject_sb.c.
* UMI touch_report emits DOWN/changed MOVE/UP, filters unchanged contacts, and
  the input thread blocks on an empty event queue. Display refreshes and empty
  touch polls do not generate HID activity. Final logs show contact release.
  No continuous fake activity is established by the evidence.
* The final logs show manual lock/unlock and no diagnostic description of the
  active idle deadline, managed-config value, idle-disable assertion or native
  timer callback. They cannot distinguish Never, an assertion, a developer
  preference, or a broken attention timeout path. No claim is made that an
  IOHIDSystem HIDIdleTime value was read from the phone.

20F66 shared-cache metadata and disassembly establish the actual timer path:

```
SBIdleTimerGlobalCoordinator -> SBBacklightIdleTimer
  -> ITAttentionAwareIdleTimer -> ITIdleTimerDescriptor.timeouts
  -> idleTimer:attentionLostTimeoutDidExpire:
     id=1: warning/dim, id=2: quick-unwarn, id=3: expiry/lock
```

The init uses the main callout queue. Method addresses/signatures were checked
against the local iOS shared cache; al=1 checks all four method IMP addresses
before installing any hook, failing closed on an ABI mismatch.

al=1 observes SpringBoard's **effective native descriptor**, so Settings,
ManagedConfiguration, native disable assertions, activation and manual-reset
policy remain authoritative. It does not write preferences or force a 30/60 s
lock. A main-queue timer supplements a missing native timeout only after its
own deadline plus 0.5 s grace, using the original callback and original
ITIdleTimeout object. Native callbacks are observed/deduplicated so a delayed
native event is not delivered a second time. Native attention resets reset the
fallback epoch. Weak timer references avoid retaining dead timers.

Real accepted touch/key events publish a Darwin notification (MOVE coalesced
at 100 ms, DOWN/UP always sent). SpringBoard resets its native timer on those
notifications. Held contacts/buttons inhibit the fallback until release.
No events are invented by frame rendering, periodic polling or the watchdog.

Interpretation on the next supervisor run:

* `umi-idle policy` and `configure`: actual effective descriptor and reason.
* `preference SBDisableAutoDim/SBDontDimOrLockOnAC/SBDisableAttentionAwareness`:
  explicit persistent overrides (unset is reported as unavailable, not false).
* `native timeout`: original timer worked; no fallback was needed for that stage.
* `missing native timeout ... id=1/3`: the native deadline existed, no accepted
  activity/reset intervened, and native delivery was late/missing; fallback
  invokes the system's normal dim/lock callback.
* Every 30 s `status`: activated/disabled/manual/held/fired, or no configured
  timer observed. Disabled/Never is **not overridden**; diagnose the policy first.

Until these lines and actual display behavior are returned, automatic-lock's
specific cause and on-device fix remain unverified. Environmental auto-brightness
also still requires the absent ALS/SLPI sensor path; this implements supplied
brightness and idle dimming, not a fabricated light sensor.

## Changed files

All device code stays in the UMI overlays:

* `overlays/xnu/files/bsd/dev/hakios.display.c`: small include, bl gate/start and
  compatibility-sysctl routing additions (preserves other agents' scanout edits).
* `overlays/xnu/files/bsd/dev/umi_backlight.c`: guarded transport, worker, mapping,
  sysctl transaction and diagnostics.
* `overlays/userland/src/inject.c`, `umi_brightness_bridge.h`: brightness property
  bridge, plus input activity calls.
* `overlays/userland/src/umi_idle_activity.h`: activity publisher.
* `overlays/userland/src/inject_sb.c`, `umi_idle_timer.h`: two include/install
  additions and isolated native timer diagnostics/fallback implementation.
* `loader/loader.c`, root `configs/boot-args.lst`: exact `bl=1` and `al=1` flags.
* `tools/test-backlight.py`, `tools/test-idle-timer.m`, `tools/test-loader-metal.py`:
  host transport, timer and argument validation tests.
* This document.

## Builds, tests and handoff

Passed:

* `python3 devices/xiaomi-mi-10/prepare.py verify`
* `python3 devices/xiaomi-mi-10/tools/test-backlight.py`: actual packet code,
  fade endpoints/monotonicity, no-write admission failures, W1C preservation,
  restore, bounded timeout, busy-DMA quarantine and error result.
* `python3 devices/xiaomi-mi-10/tools/test-display.py`: existing scanout regression.
* `python3 devices/xiaomi-mi-10/tools/test-loader-metal.py`: actual loader parser,
  including new valid flags and malformed flag rejection.
* Host Objective-C test of the production idle-timer logic, with fake native
  descriptors: deadline, inactive/disabled/manual/Never/held suppression,
  release reset, missing callback delivery, late native dedup, Settings change.
* Kernel via the required build-lock + build-kernel command after env.sh.
* Loader via flash/build.sh loader (this wrapper already takes build-lock).
* Complete CA payload via `FLASH_CA=1 METAL_PLUGIN=ca
  EXTRA_SHARE=.../out/xiaomi-mi-10/ca-share flash/build.sh userland`.
  No new userland compiler warnings; both injection dylibs have no non-weak
  dynamically looked-up symbols. Kernel/loader retain their pre-existing warnings.

Pinned artifacts: `out/xiaomi-mi-10/brightness-v1/`. Use all three candidate
kernel/loader/payload files together to test both features. The ordinary
flash outputs were built too; the pinned copies avoid subsequent agent builds
changing this candidate. Existing flashboot/UEFI stay compatible.

```
0381afe5fd200717720dd3df1bf0996b182202e84ac767a41ef55ddb2ae10409  kernel
70c164889d79673640842d8e708bf0f419d32efb209be3d624585d5d0ae96e6a  loader.efi
9ab1f7d6b9a94e482219651f849939718539d1a90374756dced88d10eed089d5  root-flash-ui-ca.apfs
```

Build logs, source hashes, dylibs and SHA256SUMS are alongside those files.
Payload snapshot: `out/xiaomi-mi-10/flash/build-20261110-080738`.
Kernel (17,867,088 bytes) + payload (20,951,040) + loader header fits 63 MiB.
The recorded morning boot line is 529 characters; adding ` bl=1 al=1` is 539/607.
Recount for any additional modem/USB/performance flags used by the supervisor.

Exact lines to check (supervisor performs all phone work):

```
umi-bl: enabled bl=1;
hakios: umi-bl bridge=1
hakios: umi-bl DisplayBrightnessFactorWithFade kind=2 value=0 fade_ms=185 result=0
umi-bl: DCS #... cmd=0x53 value=32 result=0
umi-bl: DCS #... cmd=0x51 value=... result=0
umi-bl: settled level=0 display=off
umi-bl: settled level=... display=on
hakios: umi-idle al=1:
hakios: umi-idle real HID activity publisher enabled
hakios: umi-idle policy reason=...
hakios: umi-idle configure timer=... reset=... active=... disabled=...
hakios: umi-idle native timeout timer=... id=...
hakios: umi-idle missing native timeout: ... id=1 ...
hakios: umi-idle missing native timeout: ... id=3 ...
[Display] Set Blanked: YES
```

Failure diagnostics: `umi-bl: disabled result=...`, nonzero bridge `result`,
`umi-idle ABI mismatch`, or `no configured SBBacklightIdleTimer observed`.
First compare defaults, then brightness-only `bl=1`, then `bl=1 al=1` while
checking slider min/mid/max, manual off/on, dim interrupted by touch, idle until
configured timeout, and native Never/active-playback assertions. These are
supervisor acceptance checks, **not tests performed by this agent**.

## v7 idle diagnostics checkpoint — 2026-11-10, after v6

Supervisor reports `bl=1` lock fade works on v6. Auto-lock at 60 or 30 seconds
still does not dim or lock, with USB charging connected. This checkpoint fixes
our **setting observation**, not a proven underlying auto-lock failure. It adds
no new timeout fallback and does not override AC, attention or assertion policy.

### Correct iOS 16.5 setting, proven from 20F66 binaries

* `DBSSettingsController screenLock:` at `0x2005c1f24` calls
  `[[MCProfileConnection sharedConnection] effectiveValueForSetting:MCFeatureAutoLockTime]`.
* `setScreenLock:specifier:` at `0x2005c1fa8` calls `setValue:forSetting:` with
  the same key. Positive values are seconds; Settings maps Never (-1 in its UI)
  to `2147483647` in MC.
* `MCFeatureAutoLockTime` at `0x1d768c440` is `maxInactivity`.
* SpringBoard's `SBIdleTimerGlobalStateMonitor` initialization block at
  `0x1c4ffd4a8` calls **the same `effectiveValueForSetting:` API** (stub
  `0x1c5473d40`, selector reference `0x1d9fd50c8`).
* `MCUserUserSettingsFilePath` resolves to
  `/var/mobile/Library/UserConfigurationProfiles/UserSettings.plist`;
  `MCEffectiveUserSettingsFilePath` resolves to
  `/var/mobile/Library/UserConfigurationProfiles/EffectiveUserSettings.plist`.
  Verified their once blocks and `MCUserProfileStorageDirectory` block
  `0x19ef47968`: `/var/mobile/` + `Library/UserConfigurationProfiles`.
  The implementation uses MC APIs, not direct plist reads; restrictions/defaults
  are already combined in the effective value.

The old `CFPreferencesCopyAppValue(SBAutoLockTime, com.apple.springboard)` was
not reading this setting. `<description unavailable>` also conflated an unset
preference with a formatting failure. The old statement “MC effective policy
logged separately” was incorrect: no MC query existed. The three optional
SB preference overrides being unset does not establish their effective policy.

### Changes and exact log checkpoints

Only production source changed this round: `overlays/userland/src/umi_idle_timer.h`.
Host coverage extended in `tools/test-idle-timer.m`; this document records the findings.
The supervisor's exact-word `al=1` fix is retained.

With `al=1`, startup registers these Darwin notifications (result 0 is success):

```
hakios: umi-idle observe com.apple.managedconfiguration.settingschanged result=0
hakios: umi-idle observe com.apple.managedconfiguration.effectivesettingschanged result=0
hakios: umi-idle al=1 v7: native policy + 0.5s timeout grace; MC maxInactivity observation installed
hakios: umi-idle MC reason=startup connection=... maxInactivity user=... effective=... seconds (2147483647=Never)
```

Each notification logs `umi-idle settings notification=...`, then re-reads MC
250 ms later on the main queue, allowing native notification processing to settle.
No polling fallback, preference writes or forced timer reset is added. Example
when a 30-second choice is accepted without restrictions: `user=30 effective=30`.
An unset user override is explicitly `<unset>`; effective may still be 60.

`umi-idle effective` logs the **native monitor's** `autoLockTimeout`, `onAC`,
`dontDimOrLockOnAC`, `disableAutoDim`, `dontLockEver`,
`disableAttentionAwareness`, `batterySaver`, `thermalBlocked` and
`coordinatorDisabled`. It runs after first observing the coordinator, on MC
notifications, and with the existing 30-second status log. `onAC=1` alone is
not evidence of a no-lock policy; inspect `dontDimOrLockOnAC` and the actual
deadlines. No charging policy is changed without this evidence.

`umi-idle status` now includes elapsed `idle`, backend pointer, `enabled`,
timeout count, config/attention reset counts, last attention event and input
notification count. `umi-idle deadline id=... seconds=...` lists native backend
deadlines: 1=warn, 2=quick-unwarn, 3=expire. These expose the previously silent
backend-disabled/empty-list gates and recurring resets. Existing timeout
callback delivery and all suppression conditions are preserved.

### v6 evidence and limits

`logs/test-1110-091804-umi-v6.kernel.log:3673` has only the startup diagnostics.
However, the same run's `091830-run.log.live.*` snapshots contain later policy,
configure and status lines. Deduplicated evidence is pinned as
`out/xiaomi-mi-10/idle-v7/v6-live-idle-evidence.txt`. In particular,
`091830-run.log.live.38:11156` records an Auto descriptor with warn=40, total=60;
`.51:10970` has active=1, disabled=0, manual=0, held=0, fired=0.
Thus a finite descriptor **was provided**; missing lines in the final kernel
export do not prove the hook stopped. The old logs cannot distinguish a disabled
attention backend, empty timeout list or repeated resets, and do not prove AC
policy is the cause. Preserve live snapshots for the next run as well.

### Payload and host validation

Use `out/xiaomi-mi-10/idle-v7/root-flash-ui-ca.apfs`, retaining the supervisor's
v6 kernel/loader and boot arguments including `bl=1 al=1`. No new boot args,
kernel, loader, DSI, touch, battery, render or SYSSLEEP edits in this checkpoint.

Payload SHA256:
`a9e0f61f008db495291adf693d74becf1777ab83570893e9d51c48a9e0da3b40`

SpringBoard dylib SHA256:
`d58f266e0bcd692b6cb80b9ea08c421aa4aaf6a3e243074fece9f992ab833fef`

Full userland compilation succeeded (`flash/build-20261110-094436`), but its
merged Metal bundle had four non-weak unresolved `hakios_metal_perf_*` symbols
from concurrent rendering work. **That merged payload is rejected.** The final
checkpoint uses saved v6 root content (`build-20261110-091430`), replacing only
`lib/libhakios.sb.dylib`. Repacking the unmodified base with the original
`root.apfs` filename reproduced the exact v6 hash
`2ec6e424b57eb3a0f5254d251b9e3bb1b333253ebbbe6d93aba62c6f559fdf07`.
The base and new `inject_sb.c` files are identical; the idle header is the code
change compiled into the new dylib. Final checks:

* `prepare.py verify`: exit 0.
* Host idle test: MC default/change/restriction reads, nil reporting, backend
  disabled/empty, native deadlines, inactive/disabled/manual/Never/held gates,
  release and duplicate delivery handling all pass.
* Final Metal bundle and every libhakios dylib: zero non-weak dynamic lookups.
* APFS reader: all 567 regular files match the packaged root by SHA256.
* Final root content delta from v6: only `lib/libhakios.sb.dylib`.

`SHA256SUMS`, build/test/package/symbol/readback logs, source manifests and root
file hashes are pinned alongside the payload. `source-sha256.json` describes
the new dylib's compile snapshot; `base-source-sha256.json` describes the v6 base.
No phone operation was performed. Successful MC rereading and the actual
no-fire cause still need the supervisor's runtime logs.

## auto-lock v8

2026-11-10 — `umi-autolock2`. Candidate payload fixes the supplemental idle
clock being restarted by unchanged native resets. **The upstream caller of the
one-second native reset loop is not yet established; this is not a claim that
that loop has been removed or that hardware auto-lock has passed.**

Evidence from `logs/test-1110-104546-umi-v7.union.log`: inputs remain 184 while
`resets(config/attention)` rises 382/0, 412/0, 442/0, 471/0, 501/0; `fired=0`
and idle stays below one second. Status runs every 120 ticks at 250 ms, i.e.
30 seconds, so this is approximately **one reset/second**, not ten. The
native policy already supplies 20/22/30-second deadlines and MC effective=30.

The v7 hook's unconditional `if (slot && reset)` cleared `reset_at` and `fired`.
20F66 disassembly establishes that the only direct YES caller of
`_reconfigureAttentionClientAndReset:` is `SBBacklightIdleTimer.reset`
(`0x1c4d6b66c`, call at `0x1c4d6b6cc`). The other direct callers (descriptor
setter, attention callback and expectation adjustment) pass NO. Thus the old
`config` counter was counting explicit native resets, including our HID resets;
it was not evidence of repeated settings notifications. MC observation and
status sampling only read state. Battery notification polling is five seconds,
and the cellular status poll is three seconds; neither cadence identifies the
one-second source. No evidence justifies changing either, or CommCenter.

Changes:

* Delete reset-flag-driven epoch/fired clearing. Compare a copied native
  `SBIdleTimerDescriptor` with `isEqual:`; only changed policy resets the
  supplemental clock, including a change delivered with reset=NO. The native
  equality implementation at `0x1c4a2d844` compares mode and four intervals,
  excluding audit history. Equal newly allocated descriptors do not count.
* Actual HID notifications now clear the epoch/fired bits directly before
  requesting the normal native reset. Attention callbacks retain their reset
  behavior. Inactive/disabled/manual policy keeps the clock fresh so releasing
  an assertion does not immediately deliver an overdue timeout.
* `reconfigures` counts native requests separately from actual configuration
  changes. A single stack (maximum 12 frames) is logged when an unchanged YES
  reset arrives more than two seconds after activity, with no held input. Shared
  cache frames have the cache slide subtracted for offline symbolization.

Original native reconfiguration/reset processing is still forwarded. The fix
prevents those requests from starving the existing supplemental timeout path;
it does **not** suppress arbitrary native callers. If native resets still undo
the dim, return the stack to identify and remove the actual producer. There is
no new timeout fallback, forced Auto-Lock value, boot argument or phone operation.

Changed files (relative to this device directory):

* `overlays/userland/src/umi_idle_timer.h`
* `tools/test-idle-timer.m`
* `DISPLAY-BRIGHTNESS-umi.md` (this section)

Validation and delivery:

* Required full CA build passed without compiler warnings:
  `out/xiaomi-mi-10/flash/build-20261110-113146`.
* `devices/xiaomi-mi-10/prepare.py verify`: exit 0.
* Metal bundle and SpringBoard dylib: zero non-weak dynamically looked-up symbols.
* Host test simulates 40 seconds of unchanged one-second resets, including new
  equal descriptor objects: callbacks 1/2/3 fire once at the first tick after
  20.5/22.5/30.5 seconds. Tests also cover native duplicate delivery, HID after
  dim, held/released input, attention, changed configuration, Never and assertion
  release. The test models native objects; it cannot validate native UI effects.
* APFS readback: all 568 regular files match the build root by SHA256; zero bad
  metadata checksums. This is a full build of the shared source tree, including
  other agents' work, rather than the v7 base-image repack.

Pinned artifacts under `out/xiaomi-mi-10/umi-autolock2/` (SHA256):

```
2a38270bc3c7741104dc883cb37a61c7c80e1661369ea5f8488ca726b96ca3b8  root-flash-ui-ca.apfs
f2c746fe3fdf968b4f505c2278e7399d50039bfbccf824b499de22b6557416b4  libhakios.sb.dylib
bd7ddb0ac9b30a2c90bfaae03a06496968c0a4a5cd7ec1da8b2fffce652cdbd8  HakiosMetal
```

Build/test/verify/symbol/readback logs, `SHA256SUMS`, compile snapshot hashes,
source before/after copies, v7 log evidence, native caller list and annotated
reset/equality disassembly are alongside the payload. Keep existing kernel,
loader and boot arguments; this header still uses the already-existing `al=1`.

Exact log checkpoints for the supervisor (`...` denotes variable values):

```
hakios: umi-idle al=1 v8: unchanged reconfiguration preserves idle; HID/attention/configuration reset epoch
hakios: umi-idle effective reason=status autoLockTimeout=30 onAC=0 dontDimOrLockOnAC=0 disableAutoDim=0 dontLockEver=0
hakios: umi-idle deadline id=1 seconds=20.000
hakios: umi-idle deadline id=2 seconds=22.000
hakios: umi-idle deadline id=3 seconds=30.000
hakios: umi-idle unchanged-reset stack[0]=... (unslid)
hakios: umi-idle missing native timeout: timer=... id=1 idle=... deadline=20.00; deliver native callback
hakios: umi-idle missing native timeout: timer=... id=2 idle=... deadline=22.00; deliver native callback
hakios: umi-idle missing native timeout: timer=... id=3 idle=... deadline=30.00; deliver native callback
[Display] Set Blanked: YES
```

An on-time `hakios: umi-idle native timeout timer=... id=1/2/3` can replace the
corresponding supplemental line. With unchanged inputs/policy, `reconfigures`
may still increase but `resets(config/attention)` must stay flat and idle must
advance past one second. Touch during dim must restart the full interval;
Never and native disable assertions must still prevent locking. Return every
`unchanged-reset stack[...]` frame as well as the timeout and display lines.
Actual dim/lock and the upper native reset source require supervisor hardware
validation; they were not tested here.

## auto-lock v9: dim -> full -> off bounce (2026-11-10)

v8 (`logs/test-1110-122243-umi-v8.union.log`) dims at warn=20 s (factor 0.05,
fade 0.5), then about 1.03 s later sets factor 1.0 with fade 0 (14628 -> 14905,
69277 -> 69342, 99474 -> 99747, 124686 -> 124884). It locks at 30 s. No HID
event in that interval (FTS `events=186` unchanged, first touch at 26.47).

20F66 timeout ids (`idleTimer:attentionLostTimeoutDidExpire:` switch): 1 = warn ->
observers `idleTimerDidWarn:` (dim); 2 = quickUnwarn (warn+2 s), internal
attention state only, no observer; 3 = expire -> `idleTimerDidExpire:` +
`_stopIfManual` (lock, factor 0, Blanked YES). Stock order is dim at T-10, off at T.

Restore path: `SBBacklightIdleTimer.reset` (0x1c4d6b66c) calls
`_reconfigureAttentionClientAndReset:YES` and then `idleTimerDidRefresh:`.
`-[SBIdleTimerPolicyAggregator idleTimerDidRefresh:]` (0x1c4a30a48) calls
`[_backlightController _undimFromSource:8]` if `screenIsDim`. That is the factor 1.0 with fade 0.
id=2 is not the cause, and neither is callback order.

The v8 unchanged-reset stack (lines 10386-10397, symbolised) identifies the producer:
`__NSFireDelayedPerform` (Foundation 0x1811d9a4c) -> `SBTelephonyManager
_handleTelephonyDaemonRestart` -> `handleCallControlFailure:` -> `updateCalls` ->
`updateSpringBoard` -> `SBIdleTimerGlobalCoordinator resetIdleTimer` -> `SBBacklightIdleTimer reset`.
`-[SBTelephonyManager _serverConnectionDidError:]` (0x1c4b278dc) only does
`performSelector:_handleTelephonyDaemonRestart afterDelay:1.0`. With no
`com.apple.commcenter.xpc` (COMMCENTER-umi.md), every reconnect errors again,
so the native idle timer is reset at about 1 Hz (v7/v8 `reconfigures` +~1/s while idle).
This is also why native timeouts never arrived and needed the supplement.

Fix (`overlays/userland/src/umi_idle_timer.h`, al=1 only): `_serverConnectionDidError:`
is ABI-checked with the other idle hooks and replaced by a no-op. A real CommCenter
restart still reaches `_handleTelephonyDaemonRestart` through the
`kCTTelephonyCenterDaemonRestartNotification` observer registered in
`initWithStateProvider:` (+0x310). Host idle test passes; libhakios.sb.dylib compiles with no
warnings and 0 non-weak dynamic lookups.

Check next run:

```
hakios: umi-idle al=1 v9: ... no 1 Hz telephony reset
hakios: umi-idle telephony reconnect retry dropped (no CommCenter): 0x...
hakios: umi-idle native timeout timer=... id=1        (or: missing native timeout ... id=1)
[Backlight] Setting CoreBrightness backlight factor:0.050000 with fade duration:0.500000
hakios: umi-idle native timeout timer=... id=3        (or: missing ... id=3)
[Backlight] Setting CoreBrightness backlight factor:0.000000 with fade duration:0.185000
[Display] Set Blanked: YES
```

Between the 0.05 line and the 0.000000 line there must be no
`factor:1.000000 with fade duration:0.000000` unless there is a real touch.
