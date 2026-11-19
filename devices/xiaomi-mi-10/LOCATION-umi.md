# Location on umi (iOS 16.5 locationd on the Mi 10)

Status 2026-11-11, after phone run v27 (`out/xiaomi-mi-10/runtime/logs/153920-run.log`,
`logs/test-1111-153851-umi-v27.kernel.log`).

Rule for everything here: no coordinates in any log, test output or document. Tools print accuracy, age,
speed and counts only. The kernel must never printf a latitude or longitude.

## 1. What has been changed so far

| File | Change |
|---|---|
| `overlays/userland/mkjobs.py` | `com.apple.locationd` is no longer in DENY. It ships on demand like the other `--rest` jobs (RunAtLoad, KeepAlive, StartInterval and LaunchEvents removed). `KILL_SWITCH` gives it `_LimitLoadFromDeviceTree = hakios-no-location` and drops its 3 s ThrottleInterval. `LOCATIOND=0` in the build environment puts it back into DENY. |
| `loader/loader.c` | `location=0` is whitelisted. With it, the loader makes the device-tree node `/hakios-no-location`, and launchd then does not load locationd. |
| `overlays/userland/src/loc_test.m` + `.entitlements` | Test client (section 4). It was updated after v27, see below. |
| `overlays/userland/build.sh` | Builds `loc_test` and copies it to `/redwood/bin`. After v27 it gained a signing fallback: without `ldid`, the host's `/usr/bin/codesign -f -s - --entitlements` signs it. The `codesign` on the build PATH is xnu-deps' `exit 0` stub, so the full path is required. |
| `overlays/userland/rc.jobs` | `mg_test gps location-services ...`, then `loc_test 300 &` in the background. It was `loc_test 90 &` in v27. |
| `tools/gpsd-proto-dump.py` | Prints the protobuf schemas compiled into `/usr/libexec/gpsd` and `locationd`. Write the output under `out/` or `tools/tmp/`, never into the repo, because the schemas are Apple's. |

Changes since v27 (not yet on the phone):

- `loc_test` now asks for updates on behalf of a system location bundle (`-initWithEffectiveBundlePath:`, entitlement
  `com.apple.locationd.effective_bundle`). The default is `/System/Library/LocationBundles/MobileWiFi.framework`, which locationd
  authorized by itself in v27 as soon as Location Services was switched on (wifid: "new status: 3").
  An optional second argument names another bundle; `-` asks as the bare program, which is the v27 behaviour.
- `loc_test` prints how many fixes arrived and the best accuracy. Each fix is labelled GNSS-like (30 m or better), Wi-Fi-like (300 m or better) or cell-like.
- In v27, `loc_test` was **unsigned**: the build log has `no ldid: loc_test is unsigned`, and `codesign -d` on the payload copy says "not signed at all".
  That is why it could neither enable Location Services nor be authorized. The codesign fallback fixes this.
  It was checked on the host: the binary compiles with the build's own `xnu-clang -isysroot iPhoneOS16.5.sdk -O1 -Wall -Wextra -fobjc-arc` with no warnings,
  and `codesign -d --entitlements :-` shows both keys in the XML blob, which `kern_minimac.c` reads with `cs_entitlements_blob_get`.

## 2. Kill switches

- **Boot-arg `location=0`**: the same payload, with locationd never loaded.
  It is in `out/xiaomi-mi-10/flash/loader.efi` (built 2026-11-11 15:37; `grep -c hakios-no-location` gives 1). It is **not** in `handoff-1111/loader-v11.efi`.
- **Build env `LOCATIOND=0`**: locationd's plist is left out of the payload.
- `loc_test` runs in the background with `alarm(SECONDS+20)`, so a hung locationd costs only the line `loc_test: TIMEOUT`.

## 3. Diagnosis from v27: why nothing can locate

locationd itself is healthy:

- `Successfully spawned locationd[67] because ipc (mach)` was logged once (15:39:43), with no exit and no crash in 20 minutes.
- It registers with CommCenter (22 notifications) and answers `com.apple.locationd.synchronous`.
- Its failed lookups are harmless: `routined.registration` x3 (routined is denied), `mobileassetd.v2`, `fairplayd(.versioned)`, `pairedsyncd.syncstate`,
  the iAP/ExternalAccessory servers, and six `com.apple.private.alloy.*-idswake` activations (IDS, Operation not permitted).
  None of these is on the positioning path.
- MobileGestalt says yes to everything: `gps`, `location-services`, `magnetometer`, `gyroscope` and `accelerometer` are all `true`.
  The gestalt cache is not the problem this time.
- At 15:40:43 the user switched Location Services on. Within the same second wifid logged `new status: 3`,
  `loc_test` went from authorization 2 to 0, and Settings showed the switch on.

locationd has **zero position sources**. There are three possible sources, and each one was dead in v27:

1. **Wi-Fi was switched off for the whole run.**
   - The kernel's apple80211 summary at the end of the run shows `set 19 (POWER) 1 times` and **no `set 10 (SCAN_REQ)` line at all**.
     For comparison, runs 07:58–12:53 each had 18–61 SCAN_REQ.
   - `lx: the interface is removed and the radio stopped` appears at 15:39:49, half a second after wifid started (log line 10927), and the radio is never started again.
   - wifid logs `Error: location is nil` and `WiFi3BarsTileCacheObserver ... no location available`.
   - The switch has been off since the 12:53 run, where POWER was set 3 times and the radio stopped at line 153865. It is a persisted user setting, not a driver failure.
     v26 (14:02) shows the same single POWER-off at start.
   - With no scan there are no BSSIDs, and locationd has nothing to send to `https://gs-loc.apple.com/clls/wloc`. That URL is in locationd's strings, together with `gsp10-ssl.apple.com/...`.
