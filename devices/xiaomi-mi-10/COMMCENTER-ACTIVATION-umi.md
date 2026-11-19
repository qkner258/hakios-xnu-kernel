# CommCenter activation gates — iOS 16.5 / 20F66 / Mi 10

Research date: 2026-11-11. Scope: the arm64 iPhone10,3/Mav17 software stack, presented with an SDX55 through the HakiOS bridge. Offline analysis only; no device access, binary modification, source implementation, build or payload changes were made for this research.

**The missing-ticket Internet gate exists in 16.5.** With the reference bridge's activation indication and no cached postponement ticket, Internet data is blocked even if the modem reports “Activated” and mobileactivationd returns `Activated`.

**An Apple binary patch is not yet justified.** The 16.5 Mav driver accepts BSP activation-mode TLV **`0x15`, length 1, value `1` (`kDisabled`)**, which propagates to the exact `fActivationEnabled` flag tested by that gate. Value `2` (`kBypass`) follows the same tested paths. This provides a concrete no-patch experiment: report activation disabled as part of the emulated Apple modem's activation information, alongside activated/provisioned/accepted-SIM status. Static analysis supports it; notification timing and end-to-end data still need a boot.

## Evidence conventions and inputs

**[S]** means a static instruction, pointer, symbol, string or plist was checked in the local 20F66 image. **[R]** means reference implementation/documentation. **[B]** means a proposed behavior still requires a hardware boot. A string alone establishes the presence of a code path, not that it executes for this carrier/device. Function names below are exported names, RTTI class associations or descriptive labels recovered from code/logs; stripped functions are identified by their start address. No invented method name is presented as an exported symbol.

All addresses are **unslid virtual addresses** in the named image, unless explicitly labeled file offsets. Shared-cache dylib addresses are not file offsets and cannot be used as offsets into an extracted dylib.

Inputs reused, without another IPSW/cache extraction:

- Plan: [COMMCENTER-umi.md](COMMCENTER-umi.md). Its activation unknowns are answered here; some of its “today” source descriptions are already outdated.
- Reference: `ios-port-research/repos/UniXNU_UniXNU/docs/porting/baseband.md`; `tools/tmp/commcenter-umi-research/QcomBaseband`, especially `baseband/coachz_bb_data.h`, `coachz_bb_sahara.h`, and `platform/coachz/tools/coachz_cellular_compat.py`.
- `tools/tmp/cc-fw/BuildManifest.plist`: `ProductVersion=16.5`, `ProductBuildVersion=20F66`, supported products `iPhone10,3` and `iPhone10,6` [S]. This report follows the Qualcomm Mav path, not the Intel path.
- Executables from `work/umi-v23/sysimage/rootfs`; unchanged analysis copies of CommCenter, mobileactivationd and lockdownd under `tools/tmp/cc-activation-20F66/`.
- Existing extracted frameworks/dylibs under `tools/tmp/dsc-20F66/`; original cache under `work/umi-v23/sysimage/20F66__iPhone10,3_6/`. Examined CommCenter, CoreTelephony, libCommCenter support libraries, libATCommandStudioDynamic, libBasebandManager, libBasebandCommandDriversQMI and libBBUpdaterDynamic.
- Tools: installed Python 3, `otool`, `nm`, `strings`, `codesign`, `/opt/homebrew/opt/llvm/bin/llvm-objdump`, and the existing Mach-O helper in `tools/tmp/cc-scripts/`. No packages installed. Disassembly/xref helpers and reports are in `tools/tmp/cc-activation-20F66/`.

CommCenter identity [S]:

```text
Path: /System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter
Architecture: thin arm64
Size: 24740160 bytes
LC_UUID: 5A907E0E-BF03-376B-94E6-E737BDD40300
SHA256: c3278d002d32a2402b9480a10452f5f1df3f194c571abe640daa95ef3b72231e
__TEXT vmaddr: 0x100000000; fileoff: 0
```

The source executable and analysis copy have the same SHA256. Chained pointer rebases were decoded when resolving vtables; naive raw pointer searches miss them. Helper-generated register annotations can retain stale register values, so the instruction operands and independently decoded pointers are authoritative.

## Gate table

This enumerates the activation and adjacent readiness/policy gates identified along the physical-SIM Internet path. It does not claim complete decompilation of every carrier, emergency, IMS or eSIM variant. **Settings** below means the Cellular page and its controls: no evidence establishes that the ticket boolean alone controls visibility of the entire page. UI effects marked “unknown” need a boot or a separate Preferences/SpringBoard call-graph audit.

