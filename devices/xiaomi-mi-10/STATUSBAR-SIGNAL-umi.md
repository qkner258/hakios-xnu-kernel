# Home-screen cellular status — 20F66 / umi-v29

## v29d: geometry traced; unlimited state-change diagnostics (2026-11-12)

**The narrow-notch-region hypothesis is not confirmed; no visible-signal fix is claimed.** Read this section instead of the older instructions to reproduce before a logging budget expires: every diagnostic in `umi_statusbar_signal.h` now deduplicates the last line per object/category, with no total-count limit. The supplied v29c log cannot supply UIScreen's actual exclusion rect, native bounds/scale or the HOME eviction decision after Control Center. This is a host-only static investigation and a complete observation build for that transition.

### Static geometry and provider selection, 20F66

Disassembly and ABI evidence: `tools/tmp/sb-signal-v29d/` (same cache as v29c). The distinction between **class** and **instance** notch methods matters:

| Native method / address | Result |
| --- | --- |
| `+[..._Phone visualProviderSubclassForScreen:visualProviderInfo:]`, `0x189aab9b0` | Reads `UIScreen._exclusionArea.rect`. A positive rect Y selects DynamicSplit; the top-edge cutout path selects Split. No rect leads to the legacy/preference path. |
| `+[..._Split visualProviderSubclassForScreen:visualProviderInfo:]`, `0x189aabbb0`; matching block `0x189aabdec` | Matches `_nativeDisplayBounds.width` against each candidate's **class** `nativeDisplayWidth`, then compares exclusion width/height multiplied by `_nativeScale / referenceScreenScale` (or `_scale`) against the candidate's **class** `notchSize`. This tests width **and** cutout geometry, not panel width alone. |
| `+[..._Split1125 notchSize]`, `0x189ab4178` | Class reference notch is **209×30 points**, width read directly from cache constant `0x189ea6358`. This is a matching criterion, not the instance's notch geometry. |
| `-[..._FixedSplit referenceWidth]`, `0x189ab0aac` | `UIScreen.nativeBounds.width / class.referenceScreenScale`; if that class method is absent, divides by screen `_scale`. |
| `-[..._FixedSplit notchRect]`, `0x189ab11a8`; `notchSize`, `0x189ab12a0` | Instance notch rect is **`screen._exclusionArea.rect × screen._nativeScale / screen._scale`**. Instance notch size returns this rect's width/height. Changing only the class notch constant would not correct the layout's input rect. |
| `-[..._FixedSplit leadingCenteringEdgeInset]`, `0x189ab0c2c` | Uses half the instance notch rect's X plus `cornerRadius × 0.15`, rounded to screen scale, plus the provider's centering offset. Trailing centering adds its own class offset. |
| `-[..._FixedSplit setupInContainerView:]`, `0x189ab12bc` | Sets main guide width to reference width `R`; side span `H = (R − instanceNotchWidth) / 2`. At `0x189ab2100` the normal trailing content width is `H − leadingInset − trailingInset`. |
| `-[..._FixedSplit _edgeInsetsFromCenteringEdgeInset:trailing:]`, `0x189ab0fb4`; `trailingEdgeInsets`, `0x189ab10c4` | Let `G` be native battery width + four-bar cellular width + three-bar Wi-Fi width + `2 × itemSpacing`, and `C` trailing centering inset. Insets are `L = H − (G + C − G/2) − adjustment`, `T = C − G/2`. Therefore **normal trailing content width = `G + adjustment`**. The notch-width term cancels. A narrower screen/notch side alone does not establish reduced content capacity on this fixed-split path. |
| `-[UIScreen _exclusionArea]`, `0x189252460`; `_displayInfoProvider`, `0x188f69f88` | Exclusion comes from the display-info provider. The initial path obtains display-edge info and exclusion from `_UIScreenInitialDisplayConfigurationLoader.initialDisplayContext` and builds a `UISDisplayContext`; this layout path does not directly query a MobileGestalt notch key. |
| `-[_UIStatusBarRegion overflowedDisplayItem]`, `0x1891c2698` | Native layout `fitsAllItems == false` selects the lowest-priority display item. Both horizontal and vertical bounds matter; extra items, their required widths, rounding or relations still require the runtime decision and competing frames. |

For an actually matching 1125/3× screen, `R=375`, expected notch width 209 gives `H=83`. Those are reference calculations, **not measured runtime exclusion/nativeBounds values**. `logs/v29-snap/v29c-1235.log` lines 10703/11320 report BackBoard 1125×2436 at 3×; HOME lines 100511/101223/101270/101948 identify Split1125 and enabled four-bar retained data. They do not establish that native candidate matching succeeded: the existing `inject_sb.c` fallback allocator independently selects Split1080 for `fbwidth=1080`, otherwise Split1125. Thus the 1080×2340 physical panel is not evidence of a 360-point HOME status bar in this run. The new `selection` and `geometry` records distinguish the native selection result, allocated provider, actual UIScreen geometry and fallback allocation.

Existing loss evidence remains exact: at v29c lines 26002–26005, display `0x133d6f220` becomes disabled, placement index becomes 1, while `entry-enabled=1 entry-bars=4 data-enabled=1 tokens=0`. This instance is `UIStatusBarWindow`, not labelled HOME. The last capped item/display traces precede the CC transition. Nothing in that file proves the HOME overflow victim, actual region capacity or final ancestor alpha.

### One-boot diagnostic coverage

Only signal source `overlays/userland/src/umi_statusbar_signal.h` changed. No force-enable, priority, width, notch or alpha override was added. The older trace switch and native return values remain intact.

- Native split selection records its inputs and result; each status-bar instance records the allocated/selected provider, bounds, nativeBounds, `_nativeDisplayBounds`, public/private scales, reference scale/width, artwork subtype, safe-area insets, exclusion/notch rects and normal/expanded insets.
- Every leading/trailing/expanded/internal region records its native name, actual frame/bounds, enablement and tokens. Every competing display item records its native identifier/item class, priority, enablement, intrinsic required size and layout frame. Every item state and candidate records current index, data/placement enablement and priority, including disabled candidates. Nested group/dependency relations record fulfillment, requirements and named peer states/target placements.
- `overflow` records **every** native victim, including non-cellular peers. The real index-advance paths preserve pre-advance candidates and record `overflow-before`, `overflow-next`, or `disabled-next`; direct region token changes and placement enablement record the native caller. These observers leave UIKit's decision unchanged.
- Bar layout and attribute fixup record cellular display enablement/visibility, own hidden/alpha, display/view/dynamic alpha factors, effective ancestor alpha and the first blocking ancestor. `performForStatusBar:` registers the native `addTotalCompletionHandler:` callback, so `event=animation-end` observes the state after the animation and its subanimations finish. The native method (`0x1892430c8`) uses main-queue dispatch-group notification; no timer guesses are used.
- `geometry` ties each bar to window pointer/class, scene pointer/class, root controller, HOME/LOCK/CC responder classification, native lock/expanded state and mode. `STATUS/APP` is an unclassified responder chain; use its actual window/scene/root classes instead of assuming HOME. Reuse-window moves change the record even with unchanged cellular data.
- All old 160/256/512-event budgets are removed, including aggregator/conversion/view/item traces. Last-line storage is associated with the observed object and released with it. Mutable update-object addresses were removed from item-state lines so a new delta object alone cannot defeat deduplication. Only numeric data, native UIKit identifiers/classes and code locations are read; no telephony identities or item content strings are logged.