2. **Cell positioning gets no cells.**
   - locationd asks CommCenter `kCellMonitorGetCellInfo` 55 times. Every answer is `[cm.mdl.1] #N No current cells found`.
   - CommCenter does know the serving LTE cell from NAS serving-system indications (`LTE cell changed`, `kCTRegistrationGsmCellId`).
     Its cell monitor, however, asks the modem with Apple's own NAS message. Disassembly of
     `QMICellMonitorCommandDriver::getCellInformationForSlot` (libCommCenterMCommandDrivers.dylib, 0x1f0d30304) shows
     `mov w1, #0x5556` passed to `qmi::MutableMessageBase(...)`: **nas::GetCellInfo = NAS 0x5556**, an Apple vendor message.
   - The X55 runs Qualcomm firmware and refuses it. The bridge log has `UMI BB: rsp svc 0x3 msg 0x5556 ... error 0x47` 33 times,
     where 0x47 is QMI_ERR_INVALID_QMI_CMD. The other Apple NAS messages (0x5557, 0x5562, 0x5566–0x556a) get the same answer.
   - CommCenter never sends standard NAS 0x0043 (Get Cell Location Info). It is not in the v27 request list.
3. **No GNSS.**
   - There is no Apple GNSS chip. `/usr/libexec/gpsd` talks to `/dev/cu.gps` (CommUart), SPI or PCIe (`GnssHal::CommPcie`) on an Apple board.
   - locationd never even looked up `com.apple.gpsd` in v27, although its plist is in `/redwood/LaunchDaemons`.
   - The X55 does publish **QMI LOC: QRTR service 16, instance 0x2, node 3, port 75** (`UMI MODEM: QRTR service=16 ...`), but nothing uses it.

The `loc_test` result in v27 (`error kCLErrorDomain 1` = denied) is a test-tool problem, not a locationd problem.
A bare command-line client can never be authorized, and the binary was unsigned anyway (section 1).

What could not be seen in v27:

- whether locationd would reach Apple's server;
- locationd's own os_log, because it is not in OSLOG;
- kernel `net:` connection lines, because `net_names` is a boot-arg and covers en0 only.

Do **not** add `com.apple.locationd` to OSLOG to find out. OS_ACTIVITY_DT_MODE formats os_log in-process,
and locationd logs `CLClientLocation` values (`%{private, location:CLClientLocation}`) that may then be printed unredacted.
That would put coordinates into the run log. Judge by `loc_test` and the kernel scan counters instead.

## 4. Running loc_test

The boot scenario (`rc.jobs`) runs `loc_test 300 &` automatically. By hand, over SSH:

```
/redwood/bin/loc_test                 # liveness only: "locationServicesEnabled = 0/1"
/redwood/bin/loc_test enable          # switch Location Services on (as Settings does)
/redwood/bin/loc_test 120             # 120 s of updates on behalf of MobileWiFi's bundle
/redwood/bin/loc_test 120 -           # the same, as the bare program (expected: denied)
```

## 5. Expected log lines on the next run

Good:

- `Successfully spawned locationd[...] because ipc (mach)` appears once, with no `locationd ... exited due to` lines.
- `loc_test: locationServicesEnabled = 1`. The user switched it on in v27 and the setting persists on /var.
- `loc_test: asking on behalf of /System/Library/LocationBundles/MobileWiFi.framework (its authorization: 3)`, or 4.
- With Wi-Fi switched ON (it does not need to be joined): the kernel summary has `apple80211: in this run: set 10 (SCAN_REQ) N times`,
  and `loc_test: fix N: horizontal accuracy 30..300 m (Wi-Fi-like)`, then `loc_test: done: N fixes, best ...`.
  Weather and Maps then show a position.

Bad, and what each one means:

- `loc_test: TIMEOUT`: locationd hangs. Boot with `location=0`.
- `authorization: 2` for the bundle: Settings > Privacy > Location Services > System Services has the Wi-Fi item off.
- `error kCLErrorDomain 0 (location unknown)` with SCAN_REQ > 0: scans happen but there is no fix.
  The next suspects are the position server (reachability from China, or `kCLErrorDomain 2` network) and the scan results' fields
  (`hakios.apple80211.c`, SCAN_RESULT: BSSID, RSSI, channel, age_ms).
- `error kCLErrorDomain 1` with `asking on behalf of`: the entitlement was not honoured.
  Check `codesign -d --entitlements :- root/bin/loc_test` in the build output, and look for the build line `no ldid or codesign`.
- `No current cells found` in CommCenter: see **Implementation (cell info)** below; the experimental translator is disabled by default.

## 6. Plan

### 6.1 Wi-Fi positioning (cheapest, probably no code)

1. On the next run, switch Wi-Fi ON in Settings or Control Center. There is no code blocker known on the iOS side:
   locationd links MobileWiFi's `WiFiDeviceClientScanAsync`, wifid already serves scans (18–61 per run before 12:53),
   and the kernel answers SCAN_REQ and SCAN_RESULT.
2. Read `loc_test` and the SCAN_REQ count. If there is no fix while scans happen, the next step is a scan-result field audit in `hakios.apple80211.c`
   (age_ms, RSSI and channel of every result must be sane: locationd drops stale or zero-RSSI entries),
   then a reachability check of gs-loc.apple.com from the phone (`curl -sI https://gs-loc.apple.com/` over SSH).

### 6.2 Cell positioning (km accuracy; optional)