| Gate / source | Effect when unmet: bars; carrier name; Settings; data | Cheapest no-Apple-binary-patch route | Evidence |
| --- | --- | --- | --- |
| **1. Cellular stack exists and starts.** DT `/baseband`, product `baseband-chipset=mav17`, launch job, AppleBaseband/PCI interfaces, PDP manager and AppleIPAppender | No native service/registration display; no native operator; page absent/nonfunctional possible; no native data | Publish matching services and DT, load cellular CommCenter, preserve its sandbox/user-client contract. Current source has an opt-in `umi_modem=2` path; verify the actual built image. | Stock launch plist `_LimitLoadToDeviceTree=baseband`, `_wireless`, `KeepAlive=true`, `com.apple.basebandd.xpc`; current source described below [S]. |
| **2. ABM/BBUpdater reaches booted.** Apple boot handshake, chip identity, firmware resources, personalization policy | Prevents all downstream modem operation; page may exist without service | Emulate the Apple BHI/Sahara exchange while SDX55 runs its own firmware. Advertise Mav17 chip `0x0003b0e1` and the reference's unfused/default-key behavior; provide files where BBUpdater expects them. Mav17 acceptance remains untested. | BBUpdater unpersonalized-image rejection `0x1ed58ec44`; error `0x22`; `bbticket.der` [S]. Reference Sahara [R]. |
| **3. Device activation API.** mobileactivationd/lockdownd `ActivationState`, distinct from baseband activation | Setup/activation workflows and some APIs wait/fail; exact standalone bars/name/page effect unknown; not sufficient to authorize data | Retain the existing mobileactivationd `Activated` response and SpringBoard hook. Verify injection actually loads. A genuine matching activation record is the native alternative; a state string is not that record. | HakiOS `inject.c:1244` onward; CommCenter device-state failures `0x1000ba7b0`, `0x100bc084c`, MobileActivationController paths below [S]. |
| **4. Apple baseband activation state/policy.** BSP `0xb00c`, ActivationInfo, `/cc/props/baseband_activated` | Can defer radio online, suppress operator updates/card provisioning, and reject data settings; page visibility not proven dependent | Report activated state plus **mode `kDisabled` via TLV `0x15=1`**. Both are deliberate Apple-facing bridge state; neither substitutes for real SIM/network authentication. | Model `0x100843a98`, `0x100843140`, `0x1008440e8`; data-settings rejection `0x100b31b88`; mode chain below [S/B]. |
| **5. Apple service/card provisioning and SIM acceptance.** BSP `0x4f`, `0x50/0x51`, `0x56/0x57`, SIM identity records | Empty/aborted operator name; normal subscriber service/data can fail; Settings may show a SIM/provisioning problem | Populate provisioned/accepted state and the actual active SIM's information; update it after UIM reads and SIM changes. Do not invent a SIM or confuse an absent slot with a present subscriber. | `Device is not provisioned, returning empty short operator name` at `0x1013efa02`; use `0x1006e7684`; `Baseband Not Service Provisioned` at `0x10125f387`; driver TLV sites below [S]. |
| **6. Real SIM readiness / PIN / network personalization.** UIM status and card authentication, plus Apple-side acceptance policy | No normal subscriber registration/data; carrier/name may be missing or limited; Settings can present PIN/activation errors | Forward actual UIM state, use an eligible SIM, unlock its PIN normally if needed. Model Apple's accepted-slot result consistently. Bridge status cannot unlock a genuinely locked SDX55 or satisfy network authentication. | `sim is pin locked`, `0x101428cb4`, used in `0x100ac0afc`; BSP accepted-slot callbacks [S]. Exact Apple ticket-derived SIM-lock policy is not completely reconstructed. |
| **7. Radio-online activation permission.** ActivationInterface activated/enabled queries, ticket state and subscriber interface | Can hold RF offline: no normal bars/name/data; Settings may remain accessible | Mode disabled or activated state passes the activation part; provide subscriber interface, respect airplane mode, forward DMS operating-mode commands. | Policy `0x100733a6c`, explicit waiting log at `0x100733be0` [S]. |
| **8. Radio assertions and actual NAS registration/PS service.** Airplane/brick policy, NAS/DMS | Offline/unregistered radio prevents normal service; page can show “No Service”; no subscriber data | Let native controls own radio mode; satisfy actual NAS registration and packet-service attachment on the subscribed network. Check for held airplane assertions rather than spoofing registration. | Airplane path `0x100732c84`; brick assertion `0x1004cd134`; Mav driver `DataContext activation is not possible: radio is not ready`, `0x1f0f377d8` [S]. |
| **9. Carrier selection, APN and configuration transfer.** SIM matching, installed carrier bundle, BSP PRI/GRI/config exchange | NAS bars may exist; carrier presentation/Cellular options can be incomplete; no usable Internet APN or repeated modem resets | Supply real SIM matching inputs, keep appropriate original carrier bundles, acknowledge Apple-private carrier file/config protocol as the reference does. Use the carrier's valid APN and its normal settings mechanisms. | `No default APNS` at `0x1014240ea`, function `0x100a6ff18`; no APN entries in `0x100a6edb8`; BSP `0xa000/0xa001/0xe00d` [S/R]. Carrier-bundle signature enforcement was not fully traced. |
| **10. Postponement ticket availability AND activation enabled.** Internal property → DataServiceController → Internet context selector | **Blocks Internet context selection.** Bars/name can already work. Not merely an eSIM check; no proven direct whole-page hiding rule | Preferred: BSP `0x15=1`, producing `fActivationEnabled=false`. Alternative: an appropriate valid activation-record dictionary through the native ticket API. Existing `Activated` hook alone fails this gate. | `DataConnectionInternet` / probe method `0x100e9b7c4`, rejection `0x100e9b898`; full proof below [S/B]. |
| **11. Stored-ticket lock/parse/send/acceptance, if using tickets.** `kPostponementTicket`, lock, obliteration, `ActivationTicket`, BSP `0xb00d` | Can stop ticket delivery/baseband activation; indirectly affects operator/radio/data. Not an extra mandatory ticket for mode disabled | With disabled activation, native code intentionally skips ticket push. Otherwise use the legitimate native record/API, unlocked storage and correct driver exchange; arbitrary dummy base64 is not established sufficient. | `issueStoredTicket_sync` `0x1008a40b0`; extractor `0x100842ab8`; driver send `0x1f0e47f8c` [S]. |
| **12. Carrier provisioning / data-plan / OMADM policy.** Per-slot state, `DeviceManagement`, data-plan manager, actual subscription | Bars/name/page can work while data is pending or rejected | Use a SIM with an active data plan and matching bundle. Complete the selected carrier's provisioning if required. No universal “disable entitlements” plist identified. | Remaining Internet checks `0x100e9b868`; `DeviceManagement` helper `0x10088c76c`; data-plan wait `0x100b3224c`; failure `0x10069e9d4` [S]. |
| **13. User and system data permission.** Cellular Data, roaming, per-app/management policy; data provisioned state | Normally bars/name/page remain; data globally or per app blocked | Enable the native controls appropriate to the SIM/network and allow data under actual policy. Prefer normal APIs/settings to speculative preference edits. | `0x100d9e56c`: `PDP Context cannot be made active while Data is disabled(0/1)` and `Data is not provisioned` [S]. |
| **14. Context and IP transport readiness.** WDA/WDS, mux/endpoint binding, network acceptance, PDP ifnet/AppleIPAppender, configd | Bars/name/page can work; WDS failure prevents PDP; WDS success without working IP still prevents application data | Forward real WDS start/status/runtime settings; negotiate supported WDA/QMAP, bind PCIe endpoint `{3,4}`, route muxes to interfaces, configure addresses/routes/DNS through native stack. Avoid competing kernel/userland WDS ownership. | Existing bridge/porting evidence [R]; radio-ready rejection in M driver [S]. This is transport, not an Apple activation ticket. |
| **15. Activation XPC authorization.** Privileged CoreTelephony ticket setters and client entitlements | A custom client may be unable to provision/query activation; not evidence of a separate network attach gate or missing entitlement on stock Settings | Use the native authorized mobileactivation/CoreTelephony path if choosing tickets. No custom setter needed for the bridge-mode proposal. | CoreTelephony exports and handler `0x10063f06c`; mobileactivationd fine-grained entitlement [S]. Exact selector authorization remains unresolved. |
| **16. eSIM-specific activation/attestation.** VinylController ticket subscription and eUICC capabilities | May block eSIM workflows; not a reason to require eSIM implementation for physical-SIM Internet | Start with actual physical SIM/UIM. Do not advertise unsupported eUICC capability. eSIM needs its own APDU/VINYL/provisioning implementation. | Vinyl subscription `0x1003866c8`, callback `0x10038839c`, attestation failure path `0x100387b0c` [S]. |