New installation lines must appear exactly (existing v29c placement/overflow/fixup installations also remain):

```text
hakios: sb-signal trace enabled; state changes only; no event budget; mask bits=signal,secondary-signal,data,secondary-data
hakios: sb-signal installed +_UIStatusBarVisualProvider_Split visualProviderSubclassForScreen:visualProviderInfo:
hakios: sb-signal installed _UIStatusBar layoutSubviews
hakios: sb-signal installed _UIStatusBar _prepareVisualProviderIfNeeded
hakios: sb-signal installed _UIStatusBarAnimation performForStatusBar:
hakios: sb-signal installed _UIStatusBarRegion disableWithToken:
hakios: sb-signal installed _UIStatusBarRegion enableWithToken:
hakios: sb-signal installed _UIStatusBarDisplayItemPlacement setEnabled:
```

New exact runtime format strings (printf placeholders vary at runtime; these are not observations from v29c):

```text
hakios: sb-signal geometry bar=%p role=%s window=%p window-class=%s scene=%p scene-class=%s root=%s visual=%s selected=%s mode=%ld expanded=%ld lock=%ld screen=%p bounds=%.3f,%.3f,%.3f,%.3f native=%.3f,%.3f,%.3f,%.3f display=%.3f,%.3f,%.3f,%.3f scale=%.3f native-scale=%.3f private-scale=%.3f private-native-scale=%.3f reference-scale=%.3f reference-width=%.3f artwork=%ld safe=%.3f,%.3f,%.3f,%.3f
hakios: sb-signal notch bar=%p exclusion=%.3f,%.3f,%.3f,%.3f notch=%.3f,%.3f,%.3f,%.3f class-notch=%.3f,%.3f class-native-width=%.3f leading-insets=%.3f,%.3f,%.3f,%.3f trailing-insets=%.3f,%.3f,%.3f,%.3f expanded-insets=%.3f,%.3f,%.3f,%.3f spacing=%.3f corner=%.3f
hakios: sb-signal region bar=%p role=%s region=%p name=%s enabled=%ld tokens=%ld frame=%.3f,%.3f,%.3f,%.3f bounds=%.3f,%.3f,%.3f,%.3f items=%ld frame-overflow-disabled=%ld
hakios: sb-signal region-item bar=%p region=%p display=%p name=%s item=%s priority=%ld enabled=%ld placement-enabled=%ld required=%.3f,%.3f frame=%.3f,%.3f,%.3f,%.3f
hakios: sb-signal placement event=%s caller=%s bar=%p window=%s role=%s state=%p display=%p index=%ld preferred=%ld data-enabled=%ld enabled=%ld display-enabled=%ld visible=%ld view-hidden=%ld placement-enabled=%ld region=%p region-enabled=%ld region-tokens=%ld relations=%ld view=%p view-alpha=%.3f display-alpha=%.3f view-factor=%.3f dynamic=%ld dynamic-alpha=%.3f effective-alpha=%.3f blocker=%p blocker-class=%s
hakios: sb-signal relation state=%p index=%ld relation=%p class=%s type=%ld fulfilled=%ld requires-all=%ld peer=%p peer-item=%s peer-name=%s peer-index=%ld requirement=%ld target-placement=%p target-name=%s target-enabled=%ld
hakios: sb-signal relation-peer state=%p index=%ld relation=%p peer=%p item=%s name=%s peer-index=%ld enabled=%ld
hakios: sb-signal candidate state=%p index=%ld region=%p region-enabled=%ld placement-enabled=%ld can-enable=%ld relations=%ld priority=%ld item=%s name=%s
hakios: sb-signal overflow bar=%p role=%s region=%p name=%s victim=%p victim-name=%s item=%s priority=%ld
hakios: sb-signal region-token event=%s region=%p bar=%p name=%s token=%lu tokens=%ld enabled=%ld caller=%s
hakios: sb-signal placement-enabled placement=%p name=%s enabled=%ld priority=%ld caller=%s
hakios: sb-signal state bar=%p state=%p display=%p name=%s item=%s index=%ld data-enabled=%ld enabled=%ld region=%p priority=%ld placement-enabled=%ld display-enabled=%ld visible=%ld hidden=%ld alpha=%.3f required=%.3f,%.3f tokens=%ld
hakios: sb-signal selection screen=%p selector-class=%s selected=%s native-display=%.3f,%.3f,%.3f,%.3f exclusion=%.3f,%.3f,%.3f,%.3f scale=%.3f native-scale=%.3f
```

Host-only next-log search (normal HOME → CC → HOME, then LOCK; no early-run deadline):

```sh
rg -a -n 'hakios: sb-signal (installed|unavailable|selection|geometry|notch|region|region-item|region-token|state|candidate|relation|relation-peer|overflow|placement|placement-enabled)' NEXT.log
```

Correlate `bar`, `region`, `state`, `display` and `view`. A named HOME overflow victim plus the recorded bounds/peer frames and `overflow-next` proves a capacity eviction; the geometry formulas then identify whether the input geometry, actual required item width or layout differs. A failing named dependency, disabled candidate or token/caller identifies the other enablement path. An enabled final placement with a blocking ancestor identifies presentation. Apply a geometry/provider correction only if these records demonstrate that input is wrong.

### Host validation

`bash work/umi-v23/devices/xiaomi-mi-10/flash/build.sh userland` passed, exit 0, invoked directly with no outer `build-lock.sh` wrapper. Final snapshot: `work/umi-v23/out/xiaomi-mi-10/flash/build-20261112-130604/userland`; build log: `tools/tmp/sb-signal-v29d/userland-build.log`. No compiler warnings/errors were emitted. The edited header matches the built snapshot byte-for-byte.