Implemented as an opt-in kernel translator; see **Implementation (cell info)** below for the
verified 20F66 layouts, switches, host tests and required phone acceptance checks.
No cell-positioning fix has yet been verified on the phone.

### 6.3 GNSS from the X55 (QMI LOC, service 16)

Linux reference: `~/X/mainline/umi-linux/work/modem-data/logs/02-qmi-direct.txt` lists service 16 "Location service" on the X55.
GNSS was never exercised on Linux either, so the first fix is unproven. The Mi 10's GNSS RF is assumed to sit behind the X55.

**Stage A, kernel.** Add a LOC client owned by the modem service thread, next to the bridge, not through CommCenter (CommCenter does not use LOC).

- Open QRTR to node 3, port 75.
- Send LOC_REG_EVENTS (0x0021) with the position-report mask, then LOC_START (0x0022): periodic, 1000 ms.
- Take the EVENT_POSITION_REPORT indication (0x0024).
- Verify the message and TLV ids against libqmi `data/qmi-service-loc.json` before coding.
- The engine may be locked by NV. Check LOC's engine-lock get/set messages first.
- Optional: inject XTRA. locationd's own strings name `https://iphone-ld.apple.com/xtra/xtra3gr.bin`, the Qualcomm XTRA format Apple serves for its Qualcomm-GNSS iPhones,
  via LOC inject-predicted-orbits. This turns a 30–60 s cold start outdoors into a few seconds.
- Keep only the latest fix in a root-only character device or sysctl (struct: time, lat, lon, h-acc, alt, v-acc, speed, course, satellites).
- Log counts, accuracy and satellites only.
- Gate it behind the switch file `/var/db/hakios-location/gnss-on`, read when the userland side opens the device. No new boot-arg.

**Stage B, userland: feeding locationd.**

- **B1 (first): a small `umi_gnssd` launchd job.** It reads the device and feeds locationd through CoreLocation's `CLSimulationManager`
  (Mach service `com.apple.locationd.simulation`; entitlement `com.apple.locationd.simulation`; locationd's strings say
  "unentitled client attempting to simulate location").
  - Every app gets the fix with no locationd change.
  - Fixes carry `sourceInformation.isSimulatedBySoftware = 1`, locationd stops harvesting ("Harvesting not allowed: location simulation enabled"),
    and the simulation replaces Wi-Fi fixes while it runs. Some apps reject simulated locations.
  - Run it only while the switch file exists.
- **B2 (proper): a gpsd stand-in that speaks `GpsdProtocol.proto` / `GnssTypes.proto` on `com.apple.gpsd`**, the interface of locationd's CLGnssDaemonDevice.
  The schemas come from `tools/gpsd-proto-dump.py /usr/libexec/gpsd OUTDIR`: Request, Response, Indication, StartContext, Inject* and so on, 38 messages.
  - Fixes are then real GNSS, fused with Wi-Fi, and not flagged as simulated.
  - First find out why locationd never looked up `com.apple.gpsd` in v27. It is probably a platform or GNSS-chip gate inside locationd.

Effort:

- Stage A: 2–3 days, including an outdoor phone test.
- B1: about 1 day.
- First usable GNSS fix: about 4 days.
- B2: another 1–2 weeks.

## 7. Supervisor checklist for the next run

1. The payload is built from the current overlays. Check that `codesign -d --entitlements :- .../userland/root/bin/loc_test` lists
   `com.apple.locationd.effective_bundle`, and grep the build log for `no ldid or codesign` (it must be absent).
2. No loader rebuild is needed for these changes. Use `out/xiaomi-mi-10/flash/loader.efi` (15:37 or newer) if you want `location=0` available.
3. On the phone: **switch Wi-Fi ON**. It has been off since the 12:53 run. Keep Location Services on.
4. Grep: `loc_test:`, `apple80211: in this run: set 10`, `locationd.*exited`, `No current cells found` (expected), then open Weather and Maps.

## Implementation (GNSS)

2026-11-11: stages A and B1 implemented and host-checked; **not yet tested on the phone**.
No phone tools were used. No new boot-args, CommCenter modifications, loader/reboot changes,
modem NV writes, or new EFS access. No full userland/payload build was run.

### Files and artifacts

Paths below are relative to `devices/xiaomi-mi-10/`:

| File | Purpose |
|---|---|
| `drivers/umi/hakios.umi_baseband.cpp` | Three small integration points: include LOC client, consume its dedicated RX port, send its command after ABM's batch. |
| `overlays/xnu/files/bsd/dev/umi_gnss.inc` | Asynchronous LOC session, root-only sysctls, heartbeat lease, coordinate-free diagnostics. |
| `overlays/xnu/files/bsd/dev/umi_gnss_qmi.h` | Actual LOC request builders and indication parser; integer operations only, including IEEE validation. |
| `overlays/xnu/files/bsd/dev/umi_gnss_abi.h` | Shared 64-byte latest-fix ABI, version 1. Coordinates and uncertainty cross as IEEE bit patterns, never printed. |
| `overlays/userland/src/umi_gnssd.m` | Switch polling, lease renewal, latest fresh fix -> CLSimulationManager. |
| `overlays/userland/src/umi_gnssd.entitlements` | `com.apple.locationd.simulation = true`. |
| `overlays/userland/com.hakios.umi-gnssd.plist` | Root launchd job; console diagnostics; framework activity logging disabled. |
| `overlays/userland/build.sh` | Compile/sign daemon and copy its binary/job into the next payload. Signing failure is fatal for this daemon. |
| `tools/test-gnss.py`, `tools/gnss-tests/loc.cpp` | ASan/UBSan tests of the real parser and real kernel session code, with host kernel-facility shims. |
| `LOCATION-umi.md` | This implementation record and supervisor instructions. |