## 1. Where the postponement property is read

The exact string `/cc/props/is_postponement_ticket_available` is at **CommCenter `0x1012360de`** [S]. It is an internal REST/property-bus value, **not a filesystem path, DT property, NVRAM variable or documented `defaults` switch**.

| Direct string reference | Owner / use | What is established |
| --- | --- | --- |
| `0x100846f0c`, function `0x100846ed8` | ActivationModel property publisher | Producer of the availability boolean. |
| `0x100c7ea0c`, function `0x100c7e9e0` | Shared boolean-sink binding helper | Called at `0x100e40284` for DataServiceController field `+0x322`; also `0x100c7e4c4` for a settings component's field `+0x121`. The latter's full downstream effect was not resolved. |
| `0x10060b920`, function `0x10060b5b8` | `CSIRadioModulePrivate::init`, field `+0x1a0` | Callback `0x10060c250` stores the bool and forwards it to radio virtual slot `+0x28`. Radio policy is conditional, not “no ticket always means no RF.” |
| `0x1003866c8`, function `0x10038601c` | `VinylControllerImpl`, field `+0x188` | Callback `0x10038839c`; eSIM/attestation consumes activation information too. |
| `0x100e3b418`, function `0x100e3b35c` | `MobileStoreDemo` | Callback `0x1009c1554`; no proof this independently gates ordinary physical-SIM Internet. |