Compared with `build-20261112-124644`, only `libhakios.sb.dylib` changed among `libhakios*.dylib`. `nm -u -m` on the changed SB dylib and the unchanged main injection dylib is identical to baseline: **zero new undefined symbols and zero non-weak dynamically looked-up symbols**. Packaged copies have the same undefined-symbol lists. SB SHA256: `b17f266dd017f46afc9206a6a434683f10a6b88b20af0942d5bcb1717034bb1c`. Details: `tools/tmp/sb-signal-v29d/validation.json`; newly used method signatures were read directly from cache ObjC metadata in `abi.json` (including Class return `#32@0:8@16@24`, unsigned-long region tokens, and void total-completion block).

This validates compilation/linking and static private-API ABIs. Actual HOME/LOCK geometry and the post-CC eviction remain unmeasured until the new log. No device command was run. Notification source, CommCenter, kernel and hakios-metal source were not edited.

## v29c: disabled placement identified; HOME eviction cause still unproven (2026-11-12)

**No visible-signal fix is claimed.** The new observation is specific: at 12:30, closing Control Center animates the bars back to their HOME position, then they disappear. `logs/v29-snap/v29c-1235.log` confirms valid native data and an instance changing to a disabled placement, but does not record which decision disables the HOME instance after that transition. Raising cellular priority, widening the notch region, or restoring alpha would be an unproven workaround.

Read the complete report and all 718 `hakios: sb-signal` lines. The four new diagnostic categories total 441 lines: retained 78, layout 43, item-state 160, display-state 160. Actual LF line numbers:

| Lines | Evidence |
| --- | --- |
| 22336–22337 | Primary aggregation: `cell-service=2 bars=4 max=4 raw-bars=4 enabled=1`; no secondary SIM. |
| 23395, 23397 | `main mask=5->5 disabled=0 cloak=0 all-cloak=0`; UIKit full entry enabled with four bars. |
| 23431–23432 | Single-signal view `0x133fa4a00`, display `0x133d6f220`, bar `0x135910af0`: enabled 1, data-enabled 1, placement 0, tokens 0. |
| 26002–26005 | Same display becomes enabled 0, with `changed=0 entry-enabled=1 entry-bars=4 data-enabled=1 placement=1 tokens=0`. The loss is a placement decision, not missing cellular data. This instance is in `UIStatusBarWindow`, not labelled HOME. |
| 26022 | Same view shrinks from 17×10.67 to 12.75×8 and reaches effective alpha 0. That is consistent with removal animation following disablement; it does not identify the original placement decision. |
| 27102–27109 | Same single-signal item returns to enabled 1, placement 0, four bars, alpha 1. The initial disablement is not permanent. |
| 31843–31845, 97325 | A single-signal view in `SBCoverSheetWindow` is laid out with hidden 0, effective alpha 1 and four bars. These are intermediate samples; they do not establish its final visible state. |
| 34484, 39044 | Last display-state and item-state lines respectively. Both global 160-line caps have been exhausted **before** Control Center appears. Most earlier records describe emergency/carrier/network-type items. |
| 60772–60776, 60915–60916, 61625–61626 | Control Center has both normal and expanded signal views; their effective alpha alternates during expansion/collapse. Alpha 0 in this sequence alone is not a broken HOME state. |
| 100511, 101223, 101270, 101948 | Two bars in `SBHomeScreenWindow` retain enabled four-bar data with `_UIStatusBarVisualProvider_Split1125`, mode 0, expanded 0. There are **zero** HOME `layout` records in the entire file. |
| 10703, 11320 | BackBoard display is 1125×2436, scale 3. The log does not support blaming a 360-point/1080-pixel or 2× screen configuration for this run. |

New disassembly is saved under `tools/tmp/sb-signal-v29c/`, from the same 20F66 cache documented below:

- `-[_UIStatusBarDisplayItem visible]`, `0x1891cfd78`: tests only whether `_containerView` is non-null. `visible=1` with enabled 0 means the removal animation can still own the view; it is not proof that pixels remain visible.
- `-[_UIStatusBarDisplayItemState isEnabledIgnoringAnimations]`, `0x1891c53cc`: requires both `dataEnabled` and an enabled current placement. The v29c log measures the former but omits the current placement's region and relations.
- `-[_UIStatusBarRegion overflowedDisplayItem]`, `0x1891c2698`: if `fitsAllItems` is false, returns the lowest-priority display item. `_UIStatusBarRegionAxesLayout fitsAllItems`, `0x1891c4d14`, tests the first/last layout frames against the container bounds. The Split trailing table gives cellular 1104, Wi-Fi 1107, battery 1111, so overflow can evict cellular. **No overflow decision or region bounds were logged in v29c.**
- `-[_UIStatusBar _rearrangeOverflowedItems]`, `0x189242614`, and its block at `0x1891db684`: choose an overflow victim and call `updateToNextEnabledPlacement` (`0x189ac203c`). That method increments `_currentPlacementStateIndex` directly.
- `_updateToNextPlacementStateIfNeeded`, `0x1891c92c4`, also increments the ivar directly when the current candidate is disabled; `_updatePlacementWithRecursionBlock:` (`0x1891dedac`) reaches it when enablement/relations fail. **Hooking `setCurrentPlacementStateIndex:` would miss both real eviction paths.**
- `_UIStatusBarDisplayItem _updateComputedAlpha`, `0x189213f00`: uses `dynamicHidingAlpha` when dynamically hidden; otherwise multiplies display alpha by view alpha. The existing layout trace multiplies all ancestor alphas, so its zero may also belong to a fading whole status bar/window.
- `_UIStatusBar _fixupDisplayItemAttributes`, `0x189232054`, is the native attribute reconciliation point after animations. Observing it resolves the display item through the status bar's state table; a signal view's superview chain contains no `_UIStatusBarCellularItem` or display-item state. The old view-state `item=0/status=0` fields cannot answer the placement question.

### Source change and next log

Only `overlays/userland/src/umi_statusbar_signal.h` changed; the existing `inject_sb.c` include/install call integrates it. No display or telephony overrides were added. With the existing trace switch enabled:

- `display-state` and `item-state` now spend their caps only on the single/dual signal identifiers.
- The two real placement-advance methods record `placement event=overflow-next` or `event=disabled-next`, their native caller image/offset, current placement, region enablement/tokens, and candidate relations/peer item classes. These wrappers preserve the native return values.
- An actual region overflow selecting cellular records `overflow` plus peer priorities/frames and the container bounds. An index change without this preceding decision is not evidence of frame overflow.
- `placement event=fixup` records the reconciled display alpha, view factor, dynamic hiding alpha, effective ancestor alpha and first hidden/zero-alpha ancestor (`blocker-class`), with status-bar/window pointers. The fixup wrapper observes even when the native function returns early.
- Placement events and fixup have separate 512-event caps; cellular overflow has a 256-event cap. All logs contain only numeric values, pointers, native item/window classes and caller code locations. No subscription strings or object descriptions are read.

Required new installation lines:

```text
hakios: sb-signal installed _UIStatusBarDisplayItemState updateToNextEnabledPlacement
hakios: sb-signal installed _UIStatusBarDisplayItemState _updateToNextPlacementStateIfNeeded
hakios: sb-signal installed _UIStatusBarRegion overflowedDisplayItem
hakios: sb-signal installed _UIStatusBar _fixupDisplayItemAttributes
```

For the next normal-boot reproduction, capture HOME → Control Center → HOME and lock screen early. Host-only search:

```sh
rg -a -n 'hakios: sb-signal (installed|unavailable|placement|candidate|relation|relation-peer|overflow|overflow-peer|retained|layout)' NEXT.log
```

Correlate `bar`, `state`, `display`, `region` and the transition time. A HOME `overflow`/`overflow-next` pair with out-of-bounds peer frames proves region eviction and identifies the actual width/height offender. `disabled-next` with a disabled region or unfulfilled relation identifies the disabling source/competing item. An enabled placement with effective alpha 0 instead identifies the actual display/ancestor fade. A future fix must correct that demonstrated source. These outcomes cannot be selected from the supplied v29c trace alone.

Host validation: `bash work/umi-v23/devices/xiaomi-mi-10/flash/build.sh userland` passed, exit 0, with no outer build-lock wrapper. Final snapshot: `out/xiaomi-mi-10/flash/build-20261112-124534/userland`; log: `tools/tmp/sb-signal-v29c/userland-build.log`. The snapshot header matches the edited overlay. Compared against `build-20261112-122349`, only `libhakios.sb.dylib` changed. `nm -u -m` is identical to that baseline for both SB and main injection dylibs; the packaged SB dylib has the same undefined-symbol list and zero non-weak dynamically looked-up symbols. Validation details: `tools/tmp/sb-signal-v29c/validation.json`. SB SHA256: `94d06a778b0f64ba52e33224164a0dc2a2675244a9686c1cce4747f853a2ecc6`. This is compile/link validation of observation hooks, not runtime validation or a visible-signal fix. No device commands were run; no notification, CommCenter, kernel or hakios-metal source was edited.

## v29: full log disproves missing cellular entry (2026-11-12)

**HOME glyph root cause remains unresolved; this change fixes the broken diagnostics, not the visible symptom.** The complete log is `logs/test-1112-085438-umi-v29-kva.kernel.log`. Actual LF line numbers:

| Lines | Evidence |
| --- | --- |
| 4582–4590 | Legacy hooks disabled; trace installed except `unavailable _UIStatusBarCellularItem applyUpdate:toDisplayItem:`. |
| 19042, 19053 | SpringBoard's `com.apple.systemstatus.telephonystate` connection and modern CT client are accepted. |
| 23630, 25879 | `agg item=4 domain=1 sim=1 radio=1 dead=0 service=2 cell-service=2 bars=4 max=4 rat=6 providing=1 preferred=1 raw-bars=4 enabled=1`. |
| 24901–24902 | `main mask=5->5 disabled=0 cloak=0 all-cloak=0`; `ui view=0x100e102f0 provider=server mask=5 entry=1 enabled=1 bars=4 rat=5 status=5`. |
| 26048, 42919, 42927, 42968, 46500, 46507, 46545, 57600, 95956, 95970, 95987 | Other SpringBoard status-bar instances also receive `entry=1 enabled=1 bars=4`. |
| 25848, 25865–25866 | A later sparse conversion has `entry=0`; one regular cellular display item becomes disabled. No HOME/Control Center ownership or placement is recorded. |
| 419957, 438538 | Native item 4 changes to three bars and back to four, still enabled. |

The short v29 snapshot omitted initial aggregation. Disassembly of 20F66 (`tools/tmp/blastdoor/dsc/dyld_shared_cache_arm64`, saved in `tools/tmp/sb-signal-v29/`) establishes:

- `UIStatusBar_Modern _dataFromLegacyData:` (`0x1891a4454`) passes `_statusBar.currentData` to `convertData:fromReferenceData:`. That converter creates a cellular entry (`0x18919d37c–0x18919d3e0`), then calls `makeUpdateFromData:` (`0x18919dc9c`), which removes equal entries. **`entry=0` is an unchanged delta, not absent retained data.** Reposting a full entry would target the wrong cause.
- Cellular items use `currentAggregatedData`, downstream of `currentData`. `_UIStatusBarDisplayItemState updateWithData:styleAttributes:` (`0x1891dd7d0`) checks that retained store with `canEnableDisplayItem:fromData:` at `0x1891dd9a0`; placement and enablement must be measured separately.
- `_UIStatusBarCellularItem applyUpdate:toDisplayItem:` (`0x1891a8664`) and its condensed override (`0x1892609c8`) return **id**, not void. The old trace's `v` ABI check rejected the hook. `_UIStatusBarDisplayItem` uses **`visible`**, not `isVisible`.
- `_UIStatusBarDualCellularSignalView _commonInit` (`0x189272858`) constructs the Small/Flat signal views. Their positive bars do not prove the single-SIM HOME glyph is visible. Likewise, a regular view becoming disabled alone does not prove the HOME loss. The earlier v28 presentation verdict below exceeds what its unlabelled trace establishes.

**Source change:** `overlays/userland/src/umi_statusbar_signal.h` (included by `inject_sb.c`) preserves both apply methods' object returns, reads the actual display view/visibility, distinguishes single/dual signal items, and records retained data, data enablement, placement index, disabling-token count, provider/window/mode, inherited hidden/alpha, geometry and bar layers. Existing native traces and the independent trace switch remain. No telephony/display override was added.

Supervisor: keep `/var/umi-statusbar-signal.trace` present before the next normal boot, with no legacy switch. These installation lines must appear exactly:

```text
hakios: sb-signal installed _UIStatusBar _updateWithAggregatedData:
hakios: sb-signal installed _UIStatusBarCellularItem applyUpdate:toDisplayItem:
hakios: sb-signal installed _UIStatusBarCellularCondensedItem applyUpdate:toDisplayItem:
hakios: sb-signal installed _UIStatusBarCellularSignalView layoutSubviews
```