Built kernel: `work/umi-v23/out/xiaomi-mi-10/flash/kernel-gnss` (18,068,408 bytes).
SHA-256: `84407db92f2a245a0f8f0a879bf069b9156e812cb09f167d90cfce3ef93fd20e`.
Other `kernel-*` copies were not overwritten.

Signed arm64 daemon, job and entitlement are staged in `out/xiaomi-mi-10/gnss/`.
The daemon SHA-256 is `c521232c27eec4ec639a1ec653653b8fa93e0e6ae50928109a4d50330850be9d`.
The existing APFS payloads have not been rebuilt or changed: the supervisor must include
`bin/umi_gnssd` and `LaunchDaemons/com.hakios.umi-gnssd.plist` in the next payload.
The normal userland build now does this automatically.

### Verified wire definitions and iOS API

There was no libqmi source checkout in the searched local trees; DNS prevented a small source fetch.
Definitions were instead checked in the local Debian package:
`mnt/mainline-src/build/boot1/debian-rootfs/var/cache/apt/archives/libqmi-glib5_1.36.0-1_arm64.deb`,
extracted on the host under `/private/tmp/umi-libqmi/`. Its `libqmi-glib.so.5.11.0` has exported
LOC getters, builders, parsers and named enum tables. This is libqmi's actual implementation,
not Apple or guessed QMI layouts.

| LOC operation/field | Verified definition |
|---|---|
| Register Events | Message `0x21`, TLV `0x01`: uint64 LE mask; position bit 0 and SV bit 1 (`3`). Register mask `0` on stop. |
| Start | Message `0x22`; TLV `0x01`: uint8 session; `0x10`: uint32 recurrence `1` (periodic); `0x12`: uint32 intermediate-report state `2` (disable); `0x13`: uint32 minimum interval `1000` ms. **0x12 is not an accuracy setting.** |
| Stop | Message `0x23`, TLV `0x01`: uint8 session. |
| Position indication | Message `0x24`; `0x01`: uint32 status (`0` success, `1` searching); `0x02`: uint8 session. |
| Position fields | `0x10`/`0x11`: double latitude/longitude; `0x12`: float circular uncertainty, otherwise `0x14`: float ellipse semi-major uncertainty, metres. |
| Optional fields | `0x1c`: float altitude above sea level; `0x1d`: float vertical uncertainty; `0x24`: uint64 UTC milliseconds; `0x2c`: uint8 count followed by uint16 satellite IDs (only count retained). |
| SV indication | Message `0x25`; mandatory `0x01`: uint8 altitude-assumed; `0x10`: uint8 count followed by 28-byte satellite records (only count retained). |

Reference functions: `qmi_client_loc_register_events` (`0x169c60`), `qmi_client_loc_start`
(`0x169eb0`), `qmi_client_loc_stop` (`0x16a290`),
`qmi_indication_loc_position_report_indication_parse` (`0x15cf80`),
`qmi_indication_loc_gnss_sv_info_indication_parse` (`0x15ee20`), and their named getter/enum tables.
Addresses are ELF VMAs in that exact local library.

CoreLocation was checked in the local 20F66 extraction
`tools/tmp/dsc-20F66/System/Library/Frameworks/CoreLocation.framework/CoreLocation`.
The `CLSimulationManager` method list confirms `-init`, `-appendSimulatedLocation:` (void/object),
`-clearSimulatedLocations`, `-startLocationSimulation`, `-stopLocationSimulation`, and
`-setLocationRepeatBehavior:` (void/unsigned char). `-flush` is obsolete and does nothing in 16.5;
the helper does not use it. These SPI calls are asynchronous and provide no success acknowledgement.
`locationd`'s incoming-connection handler checks the Boolean entitlement
`com.apple.locationd.simulation` (20F66 `0x10084e204` onward).
Its append handler (`0x10084d308`) and queue code accept new locations during an active simulation;
repeat behavior `0` does not repeat the last fix. No direct locationd patch or fabricated fix is used.

### Isolation, privacy and lifetime

LOC uses its own QRTR client identity, local port `0x4200`. ABM's pooled ports are `0x4100..0x415f`;
boot/WDA/WDS/TFTP use other ports. QRTR allocates the LOC client on first request, as it does for
ABM's clients; there is no QMI CTL allocation on this transport. The endpoint comes from the live
service-16 announcement, not a hardcoded remote port. Replies must match its node and port.
Only the modem service thread sends LOC traffic, outside `bb_lock`, after the ABM batch.
LOC never enters ABM's pending table, TX/frame queues, client slots, pooling/reset code or WDS handling.
It sends no global modem reset, operation-mode, engine-lock or assistance-setting commands.

`kern.hakios.gnss_enable` accepts root writes of `1` (renew a 15-second lease) or `0` (stop).
`umi_gnssd` checks the switch every second and renews every five seconds. Switch removal clears
the kernel's latest fix immediately when observed, then sends Stop and unregisters events.
Loss of the LOC endpoint also clears the fix; request timeouts are 8 seconds and latch an error
until the switch is toggled. A dead helper loses its kernel lease after 15 seconds.

`kern.hakios.gnss_fix` returns only the newest report, including receipt age, sequence, optional-field
flags and satellite counts. **Both reads and writes of the control and reads of the fix require root.**
Never dump this sysctl, `sysctl -a`, raw QMI TLVs, or a CLLocation object into a log: they contain
private location. The parser rejects malformed/duplicate/truncated TLVs, non-success or wrong-session
reports, invalid coordinate ranges/IEEE values, and fixes without usable horizontal uncertainty.
Optional altitude is used only with valid vertical uncertainty. Satellite view is the number of
entries in the modem's SV list, not a quality-filtered visibility estimate; flags distinguish unknown
counts from zero. Speed/course are not exported in ABI v1.