The literal was found in CommCenter, not in the checked CoreTelephony, libCommCenter M/K/common/Base/AWD/CNTarget, ATCommandStudio, ABM, BasebandCommandDriversQMI or BBUpdater images. This corrects the older plan's loose attribution to both CommCenter and MCommandDrivers. The M driver supplies activation information; the executable contains this property and the demonstrated data policy. Absence of a literal is not proof of absence of indirect dependencies.

### Producer and meaning of “available”

CommCenter **`0x100842ab8`** extracts `ActivationTicket` from a CFDictionary (`ActivationTicket` cstring `0x10124fafb`, CFString `0x101642ac0`). It requires a CFString, base64-decodes it at `0x100842b40`, and keeps decoded bytes at model `+0x2e0/+0x2e8`. At `0x100842b94–0x100842bc0`, nonempty decoded bytes set availability at `+0x448`, with a change notification [S].

This establishes **presence of cached bytes**, not successful Apple cryptographic validation. The function then calls ticket decoding at `0x100842d50`; the iPhone-specific path calls `0x10030c9b8` and can log `Failed to decode activation ticket` (`0x1014071d9`). Missing-record errors are `Unable to extract postponement ticket` (`0x101406e3d`) and `Did not find postponement ticket in activation record dictionary` (`0x101406e66`).

Consequently, putting arbitrary base64 in a plist might change this one availability flag, but is **not** a demonstrated solution for parsing, SIM policy, activation status and modem acceptance. The mode TLV is the more direct native policy input for a modem with no Apple activation machinery.

### Proven Internet rejection

RTTI/vtable resolution associates **`0x100e9b7c4`** with slot **`+0x3e8`** of `DataConnectionInternet` and `DataConnectionInternetProbe`. Vtables: `0x101554dc0`, `0x10160b540` (Internet), `0x1015542f8` (probe).

```asm
0x100e9b7e4  ldr  x0, [x0, #0xe8]       ; data-service interface
0x100e9b7e8  ldr  x8, [x0]
0x100e9b7ec  ldr  x8, [x8, #0x178]      ; ticket available
0x100e9b7f0  blr  x8
0x100e9b7f4  tbnz w0, #0, 0x100e9b80c  ; ticket -> continue
0x100e9b7f8  ldr  x0, [x20, #0xe8]
0x100e9b7fc  ldr  x8, [x0]
0x100e9b800  ldr  x8, [x8, #0x1b0]      ; activation enabled
0x100e9b804  blr  x8
0x100e9b808  tbnz w0, #0, 0x100e9b898  ; enabled without ticket -> reject
0x100e9b80c  ...                         ; remaining normal checks
...
0x100e9b898  mov  w8, #-2
0x100e9b89c  str  w8, [x19]              ; output reason
0x100e9b8a0  mov  w0, #1                 ; blocked
```

DataServiceController primary vtable is `0x1016055c0`; its secondary interface vtable is **`0x101606270`**, with `this = primary + 0x40`:

- Secondary `+0x178` → `0x100e63248`, reading byte at secondary `+0x2e2` = primary **`+0x322`**, the subscribed ticket value. Primary `+0x670` → `0x100e63240` reads the same byte.
- Secondary `+0x1b0` → `0x100e51ac4`, reading secondary `+0xfa` = primary **`+0x13a`**, `fActivationEnabled`.
- Diagnostics confirm the field meanings: `DATA:: fTicketAvailableSink: %s`, string `0x101460169`, use `0x100e49f28`; `DATA:: fActivationEnabled = %s`, string `0x1014601d2`, use `0x100e4a018`.

Equivalent decision:

```text
if (!ticketAvailable && activationEnabled): reject Internet, reason -2
else: continue other carrier/context checks
```

This is not only a registration-display, carrier-name or eSIM check. Other context selectors also test this pair: `DataConnectionProvisioning` at `0x100fb5720`, `DataConnectionOMADM` at `0x100fb6738`, and shared `DataConnection` selection at `0x100f3a568` (ticket call `0x100f3a6b4`). The shared implementation is referenced by MMS and several other context vtables, with earlier context-dependent exceptions; do not infer that every emergency context is blocked. The fallback Internet-only patch below would not change those other selectors.

## 2. Recommended no-patch input: BSP ActivationMode

The model defaults to enabled: **`0x100840a7c` stores zero at model `+0x288`**. Merely reporting `Activated(0)` in the usual state TLV does not change that mode [S].

The complete checked propagation chain is:

| Stage | 20F66 evidence |
| --- | --- |
| Mav activation driver receives ActivationInfo indication | `libCommCenterMCommandDrivers.dylib`, handler `0x1f0e467a8`; init `0x1f0e460f0` logs `Initializing Mav13 command driver`. “Mav13” is a class/path name also used by this later Mav stack. |
| Parse optional mode TLV | Handler installs callback `0x1f0e48950` at `0x1f0e46ad4`; `0x1f0e46afc` loads TLV **`0x15`**, calls parser `0x1f0e4a1f4`. Block descriptor `0x20cf5c008` points to signature `v16@?0r^{ActivationMode=C}8` at `0x1f0f8aa57`: one-byte input. |
| Accept mode | Callback `0x1f0e48950`: load byte; subtract 1; unsigned compare with 1; reject larger. Thus **only 1 and 2** update the optional field. Writes value at ActivationInfo `+0x1f8` and present flag at `+0x1fc`. |
| Name enum values | `libCommCenterBase::asString(ActivationMode)`, `0x1ef7876c0`, table `0x20ccf5cb8`: 0=`kEnabled` (`0x1ef7fe01f`), 1=`kDisabled` (`0x1ef7fb6a2`), 2=`kBypass` (`0x1ef7fff68`). |
| Copy into model | CommCenter model update `0x100843a98`; instructions `0x100843fcc–0x100843fd8` check optional-present and copy to model **`+0x288`**. |
| Query whether activation is required | Model vtable `0x1015a5a58`, slot `+0x118` → **`0x100845100`**: returns **`mode == 0`**. |
| Forward through activation interface | ActivationController vtable `0x1015adf70`, slot `+0x50` → `0x10089db78`; synchronous block **`0x1008a6588`** forwards to model slot `+0x118`. |
| Cache for data policy | `DataServiceController::handlePostponementStatusChanged_sync`, **`0x100e4bd84`**: calls ActivationInterface `+0x50` at `0x100e4bddc`, stores result in **`+0x13a`** at `0x100e4bde4`. Its log at `0x100e4be5c` explicitly names `fActivationEnabled`. |
| Internet selector | `0x100e9b800` reads that flag. With mode 1 or 2 propagated, missing ticket no longer selects rejection `0x100e9b898`. |

The mode also affects the earlier activated check. Model **`0x100843140`** returns true for nonzero mode; otherwise it checks ordinary activation state. ActivationController slot `+0x48` forwards through `0x1008a6570` to this model getter at slot `+0xd0`. Model update **`0x1008440e8–0x100844138`** publishes `baseband_activated=true` for nonzero mode; its property literal is `0x101243c73`, publisher `0x1008462b0`.

Radio-online policy **`0x100733a6c`** queries activated and enabled separately. With an available subscriber interface it allows the activation portion if `activated || !activationEnabled || !ticketFlag || isSimUnreadable`; it can otherwise log `Device is not activated. Waiting for activation ticket to make the baseband go online` at `0x100733be0` (string `0x1013f5f8e`). The radio object's `+0x41` flag is labeled Ticket Available in its dump at `0x100731e24`. This is another reason not to equate missing cached ticket with a universal radio-off condition.

Native ticket issuance handles this policy deliberately: **`0x1008a413c` branches away when mode disables activation**, logging `Aborting ticket push because BB activation not enabled` at `0x1008a423c`. Skipping that push under mode disabled is expected, not proof of failure.

Recommended future bridge indication [B], no implementation performed here:

| BSP activation indication TLV | Input |
| --- | --- |
| `0x01` | Activated state `0`, one byte. |
| **`0x15`** | **`1`, one byte (`kDisabled`)**. QMI TLV bytes: **`15 01 00 01`**. `2` is accepted too, but has no demonstrated advantage here. |
| `0x4f` | Service provisioned `2`. Driver parse site `0x1f0e4798c`. |
| `0x50` / `0x51` | Active slot's card-provisioning state; slot-one provisioned `2` parsed at `0x1f0e46fa0`. Do not manufacture a second subscriber. |
| `0x56` / `0x57` | Apple-side slot acceptance; nonzero accepted, parse sites `0x1f0e475a8` / `0x1f0e47604`. |
| `0x42` / `0x43` | Actual slot-one SIM identifiers obtained from UIM, in the encoding the reference models; parsed at `0x1f0e46d50` / `0x1f0e46dd8`. Values must never be logged. |

Use the existing BSP service `0xe2`, activation registration/indication message `0xb00c` framing [R]. Include mode in the **first indication and every re-registration/reset replay**, not only after a SIM read. The reference's helper and current Mi 10 overlay helper omit `0x15`; their TLV buffer sizing would also need to account for an extra four bytes in any later implementation.

The unresolved part is lifecycle: DataServiceController caches mode on postponement-status notification, and that handler has an initialization guard. Verify that the first model update/startup query and subsequent reset notifications leave its cached flag false. The static chain is strong evidence for a no-patch route, not proof of the asynchronous boot sequence.

## 3. HakiOS activation today

The device overlay is authoritative for this assessment; generic `work/umi-v23/userland` also contains older activation/UI handling.