Reproduce HOME → Control Center → HOME early, before the 160-line caps. Expected output shapes (pointers/classes/numbers vary; four bars is illustrative):

```text
hakios: sb-signal retained bar=<ptr> window=<class> visual=<class> mode=<n> expanded=<0/1> current=1 current-enabled=1 current-bars=4 stored=1 enabled=1 bars=4 status=5
hakios: sb-signal item-state item=<ptr> class=<class> update=<ptr> display=<ptr> enabled=<n> visible=<n> bar=<ptr> single-signal=1 dual-signal=0 changed=<n> entry=1 entry-enabled=1 entry-bars=4 data-enabled=<n> placement=<n> tokens=<n> view=<ptr> view-hidden=<n> view-alpha=<n> active-bars=4 bars=4 mode=2
hakios: sb-signal layout view=<ptr> class=_UIStatusBarCellularSignalView parent=<class> window=<class> frame=<x>,<y>,<w>,<h> hidden=0 alpha=1.000 icon-size=<n> active-bars=4 bars=4 mode=2 layers=4
```

Correlate `bar` and `view` pointers with the supervisor's screen-transition timestamps. Healthy `current` with unhealthy `stored` isolates UIKit aggregation; healthy stores with a disabled single-signal item isolates placement/enablement. Healthy item plus missing/zero/clipped layout isolates presentation. `window=-` means not yet attached. Do not mistake Small/Flat dual-SIM children for the single-SIM glyph.

Host validation: exact `bash work/umi-v23/devices/xiaomi-mi-10/flash/build.sh userland` passed (exit 0), no outer build-lock wrapper. Snapshot: `out/xiaomi-mi-10/flash/build-20261112-093338/userland`; log: `tools/tmp/sb-signal-v29/userland-build.log`. Source header matches the built snapshot. `nm -u -m` on the new and packaged `libhakios.sb.dylib` is identical to the 08:18 baseline: zero new undefined symbols, zero non-weak dynamically looked-up symbols. `libhakios.dylib` also has no dynamic lookups. SB dylib SHA256: `e7df872fbe0ba66504f9b660b324ce36398d60d3e2d1d431bed85f71289f1a1b`. No device commands or kernel/CommCenter edits.

Host-only investigation, 2026-11-11. No device access, full payload build, CommCenter modification, or boot-arg changes.

## v28 ordered result and source fix (2026-11-11 17:26–17:27 snapshot)

The v28 snapshot is `logs/v28-snap/v28-1735.log`. It confirms that CommCenter/SystemStatus is publishing a usable native signal entry and that the old kernel-sysctl path is stale:

| Lines | Evidence | Meaning |
| --- | --- | --- |
| 4574–4593 | Startup says `up=0 registered=0 bars=0 dbm=0`, installs the observation trace, and installs the old `setSignalMode:` replacement. | The legacy hooks were present, but their source counters were all zero. |
| 23347–23350 | `agg item=4 domain=1 sim=1 radio=1 ... service=2 cell-service=2 bars=3 ... raw-bars=3 enabled=1 mask=1`. | Native telephony data is valid before UIKit conversion. |
| 24682–24683, 25373–25375, 45013 | `main mask=5->5 disabled=0 cloak=0 all-cloak=0`; `ui ... entry=1 enabled=1 bars=3` (later bars 2). | The signal bit survives the main provider and reaches UIKit with positive bars. |
| 24699–24704, 25740–25743, 42945–42953 | Initial snapshots show `active-bars=0 ... mode=0`, then the same views receive `requested=2 applied=2` and finish at `active-bars=3 ... mode=2`. | The `setSignalMode:` replacement did **not** force mode 0. Its force branch is gated by `kern.hakios.cellular_registered > 0`, while v28 reports zero; mode 0 is the native pre-update state. |
| 25475–25476 | `_UIStatusBarDisplayItem enabled=0 view=... _UIStatusBarCellularSignalView`. | The observed loss is at UIKit presentation/display-item enablement, after native data and mode are good. |
| 26509–26512 | The same cellular display item/view is later `enabled=1`, with `active-bars=3 ... mode=2`. | The view can render the native entry; enablement changes with status-bar presentation state. |
| 45011–45015 | CommCenter reports a later signal change; aggregator still reports `domain=1 ... cell-service=2 bars=2 ... enabled=1`, and UIKit receives `entry=1`. | Ongoing native updates are reaching SpringBoard; this is not a one-time startup replay failure. |

The trace does not label each status-bar instance as HOME, Notification Center, Control Center, or an app, so the exact screen transition is not encoded in this snapshot. It does establish the loss layer: native publication, aggregation, conversion, and signal-view mode are healthy; a cellular display item is disabled during the bad presentation state. The old hook is not the line that actively sets mode 0 in v28, but it is obsolete, reads counters that CommCenter no longer feeds, schedules redundant legacy updates, and contains a latent ABI error in its opt-in bars setter.

### Source change

`inject_sb.c` now leaves the legacy cellular hooks completely uninstalled by default. Create `/var/umi-statusbar-signal.legacy` **before SpringBoard starts** to restore that path for comparison. The opt-in path now calls the iOS 16.5 bars setter as `^(int bars)`; the previous call passed `(data, bars)` and could write the captured data pointer as the bar count if the old sysctl branch ever became active. The native trace switch `/var/umi-statusbar-signal.trace` remains independent.

For the next phone check, boot without the legacy switch and reproduce HOME → app → HOME, Notification Center, and Control Center with the trace enabled. Confirm that there is no `cellular: ... setSignalMode: replaced` line, native `agg`/`ui` lines remain positive, and the affected HOME/Notification Center view's display item stays enabled. If the icon is still absent while those conditions hold, the remaining fix is the screen-specific display-item/presentation transition, not telephony data.

## v27 ordered result (2026-11-11 15:38 run)

The supplied v27 trace does **not** establish a home-only loss in the published data. At lines 74078 and 75660, item 4 is still `domain=1 sim=1 radio=1 service=2 cell-service=2 bars=4 ... raw-bars=4 enabled=1`; the main provider keeps the signal bit (`main mask=5->5 disabled=0 cloak=0 all-cloak=0`, for example 75074, 75663, 75894). The long-lived view `0x101912d90` receives a full cellular entry once (75075), then receives `entry=0` (75885, 75888) while the aggregator and mask remain good. Later-created views receive full entries with four bars (75914, 91913, 91932, 92043).