The helper submits each new fix at most once, rejects receipt age over 10 seconds (and UTC age
outside +/-10 seconds when UTC is present), and stops simulation when fixes become stale.
It requests no-repeat behavior. A root-owned, mode-0600 `simulation-active` marker in the same
directory lets a restarted helper clean up its own previous simulation, including when the switch
was removed during a crash. An ordinary boot without either file makes no LOC requests or
simulation connection. The new launchd job stays idle. Stop calls are asynchronous too: supervisor
verification of resumed Wi-Fi fixes is required, especially after crashes or locationd restarts.

### Supervisor phone test

Use the new kernel with the current working cellular setup (`umi_modem=2`), a payload containing
the signed daemon/job, Location Services on, and no `location=0`. No new boot-arg is needed.
Only the supervisor runs these commands or boots/tests the phone.

1. First boot with **no `gnss-on` file**. Expect `gnss: helper ready; idle ...`, no LOC session
   start, and the same cellular registration/data behavior as the baseline. Check browsing over
   cellular and pdp_ip0 TX/RX before enabling GNSS.
2. As root, create the switch directory/file (ordinary shell syntax, via the supervisor's existing
   test mechanism; the agent has not executed these on a phone):

   ```sh
   mkdir -p /var/db/hakios-location
   chmod 700 /var/db/hakios-location
   touch /var/db/hakios-location/gnss-on
   chmod 600 /var/db/hakios-location/gnss-on
   ```

   The directory/file must belong to root. Symlinks and group/world-writable switch files are ignored.
   **Give GNSS a sky view: beside a window or outdoors. An unassisted cold start can take minutes;
   allow 10–15 minutes before concluding that the engine/RF path is unavailable.**
3. Good console lines, all coordinate-free:
   - `gnss: enabled by helper; waiting for LOC session`
   - `gnss: session start requested; LOC on independent QRTR port 0x4200`
   - `gnss: response msg 0x21 result 0`, then `0x22 result 0`
   - `gnss: session started; periodic 1000 ms ...`
   - `gnss: satellites in view N; fixes M` (can precede a fix)
   - `gnss: first fix after N ms`, then fix count/accuracy/view/used/age lines
   - `gnss: simulation start submitted ...`, `gnss: submitted fix count ...`
4. Run the already signed `/redwood/bin/loc_test 300` and check actual app delivery:
   `loc_test: fix ... simulated 1`. Open Maps/Weather without logging their location. The helper's
   "submitted" message alone does not prove that locationd accepted the entitlement or delivered a fix.
   Continue checking cellular traffic during acquisition and after fixes begin; no new ABM ASSERT,
   CommCenter respawn, WDS failure, client exhaustion or bridge-timeout growth is acceptable.
5. Remove `/var/db/hakios-location/gnss-on`. Expect helper `switch off`, simulation stop submitted,
   kernel `0x23 result 0`, unregister `0x21 result 0`, then `session stopped`. Confirm simulated fixes
   stop and, with Wi-Fi enabled, normal location sources can resume. Also test off/on reacquisition.
   A graceful helper termination stops both paths; a forced helper exit should expire the kernel
   lease and the launchd replacement should clean up the marker/session.

Bad markers:

- `kernel control errno 2/6` (ENOENT/ENXIO): wrong kernel or bridge not published; no GNSS traffic is sent.
- Enabled/waiting without `session start requested`: LOC service is not announced yet; check modem ready/service 16.
- Nonzero `response ... result`: negative is malformed response; positive is **QMI error + 1**.
  A `timeout msg ...` means LOC did not answer. Toggle the switch to retry; do not restart CommCenter.
- `position status` other than `0/1`: the modem reports a failure/lock/offline condition. Do not change NV to work around it.
- Satellites stay zero/no fix with QMI success: sky view, antenna/RF path or disabled/locked GNSS engine remain suspects.
- Kernel fixes but no helper submission: check receipt/UTC freshness and phone clock, ABI read errors, signing/API/marker errors.
- Helper submissions but no `loc_test simulated 1`: simulation entitlement/connection or client authorization needs phone diagnosis.
- `helper IPC timeout`, or no resumption after switch removal: simulation lifecycle needs inspection; the marker is retained
  across a helper crash for cleanup. Do not enable locationd's OSLOG/OS_ACTIVITY_DT_MODE: it can expose coordinates.

### Host verification and known gaps

Passed `python3 devices/xiaomi-mi-10/tools/test-gnss.py`: actual parser + session implementation under
ASan/UBSan, 10,000 malformed inputs, root access checks, idle default, endpoint/session matching,
lease expiry, stop/unregister, cancellation, service loss and timeout retry behavior. Existing
`tools/modem-tests/bb.c` also passes under ASan/UBSan (QMI pooling/routing/pending, WDS and activation).
`umi_gnssd.m` compiles with `xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" -O1
-Wall -Wextra -Werror -fobjc-arc -framework Foundation -framework CoreLocation`, with
`-Idevices/xiaomi-mi-10/overlays/xnu/files/bsd/dev`. `/usr/bin/codesign --verify` passes and the embedded
entitlement was inspected. The launchd plist and build shell syntax validate. No dynamic-lookup imports.

