# Status bar on umi (HakiOS 16.5 / 20F66)

## v29h data-type item ("4G" missing on the home screen after boot)

Symptom (v29g trace, `work/v29g/INVESTIGATION-sb-trace-abort.md` §4): after a boot the HOME/Split
bar shows the signal bars but not the data network type; Control Center shows "中国移动 4G" and
CommCenter reports `indicator=4G (7)` every ~10 s. A respring brings the "4G" back. The home
bar's data-type display items had `data-enabled=0` for `_UIStatusBarCellularCondensedItem
animatedTypeDisplayIdentifier` and the expanded/secondary type identifiers.

### Binary evidence (SpringBoard, 20F66, unslid addresses)

- `-[SBStatusBarStateAggregator _updateDataNetworkItem]` `0x1c4a07be8`: reads
  `telephonyDomain.data` (SystemStatus `STTelephonyStatusDomainData`), then calls
  `_updateDataNetworkItem:withData:primary:dataNetworkTypeGetter:dataNetworkTypeSetter:` with
  item **9**, primary **1**.
- `-[SBStatusBarStateAggregator _updateDataNetworkItem:withData:primary:dataNetworkTypeGetter:dataNetworkTypeSetter:]`
  `0x1c5044578`: chooses the data-providing SIM from `primarySIMInfo`/`secondarySIMInfo`, prefers
  tethering/Wi-Fi, then the cellular RAT; calls `_setItem:enabled:` for item 9 and, on a type
  change, the setter block plus `_notifyItemChanged:`.
- `-[SBStatusBarStateAggregator _setItem:enabled:]` `0x1c4a08038` → `_setItem:enabled:inList:
  itemPostState:` (`0x1c50479a4`) writes `itemIsEnabled[item]` and posts through
  `_postItem:withState:inList:` when it changed.
- `-[SBStatusBarStateAggregator updateStatusBarItem:]` `0x1c4a079fc`: `beginCoalescentBlock`,
  jump table at `0x1c519a8c2` (item 9 → `0x1c4a07aa8` `_updateDataNetworkItem`, item 10 →
  `0x1c4a07ab4` `_updateSecondaryDataNetworkItem`), `endCoalescentBlock`.
- The aggregator's own telephony-domain handler: `_registerForNotifications` registers
  `-[STTelephonyStatusDomain observeDataWithBlock:]` at `0x1c5041794`; the block reached from it
  calls `updateStatusBarItem:` for **4, 5, 6, 7, 9, 10, 32, 29, 30, 31, 45** (handler code at
  `0x1c5042bb4`). `_block_invoke_24` `0x1c5042d64` is another native caller of 9 and 10. So
  `updateStatusBarItem:9` is exactly the call SpringBoard makes when telephony data changes.
- `-[SBStatusBarStateAggregator _statusBarData]` `0x1c5040778` is `return self + 0x1a8`; the first
  member at that address is `_Bool itemIsEnabled[46]` (same offset as the list passed by
  `_setItem:enabled:`).
- `-[SBStatusBarStateProvider init]` `0x1c504e964` obtains the aggregator from
  `[SBApp statusBarStateAggregator]` and stores it weakly (`_stateAggregator`); `SpringBoard`'s
  `-statusBarStateAggregator` `0x1c4ab6b74` returns `_statusBarStateAggregator` (and asserts if
  it is nil). `SBMainStatusBarStateProvider` (the HOME bar's provider) is what registers with it.

So item 9 is enabled exactly when the SystemStatus telephony domain says a SIM provides data and
has a recognised type. Boot-time the aggregator's first snapshot predates SpringBoard's second
batch of CommCenter subscriptions, and the later periodic `dataStatus` traffic does not re-enable
the item (the v29g trace could not see why; the domain observer above should have fired).

### Fix implemented (inject_sb.c)

`umi_data_type_nudge()` in `overlays/userland/src/inject_sb.c`, scheduled from the SpringBoard
constructor at 7 s on the main queue and repeated every 4 s, at most 3 times:

1. Read SpringBoard's `_statusBarStateAggregator` ivar through `umi_signal_ivar_object` (not the
   asserting getter). If item 9 is already 1, stop silently.
2. With `respondsToSelector`-equivalent runtime checks (`v`, 3 arguments), call
   `updateStatusBarItem:` **9** and **10** — the same call the native telephony observer makes.
3. Log one line per nudge: `hakios: sb-data-type nudge N: item 9 a->b, item 10 c`.
4. Repeat only while `_statusBarData` shows item 9 still not 1.

No new hooks, no `-[_UIStatusBarDisplayItem view]`, no `intrinsicContentSize`, no polling.

### Risks

- Private API names/ABI (`updateStatusBarItem:` item indices, `_statusBarData` layout) are 20F66
  specific; the runtime type checks and `respondsToSelector`-style guards make a mismatch a log
  line instead of a crash, but a future iOS would need the offsets re-checked.
- With Wi-Fi active item 9 is disabled by design; the three nudges then just re-evaluate it and
  log three lines (bounded, no loop).
- Reading the ivar avoids `-[SpringBoard statusBarStateAggregator]`'s nil assertion; if the
  aggregator is not there yet the nudge logs "no aggregator" and retries on the next slot.