`entry=0` is therefore consistent with UIKit's sparse/delta path, not evidence that the aggregator cleared the bars. UIKitCore 20F66 has `-[_UIStatusBarData applyUpdate:]` at `0x18924a50c` and `-[_UIStatusBarData dataByApplyingUpdate:keys:]` at `0x189ac0cf8`; those APIs explicitly apply updates by key and retain fields absent from a delta. `-[UIStatusBar_Modern _dataFromLegacyData:]` is `0x1891a4454`, and the posted-data path is `0x189ad7654`. The trace still cannot say whether the existing HOME view retained its four active bars, was hidden, or was clipped after a sparse update.

The existing cellular replacements are not the demonstrated cause in v27. Their startup sysctls are `up=0 registered=0 bars=0` (4579 and 70668), so `update_signal_item` never enters its setter/force-visible branch. The `setSignalMode:` replacement is installed (4580, 70669), but the observed request is already mode 2 (24181), so it passes the requested mode through. The old bars setter call remains a latent ABI defect: UIKit's native setter at `0x1c504332c` takes `(block, int bars)`, while the old hook calls it as `(block, data, shown)`. It is not exercised when the old `registered` sysctl is zero; do not infer from its presence that it caused the v27 HOME symptom.

The UIKit binary metadata also identifies the required observation points: `_UIStatusBarSignalView setNumberOfActiveBars:` (`0x18924a0b4`), `setNumberOfBars:` (`0x1892a698c`), `_UIStatusBarCellularItem applyUpdate:toDisplayItem:` (`0x1891a8664`), and `_UIStatusBarDisplayItem setEnabled:` (`0x1891c675c`). The opt-in trace now wraps those setters/update methods, in addition to the existing converter and aggregator hooks. It logs only numeric state, object pointers/classes, and a capped number of lines. No display fix is justified until this trace identifies a divergence.

## Verdict

**The exact loss point is not established by the supplied logs. The battery fix's missing-native-source explanation is not supported here.** Native telephony state and a SystemStatus publisher are wired into SpringBoard; its CT clients are accepted and its aggregator receives non-null SIM information. The old kernel-sysctl hooks are inactive in this run, so they do not explain working in-app bars. Both displays share the native feed. The strongest next lead is per-view overrides/presentation, or filtering that changes when HOME becomes visible. That is a hypothesis, not a proven root cause; a stale/incomplete telephony-domain field remains possible.

Do not add a second synthetic signal publisher or force registration/bars. Instrument the existing native path first. A small, opt-in observation patch is included below; it does **not** fix the visible symptom yet.

## Log evidence

Line numbers below are actual LF line numbers (`rg -a -n`, necessary because these logs contain NUL bytes), all in `logs/test-1111-140149-umi-v26.kernel.log`:

| Lines | Evidence and implication |
| --- | --- |
| 16361–16375 | `supported=[spi,identity,phone,carrier-settings,preferences-reset,voice]`; SpringBoard's connection named `com.apple.systemstatus.telephonystate`; `Added new client [SpringBoard (14)]`; `registered for 17 notification(s)`. Both legacy CT and modern CoreTelephony XPC clients are accepted. |
| 17329 | `Sending kRegistrationStatusNotification with kRegisteredHome, RAT: kRatLTE`. Real registration reaches CommCenter. |
| 22266 | `Computed base bars of 5 from model Mav5LTE`. Native bars exist; this alone does not prove the final domain value. The log's wall clock steps backward during startup, so use event order too. |
| 22751, 24464, 24541, 53947 | Existing hook receives primary `signal item 4` with non-null domain/SIM objects, but logs `up 0 registered 0 bars 0 dbm 0`. These numbers are **legacy kernel sysctls**, not CT or SystemStatus values. No `bars shown` override was found. |
| 23911 | `_UIStatusBarCellularSmallSignalView asked for signal mode 2`. At least one cellular view reaches native active mode; the line does not identify which screen. |
| 53941, 55793 | `kCTEventIndicatorSignalStrength (16) sent to 3 (of 49) clients.` The aggregator runs again shortly afterward (53947). This is consistent with ongoing native updates, not just one startup sample. |
| 91093, 91965 | A later SpringBoard instance, PID 371, again creates `com.apple.systemstatus.telephonystate` and receives non-null domain/SIM objects. A missed first registration is therefore a weaker explanation, although individual cached fields are not logged. |
| 51612 | `Client [bili-inter (303)] entitlement failed: 'public-signal-strength'`. This belongs to the application, **not SpringBoard**; it does not explain the home-only symptom. |
| 25849 | `SystemStatusUI - STUIDataAccessStatusDomain ... Input generated no data`. This concerns sensor/data-access attribution, not the telephony domain. |

Searched `.out`, `.kernel.log`, and `.live.log` for SpringBoard, SBTelephonyManager, CoreTelephony, SystemStatus, STTelephony, statusbar, signal/bars, invalid XPC, sandbox/entitlement failures, and “not registered.” `.kernel.log` contains the useful CT trace; the other files do not supply the missing home-screen state. No SpringBoard CT entitlement failure or telephony-publisher denial was found. Unrelated missing-service XPC errors do not establish a telephony failure. Absence of an error is not proof that every domain field is correct.

## Binary path, not an independent in-app signal source

Inspected the local 16.5 cache at `tools/tmp/blastdoor/dsc/dyld_shared_cache_arm64`, plus extracted images under `tools/tmp/dsc-20F66/System/Library/`. `nm` establishes classes/imports; `ipsw dyld macho ... -o` and `ipsw dyld disass` resolve selectors and implementation addresses. The latter is preferable to dumping ObjC metadata from standalone extractions, which have unresolved cache pointers.

```text
CommCenter / CoreTelephonyClient + legacy CTServerConnection
  -> SystemStatusServer STTelephonyStateProvider (inside SpringBoard)
  -> STTelephonyStatusDomainDataProvider -> STTelephonyStatusDomainPublisher
  -> SystemStatus STTelephonyStatusDomain / primarySIMInfo
  -> SBStatusBarStateAggregator -> raw status-bar data
  -> SBStatusBarStateProvider / SBMainStatusBarStateProvider
  -> UIStatusBarServer and local in-process providers -> status-bar views
  -> UIStatusBar_Modern -> _UIStatusBarDataConverter -> cellular views
```

Relevant symbols and instructions (unslid addresses):