Kernel built only through `/Users/xm666/X/umi-ios/build-lock.sh`, running
`bash devices/xiaomi-mi-10/build-kernel.sh BUILD_DSYM=0` from `work/umi-v23`.
The default build compiled/linked but its dSYM wrapper hit sandbox `/dev/fd` denial;
`BUILD_DSYM=0` skips only that debug-symbol step. The final kernel copy was made while holding the same build lock.

Open gaps: real X55 GNSS/RF support and first fix are unproven (also untested on the Linux reference).
The engine could be disabled/locked by modem NV; no lock/NV changes or probing writes were attempted.
No operation-mode override, SUPL configuration, time injection or XTRA predicted-orbit injection is implemented.
Apple's `xtra3gr.bin` assistance path remains optional follow-up after acquisition works; there is no
network download in the helper. B2/gpsd integration and fusion remain future work. B1 fixes are
**simulated by software**, override Wi-Fi positioning while active, suppress locationd harvesting,
and may be rejected by apps. Acceptance, continuous delivery, sourceInformation, cleanup/reconnection,
and cellular coexistence require the supervisor's phone run.

## Implementation (cell info)

2026-11-11: experimental LTE NAS `0x0043` → Apple NAS `0x5556` bridge, **default off**.
No phone access, CommCenter changes, modem NV writes, new boot arguments or userland payload build.
The code is ready for a supervisor-controlled coexistence/positioning run; host tests do not prove
X55 firmware support, remaining NAS client capacity or locationd/server acceptance.

Files (relative to `devices/xiaomi-mi-10`):

- `overlays/xnu/files/bsd/dev/umi_cellinfo_qmi.h`: portable strict QMI parser and packed Apple TLV encoder.
- `overlays/xnu/files/bsd/dev/umi_cellinfo.inc`: bounded request coalescing, client ownership, subscription,
  switch, timeout and independent QRTR lifecycle; included by `drivers/umi/hakios.umi_baseband.cpp`.
- `tools/cellinfo-tests/cellinfo.cpp`, `tools/test-cellinfo.py`: execute the actual translator and session code
  using synthetic packets and kernel facility shims, under ASan/UBSan.

### Binary evidence and Apple layout

Inspected the local iOS 16.5 / 20F66 dyld cache and extracted binaries with `ipsw dyld disass/symaddr`
and LLVM disassembly. Reference images: `libCommCenterMCommandDrivers.dylib`, `libCommCenterBase.dylib`,
`libQMIParserDynamic.dylib`, CommCenter and locationd. Named NAS field parsers in MCommandDrivers give
the packed layout; the consumers give the field semantics. Local disassembly remains under
`tools/tmp/cellinfo-*` outside the port tree; no proprietary binary/disassembly is added to source.

- `QMICellMonitorCommandDriver::getCellInformationForSlot` at `0x1f0d30304` sends NAS `0x5556`
  with an empty request TLV list. Callback `0x1f0d308dc` and the unsolicited handler both process
  `nas::GetCellInfo::RespInd`. QMI uses request/response/indication types `0/2/4`, little-endian
  `u16 transaction, u16 message, u16 TLV bytes`. Replies preserve the original Apple transaction/client/channel.
  Result TLV `0x02` is the usual two `u16` values (result, error); indications have no result TLV.
- The callback selects exactly one LTE identity version from personality flags, rather than searching
  older versions. We therefore emit parallel versions of the same observation (no duplicate cells in
  the chosen version). Missing optional TLVs are permitted. RAT is implicit in the LTE TLV type;
  there is no invented RAT byte in each record.
- `nas::tlv::parseField(LteCell{,_V2,_V3,_V4})` at `0x1f0d4fa78`, `0x1f0d4fba8`,
  `0x1f0d4fcd8`, `0x1f0d4fe20` advances by 26/28/31/50 bytes respectively. Each identity TLV
  starts with **u8 record count**, followed by packed records. Multi-byte fields below are little endian.

| Field (offset from record start) | `0xbb` V1 | `0xbc` V2 | `0xd2` V3 / `0xd3` V4 |
| --- | --- | --- | --- |
| Index | u16 @0 | u16 @0 | u16 @0 |
| MCC, MNC (decimal integers, not BCD) | u16 @2, u16 @4 | same | same |
| Band enum | u8 @6 | same | same |
| TAC | u16 @7 | u16 @7 | u32 @7 |
| E-UTRAN cell identity | u32 @9 | u32 @9 | u32 @11 |
| EARFCN | u16 @13 | u32 @13 | u32 @15 |
| PCI / Apple PID | u16 @15 | u16 @17 | u16 @19 |
| Sector latitude, longitude (raw signed integers) | i32 @17, i32 @21 | i32 @19, i32 @23 | i32 @21, i32 @25 |
| Bandwidth | u8 @25 | u8 @27 | u8 @29 |
| Deployment type | absent | absent | u8 @30 |

V4 additionally has `u32 @31, u8 @35, u32 @36, i16 @40, u32 @42, u32 @46`.
The first four map to throughput, CSG indication, CSG ID and PMax in the consumer at `0x1f0e20b44`;
the final two are consumed by the wire parser but not that LTE model callback. These are unavailable
from the selected standard NAS observation and are encoded all-ones. Band `0xff` becomes model band
zero via Apple's `+1` conversion. Unavailable bandwidth/deployment/neighbor-extension fields also use
all-ones. Real firmware acceptance of these unavailable enums still needs the phone run.