- `overlays/userland/src/inject.c:1244–1299`: replaces `-[MobileActivationDaemon getActivationStateWithCompletionBlock:]`. It invokes the completion block with `{ActivationState: Activated}` and a nil error. Its comment explicitly says **no activation record, no Secure Enclave and nothing sent to Apple**; it describes previous `Unactivated`/UIK failure. This research did not inspect device logs to confirm deployment.
- `overlays/userland/build.sh:143`: the default `libhakios.dylib` injection list includes mobileactivationd. `inject.c` dispatches to the activation hook for that process.
- `overlays/userland/src/inject_sb.c:265` onward: forces `SBLockdownManager` state to `2` and suppresses Setup Assistant requirements. This UI state does not populate CommCenter's ticket storage.
- The hook's source comment says lockdownd's ActivationState getter delegates to MobileActivation. It does **not** establish that record-copy/ticket APIs are also faked. No corresponding postponement-ticket writer was found in the checked device-overlay sources.
- **Current source differs from the older plan:** `build.sh:131–133` explicitly adds `com.apple.CommCenter`; `mkjobs.py:221` denies `com.apple.CommCenter-noncellular`, not all CommCenter; its named-job handling preserves `_LimitLoadToDeviceTree=baseband`. `loader/loader.c` emits the baseband node and Mav17 product identity for `umi_modem=2` (mode test near `3363`, DT near `3689/3715`). `drivers/umi/hakios.umi_baseband.cpp:275` calls the copied activation helper at `overlays/xnu/files/bsd/dev/coachz_bb_data.h:275`. That helper still omits activation-mode TLV `0x15`. These are source observations, not claims that a corresponding payload is built or running.

Stock mobileactivationd provides a separate record validation path: `load_and_validate_activation_record` **`0x100016150`**, `verify_activation_record_certificates` **`0x10001e6f4`**, `verify_activation_record` **`0x10001f4e4`**. Strings include `activation_record.plist` (`0x100200b26`), `/private/var/mobile/Library/mad/activation_records` (`0x10020221f`), `/private/var/mobile/Library/mad/data_ark.plist` (`0x100202293`), `Unactivated` (`0x1002049d8`) and `FactoryActivated` (`0x1002049ee`) [S]. Writing those names into an arbitrary plist does not satisfy validation.

CommCenter separately checks device activation and ticket availability:

- `0x1000ba7b0` and `0x100bc084c` have `Device is not activated! activationState= %@, queryError= %@` paths.
- MobileActivationController callback **`0x100fd6eb0`** distinguishes `Ticket is available` from `No baseband activation ticket found`.
- **`0x100fd75c8`** can wait for a ticket to be set by mobile activation.
- **`0x100fd5360`** has a factory-activation reactivation-skip path; `0x100fd564c` examines `ActivationState` in a ticket dictionary.
- The setter's factory path **`0x1008a5dd4`** logs `FactoryActivation enabled, preparing ticket in memory but not writing to disk`. Factory mode changes handling of a supplied ticket; it does not prove that changing `Activated` to `FactoryActivated` conjures a ticket or fixes the Internet gate.

No verified NVRAM/DT property or standalone `/var` boolean was found that directly overrides the Internet ticket condition. DT satisfies discovery; the existing injection satisfies a device-state query; BSP mode satisfies the demonstrated baseband/data activation policy. These are separate inputs.

## 4. Native ticket storage and “CommCenter activation XPC”

The cached dictionary is separate from mobileactivationd's signed record store. In `libCommCenterBase.dylib` [S]:

| Symbol / role | Address and value |
| --- | --- |
| `_kCommCenterPreferencesNoBackupAppID` | `0x20ccf3ed8` → CFString `0x20cd03db0` → `com.apple.commcenter.device_specific_nobackup`, cstring `0x1ef7ff516` |
| `_kPostponementTicketPlist` | `0x20ccf3f10` → key **`kPostponementTicket`**, cstring `0x1ef7ff5ef` |
| `_kPostponementTicketLock` | `0x20ccf3f18` → **`kPostponementTicketLock`**, cstring `0x1ef7ff603` |
| Obliteration marker | `0x20ccf3f20` → **`kPostponementTicketObliterated`**, cstring `0x1ef7ff61b` |
| Persistent access | `DevicePersistentCopyValue` `0x1ef781bec`; `DevicePersistentSaveValue` `0x1ef781cec` use that domain |

The likely on-disk preference file for the `_wireless` daemon is `/var/wireless/Library/Preferences/com.apple.commcenter.device_specific_nobackup.plist`; **that physical path is inferred**, whereas the domain/key are verified. Do not rely on direct file edits and cfprefsd cache timing as an established provisioning method.

CoreTelephony exports `_CTServerConnectionCopyPostponementStatus` **`0x1881eaa14`**, `_CTServerConnectionSetPostponementTicket` **`0x1881ead9c`**, and lock/unlock functions at **`0x1881eaf0c` / `0x1881eb02c`**. CommCenter XPC handler **`0x10063f06c`** reads `kPostponementTicket`, converts the XPC object to a CF dictionary, invokes ActivationInterface slot `+0x20`, and replies with `kPostponementTicketSet`. The normal setter block **`0x1008a5dd4`**, at `0x1008a5ed4–0x1008a5f1c`, stores the dictionary, clears the obliteration marker and flushes preferences.

`issueStoredTicket_sync` **`0x1008a40b0`** checks the lock (default false), whether baseband activation is enabled, and model parsing before sending bytes. M driver sender **`0x1f0e47f8c`** rejects empty input and constructs **BSP `0xb00d`** at `0x1f0e48000`, with TLV `0x01`. A ticket-based bridge would need the matching reply/indication semantics, not only the existing `0xb00c` registration response. The disabled-mode proposal avoids requiring this exchange for Apple activation.