- `-[STTelephonyStateProvider init]`, `0x1c47f5590`: reads MobileGestalt `contains-cellular-radio` and `CellularTelephonyCapability`, constructs `CoreTelephonyClient`, and registers legacy CT notifications. Its ObjC delegates include signal, registration, subscriber, data, and carrier-bundle callbacks. **`STTelephonyStateProvider` is the actual 16.5 class**, not `STTelephonyStatusProvider`.
- `-[SBStatusBarStateAggregator initWithMainDisplayWindowScene:registerForNotifications:]`: `0x1c50404b4` obtains `systemStatusServer`; `0x1c50404e0` onward constructs `STTelephonyStatusDomainDataProvider` using SBTelephonyManager's `telephonyStateProvider`; `0x1c50405f8` constructs `STTelephonyStatusDomain` with the server handle. The publisher is hosted in SpringBoard, not a separate absent SystemStatus daemon.
- `-[STTelephonyStatusDomainDataProvider initWithServerHandle:stateProvider:]`, `0x1c47eff2c`: adds a state observer, constructs the publisher, and immediately calls `_updateDataForSlot:0`. Thus a blanket “no initial replay” claim is incorrect.
- `_SIMInfoForStateProvider:subscriptionInfo:carrierBundleInfo:slot:`, `0x1c47f02bc`: copies bars/max bars; maps `registrationStatus` to `serviceState` and **separately** `cellularRegistrationStatus` to `cellularServiceState`; also derives the data-network type and data-connection flags. Carrier policy includes `LTEConnectionShows4G`; the UI's RAT enum is not a QMI RAT value.
- `-[SBStatusBarStateAggregator _updateSignalStrengthItem]`, `0x1c5043258`: obtains `telephonyDomain.data.primarySIMInfo`; calls the shared updater with item **4** (secondary is 5). That updater (`0x1c5043430`) normally enables bars only with SIM info, `isCellularRadioCapabilityEnabled`, **`cellularServiceState == 2`**, and airplane mode off; there is also a radio-dead fallback and an override-manager path. It then reads `signalStrengthBars`. Non-null SIM info alone is insufficient.
- `_updateDataNetworkItem:withData:primary:dataNetworkTypeGetter:dataNetworkTypeSetter:`, `0x1c5044578`: selects the data-providing SIM and checks tethering/Wi-Fi before the cellular RAT. Primary item is **9**, secondary 10. No “4G” while Wi-Fi is active can be normal, even while bars remain visible.
- `-[SBMainStatusBarStateProvider _composePostDataFromAggregatorData:]`, `0x1c4a0a03c`: clears item enable bytes according to `_itemIsDisabled[46]`. `_updateDisabledItems`, `0x1c50491b4`, combines disabling requests, `_telephonyAndBluetoothCloaked`, and `_allButBatteryCloaked`. Measure it across HOME/app transitions. It is **not exclusively a home-screen filter**: `-[SBMainStatusBarStateProvider statusBarStateProvider:didPostStatusBarData:withActions:]` (`0x1c4a39998`, server call at `0x1c4a39a18`) forwards its output to `+[UIStatusBarServer postStatusBarData:withActions:]`, feeding apps too. A constant suppression there should affect that server feed as well. Base `SBStatusBarStateProvider`'s compose method (`0x1c504eb34`) simply returns.
- `-[UIStatusBar_Modern statusBarStateProvider:didPostStatusBarData:withActions:]`, `0x189ad7654`, calls `_dataFromLegacyData:` (`0x1891a4454`), which applies local overrides and uses `_UIStatusBarDataConverter`. The converter's long `_cellularEntryWithData:signalStrengthEnabled:...` selector is on **`_UIStatusBarDataConverter`**, not any of the three classes searched by the old injection. `STUIStatusBar` in SystemStatusUI declares no own methods/ivars and inherits `UIStatusBar`; its name does not establish another signal publisher.

Existing HakiOS hooks: overlay `inject_sb.c:363–613` changes aggregator bars/service and cellular-view mode from `kern.hakios.cellular_*`; the app injection does not provide an alternative signal feed. Comments claiming CommCenter is absent predate this bridge. `umi_idle_timer.h` also suppresses SBTelephonyManager's reconnect error callback, but no invocation of its “telephony reconnect retry dropped” log was found in v26; it is not evidence of the present failure.

One **latent bug** must not be revived: the old bars hook invokes the setter with `(block, data, shown)`. At `0x1c504332c` the native setter stores **w1** as bars; its ABI is `(block, int bars)`, with no data argument. The getter takes only the block. The current zero-sysctl guard prevents the bad setter call here. It is not the cause demonstrated by this run, and feeding the old sysctls again would make it dangerous.

## Included diagnostic and proposed fix

Added `overlays/userland/src/umi_statusbar_signal.h`; `inject_sb.c` includes it and installs the diagnostic independently of the optional legacy hook. By default the old cellular hook is not installed, preserving the native CommCenter/SystemStatus path. The diagnostic:

- Installs only when `/var/umi-statusbar-signal.trace` exists before SpringBoard starts; removing the file stops logging at runtime. No hooks are installed by default.
- Checks runtime method argument/return kinds; checks ivar types and uses runtime offsets. Reads only numeric telephony fields and the enable-byte prefix, never object descriptions, SIM labels/identities, phone numbers, or messages.
- Logs domain/service/bars versus raw aggregator output, before/after main-provider item masks and cloak state, UIKit's converted cellular entry and local-provider class, signal-view `numberOfActiveBars`/`numberOfBars`/`signalMode`/hidden/alpha, `_UIStatusBarCellularItem` update state, and `_UIStatusBarDisplayItem` enabled plus its view class. Each path is capped at 160 lines per SpringBoard instance; reproduce early after startup. `-1` means a getter/ivar is unavailable.

Choose the fix from the first observed divergence:

| Next trace | Targeted fix |
| --- | --- |
| Domain `radio=0`, despite established registration | Trace the two Gestalt answers and the provider's cached/suppression state; correct the proven capability source or stale cache. Settings → Cellular alone does not establish both answers. |
| Domain bars positive but `cell-service != 2`, or stale/zero domain bars | Compare STTelephonyStateProvider's subscription cache and CoreTelephony registration/signal callbacks; repair the missing real-state update/replay or bridge response. Do not equate a working PS connection with valid CS/display registration. |
| Aggregator enabled, main mask loses bit 0 on HOME transition, `disabled`/cloak set | Identify the disabling request owner or cloak transition; release a proven stale assertion at its source. Remember the same provider also publishes the application feed; correlate its timing, rather than assuming this is a home-only branch. Do not globally force items visible through airplane mode/SOS/no-service states. |
| Published mask preserves signal, a home UIKit entry is disabled/missing | Fix the local override/conversion input or provider selection. |
| UIKit entry enabled with positive bars on the home instance | Data publication is working; inspect split-provider placement, visibility/alpha, clipping, and rendering for that view. Existing fallback-to-Split1080/1125 hooks make this worth checking. |