**No fabricated tower coordinates:** both packed coordinate fields are zero. This is verified as the
absent-coordinate combination, not a reported geographical position: locationd's
`CLTelephonyService::lteCellFromCellMonitorDict` (`0x10022b02c`) reads the optional sector keys at
`0x10022b30c`, ORs the two raw integers at `0x10022b350`, and skips the coordinate conversion when
both are zero (`0x10022b358` → `0x10022b378`). Otherwise its scale is 1/14400 degrees.
CommCenter's LTE exporter is at `0x1006cf320`. MCC/MNC extraction, identity/frequency/PCI and optional
metadata are independent of that coordinate branch. No precise coordinates are produced or logged.

Neighbor TLVs also start with u8 count. `0xbf` records are 8 bytes:
`u16 EARFCN @0, u16 PCI @2, i16 RSRP @4, i16 RSRQ @6`.
`0xd4` records are 27 bytes:
`u32 EARFCN @0, u16 PCI @4, i16 RSRP @6, i16 RSRQ @8, u32 bandwidth @10,
u8 neighborType @14, u32 throughput @15, u32 @19, u32 @23`.
The old/new parsers are `0x1f0d4f190` / `0x1f0d4f1fc`; signal fields use
`SigInfoBase<short,1,1>`. Standard tenths of dBm/dB are divided by ten (toward zero) for these
integer Apple fields. No neighbor PLMN/TAC/ECGI is available in this standard measurement list,
and none is invented. The serving PCI is excluded from the neighbor vector. Serving RSRP/RSRQ
continues to come from CommCenter's existing signal model (LTE exporter calls `0x1006cf7f0`);
we do not change that model or its forwarded NAS signal requests.

### Standard NAS input and transport isolation

A single read-only NAS Get Cell Location Info `0x0043` is sufficient for the LTE observation.
The standard wire read order was cross-checked against local libqmi 1.36.0's
`qmi_message_nas_get_cell_location_info_response_parse` (`0x1f4bc0`, LTE branch `0x1f54a0`).
TLV `0x13` has a 19-byte prefix:
`u8 idle @0, PLMN BCD[3] @1, u16 TAC @4, u32 ECI @6, u16 EARFCN @10,
u16 serving PCI @12, four reselection bytes @14..17, u8 count @18`.
Each 10-byte measurement is `u16 PCI, i16 RSRQ, i16 RSRP, i16 RSSI, i16 SRXLEV`.
TLV `0x1e` is the extended LTE EARFCN, u32. `0x2e` is an NR frequency extension,
**not** a second intra-LTE layout. Invalid PLMN digits/identities and unavailable measurements are
not turned into valid cells. LTE ECI is limited to 28 bits, PCI to 0..503; measurements are bounded
and capped at 32 actual neighbors. A wide EARFCN produces an empty V1 identity/old-neighbor list,
with the full value in newer formats. There is no truncation to u16.

The input envelope rejects duplicate/truncated TLVs, inconsistent arrays and trailing bytes.
A successful standard reply without a usable LTE identity produces explicit empty Apple lists,
not stale cells. Non-LTE and inter-frequency/NR lists are not translated in this implementation.
Modem errors propagate to waiting Apple requests; malformed replies/timeouts produce QMI internal
error. There is no request cascade through 0x0024/0x004d/0x004f and no cached old location fallback.

Only enabled NAS `0x5556` requests are intercepted. Everything else follows the existing bridge.
`0x5557` is `sendRegistrationHintsForSlot`, **not** cell-change registration. Other sibling call sites
resolve to preferred-PLMN CRC/list (`0x5567/0x5566`), cellular-data status, offload/Wi-Fi information,
smart-data/data-SIM state and standalone-5G state (`0x5562`, `0x5568..0x556a`). Those paths are not
needed to parse GetCellInfo and retain their current forwarding/error behavior.

The bridge owns QRTR local port **0x4201**, separate from ABM's pooled clients/transaction table
(0x4100..0x415f) and GNSS (0x4200). At most one standard query is outstanding; up to 32 Apple requests
coalesce against it. A reply must match NAS endpoint, message, response type and independent transaction.
Timeout is 8 seconds; stale/foreign replies cannot complete another request. CTL release/sync,
baseband host reset and pooled-slot reuse prune local waiters/subscriptions immediately. NAS service
loss cancels local requests; disabling releases only the dedicated client. Original ABM NAS requests,
registration, WDS/data/SMS forwarding and GNSS are unchanged by the hook.

Unsolicited updates: startup at `0x1f0e29938` sends standard NAS IndicationRegister `0x0003` with
Apple TLV **0xa0 CellInfoChange = 1**. We observe the request without modifying it and commit the
subscription only on its matching successful original response. Successful unsubscribe (value zero)
stops synthetic notifications. While a live NAS client is subscribed, a read-only query runs every
5 seconds after completion and successful observations are delivered as NAS `0x5556` indications
using exactly the same TLVs. This is periodic sampling, not a modem-native change event. No new
modem event registration/mode/configuration command is sent.

### Switches, tests and build

**Default off** until on-phone coexistence is proven. The supervisor may create root-owned switches:

```sh
mkdir -p /var/db/hakios-location
chmod 700 /var/db/hakios-location
touch /var/db/hakios-location/cellinfo-on
chmod 600 /var/db/hakios-location/cellinfo-on
```

`/var/db/hakios-location/cellinfo-off` overrides `cellinfo-on` and disables interception/queries;
removing `cellinfo-on` also disables it. A separate thread-call worker checks presence about every
2 seconds, outside `bb_lock`; **the modem service thread does no filesystem I/O**. It notices
subscriptions even while disabled, so toggling files requires no CommCenter restart. Disabling answers
already intercepted requests with an error and drops late dedicated replies; subsequent requests take
the unchanged original forwarding path. Missing `/var` and failure to allocate the optional worker
leave the feature disabled. No new boot argument or helper/payload is required.