The stock launch plist has `com.apple.commcenter.xpc` and `com.apple.commcenter.coretelephony.xpc`, and hosts `com.apple.basebandd.xpc` within CommCenter. It does not list a separate service literally named “commcenter activation.” Activation is handled through the main stack's APIs. Stock mobileactivationd carries `com.apple.CommCenter.fine-grained` entries `cellular-plan`, `spi`, `identity`; this establishes intended privileged access, **not the exact entitlement enforced by each ticket selector**. That authorization map remains open. App XPC permission and the carrier's entitlement/subscription decision are distinct.

## 5. Other activation-looking checks and remaining data policy

**BBUpdater personalization is a firmware-loading gate.** In `libBBUpdaterDynamic`, function **`0x1ed58ec44`** rejects an unpersonalized image on a fused radio: log use `0x1ed58ed24`, string `0x1ed65c934`, return error `0x22` at `0x1ed58ed58`. `bbticket.der` is at `0x1ed662dd4`, with uses `0x1ed5df158` and `0x1ed5fbe7c`. This is not the `ActivationTicket` in a postponement dictionary. The reference's `coachz_bb_sahara.h:94–104` presents default/unfused PK-hash behavior and consumes/discards Apple's boot images. That is the no-patch model to test for Mav17; the real SDX55 must continue using its own firmware. A correctly personalized Apple firmware/ticket pair is the native alternative, not something this port currently has.

**ABM's “Ticket was not provided” is a false lead for normal data.** `libBasebandManager` string **`0x1b8041ce0`**, used in functions `0x1b7f279a0` and `0x1b7f2869c`, belongs to **RF Self Test / FTM** flows (`RF Self Test in FTM requested`, use `0x1b7f27dc0`). It is not evidence of another ordinary PDP postponement-ticket gate.

**Baseband activation and provisioning affect operator display independently of ticket availability.** CommCenter has `Operator name change aborted: not activated` (`0x1013efc06`, use `0x1006e8198`) and `...not provisioned` (`0x1013efc35`, use `0x1006e81c4`). `0x1008a3958` can delay card-provisioning notification until baseband activation. The full 16.5 no-SIM log is **`BB activated, but there is no SIM, making the BB go online`** (`0x10140c221`, use `0x1008a30d8`), so the shortened phrase in reference comments must not be interpreted as a blanket radio-online prohibition. No SIM still means no normal subscriber data, and missing SIM identity harms carrier matching.

**Carrier/data-plan checks survive passing the ticket gate.** Internet selector `0x100e9b868` queries a per-slot policy flag through interface slot `+0x168` (`0x100e51a94` thunk to `0x100e5190c`) and then helper **`0x10088c76c`**, which checks for a carrier-bundle **`DeviceManagement`** dictionary (CFString `0x101643620`, cstring `0x101252ec4`). This is a separate conditional rejection; the complete meaning of the per-slot flag remains unresolved. Nearby `UseAPNValuesFromOMADM` machinery is carrier provisioning, not proof of Apple activation-ticket cryptography.

`activateDataSettings` **`0x100b3198c`** has explicit branches for current baseband activated state (`0x100b31b88`), not-provisioned baseband (`0x100b32108`), and `activation pending data plan check, bailing out` (`0x100b3224c`, string `0x10142ed51`). It also has **`activation does not require data plan check. Continue activating context`** (`0x100b322f4`, string `0x10142ed88`). Therefore an online entitlement/data-plan transaction is **not demonstrated mandatory for every LTE carrier/context**. Data-plan failure function **`0x10069e9d4`** uses `User is not subscribed to a valid data plan...` (`0x1013e9e40`). Use the active SIM's proper bundle and plan; do not assume a forged provisioned bit makes the network accept an APN.

**General data-disable policy is independent.** Function **`0x100d9e56c`** uses strings `Data is not Allowed or Blocked: PDP Context cannot be made active while Data is disabled(0/1)` (`0x10126d332`, `0x10126d38e`) and `Data is not provisioned` (`0x10126d4d9`). Radio assertion function **`0x1004cd134`** logs `Brick mode enabled; acquiring airplane mode assertion` (`0x1013cea42`). This identifies another possible RF hold, but its upstream activation relationship was not fully traced; do not infer that all unactivated devices enter brick mode.

Finally, RF online, NAS registered, WDS connected and usable IP are four different milestones. WDA/QMAP compatibility, correct mux routing, protocol plumbers, traffic unblock, addresses, configd publication, default route and DNS still matter after all activation gates pass. No Apple activation record can compensate for a broken data plane.

## 6. Optional fallback patch — specified, not applied

**Not established unavoidable. Try the mode input first.** If boot evidence shows the no-patch path cannot reach the required data-policy state, the smallest demonstrated localized change for the Internet gate is one arm64 instruction in the pinned CommCenter above:

| Item | Value |
| --- | --- |
| File | `/System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter` |
| Unslid VA | **`0x100e9b7f4`** |
| Thin-file offset | **`0x00e9b7f4`** |
| Original bytes, little endian | **`c0 00 00 37`** |
| Original instruction | `tbnz w0, #0, 0x100e9b80c` |
| Proposed bytes, little endian | **`06 00 00 14`** |
| Proposed instruction | `b 0x100e9b80c` |

The branch skips the two-input ticket/activation-enabled rejection and continues normal Internet context selection. It does not force WDS success, create a SIM, fix APNs, change the radio-online policy or remove later carrier/OMADM checks. It affects the shared Internet/probe method, not the other ticket-sensitive context methods listed above.

Encoding verification: destination minus branch address is `0x18`; the arm64 `B` immediate is `0x18 / 4 = 6`, giving instruction word `0x14000006`. The original word was read directly from the unchanged file. Guard on **SHA256, UUID, architecture, segment mapping and original instruction bytes**, not an offset reused from another build. A later deployment would also need appropriate signing/trust handling. No patcher was added and no binary was changed here.

## 7. What differs from iOS 17

The local reference is specifically **iPadOS 17.7.11 / 21H461**, not a representative binary for every iOS 17 release. Its compatibility script pins its own hash/UUID and instruction guard. Comparison:

| Aspect | iOS 16.5 / 20F66 | Reference 17.7.11 / 21H461 |
| --- | --- | --- |
| Internet policy | Missing ticket **and** activation enabled rejects | Same two-condition pattern in the pinned guard |
| Ticket / enabled interface slots | `+0x178` / `+0x1b0` | `+0x188` / `+0x1b8` |
| Candidate patch location | `0x100e9b7f4` → `0x100e9b80c` | Script guard `0x100fa5a54`, patched branch `0x100fa5a78` → `0x100fa5aa0`, replacement `0a 00 00 14` |
| Mode-based no-patch route | TLV `0x15`, model and data-policy propagation verified statically here | Not independently traced in a complete 17 binary; reference helper omits the TLV |
| Device/ticket parsing | iPhone-specific decoder path exists | Reference uses an iPad identity; arbitrary-ticket assumptions must not be transferred |

16.5 is **not inherently easier because it lacks the gate**: it has it. The newly identified mode input may avoid the reference's patch; it is not proven exclusive to 16.5 and could also help the reference. Mav17 boot-personalization details and the iPhone activation parser are additional uncertainties relative to the reference, not proven incompatibilities.

## 8. Recommended sequence and boot questions

For a later implementation/boot, preserve the existing fake device-state response, use the native BSP disabled-activation mode, and keep the Apple executable untouched initially. Supply all the independent inputs in the gate table: Apple-facing boot/services, valid SIM/UIM information, provisioned/accepted BSP state, carrier configuration, real radio/NAS/WDS operation and a working IP path.

The decisive untested observations are:

1. Does the built image actually launch cellular CommCenter under `umi_modem=2`, and does the existing mobileactivationd injection return `Activated` to its caller? Source configuration is not runtime proof.
2. Does Mav17 BBUpdater accept the emulated unfused/default-key handshake and reach booted without a personalized firmware ticket? Which additional image/loader exchanges are required?
3. Does the first `0xb00c` indication reach the analyzed Mav driver, including `0x15=1`, and leave **mode disabled, baseband activated, service/card provisioned and active slot accepted**?
4. Does DataServiceController observe **ticket available=false, activation enabled=false**, including cold boot, modem reset and SIM-read replay? If its cached flag remains true, trace initialization and postponement-status notification before considering the binary patch.
5. Does the real SIM become ready, NAS register and packet service attach? Does carrier matching choose the expected original bundle and produce an Internet APN without recurring file-transfer resets?
6. Does the chosen carrier take the DeviceManagement/OMADM/data-plan wait branch? If so, establish the relevant policy and outcome; mode disabled intentionally does not remove it.
7. Does a native Internet request reach WDS Start Network, receive valid runtime settings, bring up the expected mux/ifnet, and publish usable routes/DNS? Test application traffic with Wi-Fi off; WDS success alone is insufficient.
8. Can bars, operator name and the Cellular page work while the ticket remains absent? This is expected to be separable from Internet admission, but exact Settings presentation and the unresolved settings-property consumer need observation.

Observe only allowlisted booleans, enum names, service/message IDs, result codes and counters. Do not dump activation dictionaries, UIM payloads, complete QMI TLVs, modem identity, or unfiltered CommCenter state: those can contain subscriber identifiers. The existing dump functions used to identify field names statically are not a recommendation to invoke full dumps on hardware.

**Answer to the narrow question:** **yes**, with enabled activation policy and no ticket, 16.5 blocks Internet data. **Patch necessity: unknown, and likely avoidable** with BSP `0x15=1` plus the existing device-state hook and the remaining SIM/carrier/radio/data prerequisites. The no-patch route has a verified static chain; only a boot can establish that all lifecycle and carrier conditions are satisfied.