No display-item override was added: v28 shows that native signal data is already valid and that UIKit presentation enablement is the remaining observation point. The legacy publisher is disabled by default so the next run tests the native path without the stale hook; `/var/umi-statusbar-signal.legacy` restores it for a controlled comparison.

## Next run: exact grep targets / supervisor-only device checks

Before the supervisor's next **normal** boot, create a persistent, readable switch file using the already authorized supervisor device workflow. If using an existing root SSH session, the commands are:

```sh
touch /var/umi-statusbar-signal.trace
chmod 0644 /var/umi-statusbar-signal.trace
# Runtime log kill switch (hooks remain transparent pass-throughs):
rm -f /var/umi-statusbar-signal.trace
```

The agent did not run these commands on a device. Setting the file after SpringBoard has already started does not install hooks; arrange it before the next planned boot. Do not restart/kill CommCenter. The supervisor performs the merged build and packages the overlay header alongside inject_sb.c.

Host log searches (the first prints only numeric diagnostic lines; inspect CT targets without dumping arbitrary telephony objects):

```sh
rg -a -n 'hakios: sb-signal (trace enabled|installed|unavailable|agg|main|ui|view-state|display-state|item-state)' NEXT.kernel.log
rg -a -n "com.apple.systemstatus.telephonystate|Added new client \[SpringBoard|Sending kRegistrationStatusNotification with kRegisteredHome|Computed base bars of|kCTEventIndicatorSignalStrength" NEXT.kernel.log
rg -a -n "SpringBoard.*entitlement failed|Client \[SpringBoard.*(denied|failed)|umi-idle telephony reconnect retry dropped|Status bar data manager: telephony domain reports 0 signal strength bars|reporting 0 signal strength bars because we have no SIM info" NEXT.kernel.log
```

Expected observation installation lines (all seven must appear; otherwise look for matching `unavailable`). With the default v28 fix there should also be a line saying `legacy hooks disabled`; there must be no old `setSignalMode: replaced` line unless the legacy switch is present:

```text
hakios: sb-signal installed SBStatusBarStateAggregator _updateSignalStrengthItem:withData:SIMInfo:barsGetter:barsSetter:
hakios: sb-signal installed SBMainStatusBarStateProvider _composePostDataFromAggregatorData:
hakios: sb-signal installed UIStatusBar_Modern _dataFromLegacyData:
hakios: sb-signal installed _UIStatusBarSignalView setNumberOfActiveBars:
hakios: sb-signal installed _UIStatusBarSignalView setNumberOfBars:
hakios: sb-signal installed _UIStatusBarDisplayItem setEnabled:
hakios: sb-signal installed _UIStatusBarCellularItem applyUpdate:toDisplayItem:
```

Good registered-state examples, **illustrative values, not existing v27 evidence**:

```text
hakios: sb-signal agg item=4 domain=1 sim=1 radio=1 dead=0 service=2 cell-service=2 bars=4 max=5 rat=... providing=... preferred=... raw-bars=4 enabled=1 mask=...
hakios: sb-signal main mask=1->1 disabled=0 cloak=0 all-cloak=0
hakios: sb-signal ui view=... provider=server mask=1 entry=1 enabled=1 bars=4 rat=... status=...
hakios: sb-signal view-state view=... class=_UIStatusBarCellularSmallSignalView requested=-1 applied=-1 active-bars=4 bars=4 mode=2 hidden=0 alpha=1.000 ...
hakios: sb-signal display-state display=... class=_UIStatusBarDisplayItem enabled=1 view=... view-class=_UIStatusBarCellular...
hakios: sb-signal item-state item=... class=_UIStatusBarCellularItem update=... display=... enabled=1 visible=1 view=... view-hidden=0 view-visible=1 view-alpha=1.000 active-bars=4 bars=4 mode=2
```

Masks are hexadecimal: bits 0/1 = signal items 4/5; bits 2/3 = data items 9/10. Wi-Fi can change the data bits. UI `rat` is the converted UIKit type, not the domain enum. `provider` reports the in-process provider class, or `server` if that getter is nil. Do not infer HOME versus app from this string alone; correlate view addresses and observation timestamps.

Bad signatures to isolate: `agg ... domain=0`, `sim=0`, `radio=0`, `cell-service` other than 2, `bars>0 raw-bars=0 enabled=0`; `main mask=1->0 disabled=1`; or `ui ... enabled=0` on the observed home instance. These pinpoint different layers and must not be conflated. Startup/no-service values are expected transiently; correlate a stable home screen with the immediately preceding native updates.

For the new lines, match `display-state ... view=...`, `item-state ... view=...`, and `view-state view=...` pointers to the `ui view=...` pointer. A good HOME sample has active-bars/bars positive, mode 2, hidden 0, alpha near 1, item/display enabled and visible. A bad sample is item/display enabled 0, hidden 1/visible 0, alpha 0, or active-bars/bars 0 while the same-time `main mask` still contains bit 0. The wrappers run after the native setter/update, so they record the state actually stored by UIKit. If no setter runs after the first HOME entry, the trace must be repeated while switching HOME → app → HOME and while waiting for a later signal update; that absence is itself evidence that the sparse update did not touch the view.

On-device observation for the supervisor: shortly after registration, compare HOME → app → HOME, then Control Center, recording timestamps. Compare Wi-Fi on/off with the same data state; separately observe a later signal update. Correlate the home view address/provider with the `ui` lines and main-provider filtering with transition timestamps. If needed, `sysctl kern.hakios.cellular_up kern.hakios.cellular_registered kern.hakios.cellular_bars kern.hakios.cellular_dbm` prints only the old numeric counters; zero does **not** mean CommCenter is unregistered. Avoid subscription-object dumps and unrestricted CT log exports.

## Host validation / files

Passed the overlay's exact dynamic-library compile/link commands (the `DYN` commands at `overlays/userland/build.sh:270–273`) for both `inject.c` and `inject_sb.c`, using the existing generated header include directories. Outputs: `/private/tmp/libhakios.inject-v28.dylib` and `/private/tmp/libhakios.sb-v28.dylib` (both `arm64` Mach-O shared libraries). This validates compilation/linking, not runtime private-API availability or the handset UI. No payload build was run.

Repository changes: this report and `overlays/userland/src/inject_sb.c` (legacy hook gate plus corrected setter ABI). The existing value-only observation header and integration remain unchanged. All forbidden files were left untouched.