Host verification (all passed):

```sh
python3 devices/xiaomi-mi-10/tools/test-cellinfo.py
python3 devices/xiaomi-mi-10/tools/test-modem.py
python3 devices/xiaomi-mi-10/tools/test-gnss.py
```

Cell-info tests exercise actual code under ASan/UBSan with independent Apple wire assertions for all
four identity versions and both neighbor versions, two-/three-digit PLMN, extended EARFCN, absent
coordinates, unknown/empty/error results, bounded lists, duplicate/truncated TLVs and 20,000 malformed
inputs. Session tests cover original registration bytes, success-gated subscription, indications,
coalescing across both QMUX channels, exact response transactions, endpoint matching, timeout,
late reply, queue capacity, disabling/releasing, pooled-client reuse, host reset and unrelated-message
pass-through. Existing modem regression covers PCIe/MHI/QRTR, QMUX/CTL/client pooling, activation,
WDS/QMAP/data, loader gates and firmware staging; GNSS parser/session tests also pass.

Kernel build is only through the shared lock, with `BUILD_DSYM=0` as in the GNSS build:
`/Users/xm666/X/umi-ios/build-lock.sh bash devices/xiaomi-mi-10/build-kernel.sh BUILD_DSYM=0`.
The build-and-copy wrapper holds that same lock until copying the newly built
`mach.development.bcm2837` to `out/xiaomi-mi-10/flash/kernel-cellinfo` and hashing it.
Other `kernel-*` copies are not overwritten.

Build status at handoff: the final cell-info/baseband translation unit compiles successfully in the
required locked kernel build. The full build is blocked before linking by concurrent hotspot changes:
`overlays/xnu/files/iokit/Kernel/hakios.wlan.cpp:195` passes a const byte pointer to `setProperty`
(which requires mutable `void *` for this overload), and
`overlays/xnu/files/bsd/dev/hakios.net.c:555` designates nonexistent `ifnet_init_eparams.detached`
(the current XNU field is `detach`). These files were not modified by this task, per the ownership rule.
No fresh `kernel-cellinfo` was copied and there is **no kernel SHA256 to report yet**; an old kernel is
not substituted. The exact failure log is `/Users/xm666/X/umi-ios/tools/tmp/cellinfo-kernel-build.log`.
Once the hotspot agent fixes these build errors, rerun the locked build-and-copy command before any
phone acceptance run. All completed host tests above remain valid.

### Supervisor phone acceptance (not executed by this agent)

1. Boot this kernel with the existing CommCenter bridge configuration. Leave GNSS simulation off.
   First verify the default-off baseline: registration, cellular Internet and SMS must work as before.
   Do not enable locationd's OSLOG/OS_ACTIVITY_DT_MODE: it can print private locations/identifiers.
2. Enable the cellinfo-on file, ensure cellinfo-off is absent. Check only header/count lines:
   `cellinfo: enabled (experimental; file switch, default off)`, then
   `cellinfo: translated NAS 0x43 -> 0x5556: LTE cells 1 measurements N indications M`.
   Successful on-demand QMI responses must retain the original 0x5556 transaction/client;
   standard 0x43 should no longer be rejected. No payload or raw cell-ID logging is needed.
3. Trigger the existing authorized `loc_test`/Maps/Weather location request with cellular data on.
   Test with Wi-Fi positioning and GNSS simulation off to isolate cell positioning. While LTE is
   registered, CommCenter's **"No current cells found" should disappear** and locationd should receive
   current LTE cells. Verify a real cell-positioning fix at the app/loc_test level; seeing the bridge's
   translated-count line alone does not prove a server lookup or successful position.
4. Keep cellular traffic running and exercise SMS while requesting positions/receiving updates.
   No CommCenter restart/ABM ASSERT, registration loss, NAS client exhaustion, WDS regression or
   timeout growth is acceptable. Check stationary and changed-cell conditions, subscription updates,
   and a disable/re-enable cycle without restarting CommCenter. Do not log actual identities/coordinates.
5. Create cellinfo-off (or remove cellinfo-on): expect `cellinfo: disabled ...`; dedicated queries and
   indications stop. Verify original cellular traffic/SMS remain usable. Leave disabled on any regression.

Bad lines and interpretation:

- `cellinfo: query failed error 71 ...`: X55 rejected standard 0x43 with QMI error 0x47; no translation
  fallback is attempted. Other positive values are actual QMI errors; negative means malformed result.
- `cellinfo: NAS 0x43 timeout ...`: no matching dedicated response in 8 seconds; Apple requests get errors.
- `LTE cells 0`: the standard snapshot has no usable LTE identity (including non-LTE registration).
  Empty Apple lists are intentional; "No current cells found" can legitimately remain in this case.
- Continued `rsp svc 0x3 msg 0x5556 ... error 0x47` after enabling: interception is not active;
  check switch files and kernel selection, allowing the 2-second switch interval.
- `QMI parsing error`, `ABM ASSERT`, CommCenter respawn or locationd rejection: capture only sanitized
  header/count evidence, disable this feature, and investigate the selected format/unavailable metadata.

Remaining limits: LTE serving identity and same-frequency neighbors only; no neighbor ECGIs or fabricated
PLMNs, no NR/UMTS/GSM translation, no inter-frequency list and no serving-signal override. Two-/three-digit
MNC numeric values are preserved, but Apple's MCC-aware MNC constructor chooses display width; the
vendor format has no explicit MNC-digit-count member. Additional dedicated NAS-client capacity, X55
reply details, unavailable enum interpretation and five-second indication behavior require real-phone
verification before changing the default to on.
