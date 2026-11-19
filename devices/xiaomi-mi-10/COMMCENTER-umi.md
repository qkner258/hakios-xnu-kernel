# Native CommCenter on the Mi 10's SDX55 — research and design (2026-11-11)

Goal ("route two"): iOS 16.5's own CommCenter drives the external SDX55, so the status bar shows signal bars and the carrier,
and Settings → Cellular (including the Cellular Data switch) works natively. Calls and SMS come later.

Tags: **[E]** evidence from this tree, the 20F66 IPSW or our logs. **[R]** taken from the reference implementation (below,
iPadOS 17 21H461), not yet re-verified on 16.5. **[S]** speculation, to be confirmed by the first trace.

This replaces the 2026-11-10 Codex draft. Its still-valid facts are folded into §0 and §5.

## Verdict

Feasible, and largely already solved by someone else. **UniXNU/QcomBaseband** (HoshinoYumeka / chiba233, BSD-2, extracted
from Trogdor-ipadOS) is a kernel bridge that does exactly this:

- it publishes Apple's PCIe baseband IOKit classes (`AppleBaseband`, `AppleBasebandPCIControl`, one `AppleBasebandPCIInterface`
  per channel, `AppleIPAppender`, `AppleBasebandPCIMAVPDPManager`);
- it carries CommCenter's QMUX frames on channels `qmux1`/`qmux2` to the modem's QMI services over QRTR;
- it answers the Apple-private QMI that a non-Apple modem doesn't have;
- it emulates the BHI/Sahara firmware handshake for BBUpdater and drops the images.

On a Chromebook (SC7180, iPadOS 17.7.11) the reference reports carrier, signal bars, SIM identity, LTE and IPv4 browsing
through native CommCenter. Its catalog entry still lists `verified_devices = []`.

- Docs (local): `ios-port-research/repos/UniXNU_UniXNU/docs/porting/baseband.md`, `docs/drivers/qcom-baseband.md`, `components.toml:555`.
- Source (cloned by the interrupted Codex run): `tools/tmp/commcenter-umi-research/QcomBaseband` @ `dd0199cf`.
  The key files are `baseband/coachz_baseband.cpp` (1263 lines) and `coachz_bb_{qmux,control,sahara,channels,data,pdp}.h/.cpp`.
- No other repo under `ios-port-research/repos` touches this. The oosexgang and JopangJovan trees only *exclude* CommCenter,
  and UniXNU-umi passes `sheng-disable-jobs=…com.apple.CommCenter`.

The Mi 10 is an easier fit than Coachz for three reasons:

1. iPhone10,3's baseband *is* a PCIe Qualcomm "Mav" (Mav17), and the SDX55 also sits on PCIe/MHI. CommCenter's data-port
   endpoint `{type 3 = PCIe, iface 4}` is exactly what our driver already uses on IP_HW0 (`umi_modem_qmi.h:70`), so the
   reference's IPA/DPM endpoint rewrite is not needed.
2. Our kernel already has MHI, QRTR (HELLO/NS), TFTP, EFS and QMAP over IP_HW0 working
   (`logs/test-1111-100715-umi-v21.union.log`: 59 QRTR services, NAS registered on LTE, WDS up on mux 1).
3. The 16.5 user-space ABI matches the reference in every point checked (§1).

The main unknowns are listed in §6. The biggest is CommCenter's postponement-ticket gate on the Internet data context: the
reference needed a one-branch CommCenter patch for it on 17.7.11, and that patch needs the user's approval. The others are the
Mav17 BBUpdater flow and the WDA data format ABM asks for.

---

## 0. Today (no baseband stack)

- [E] CommCenter is not shipped. `overlays/userland/mkjobs.py:216` has `DENY = ('CommCenter', …)`, and the payload binds its
  own `/LaunchDaemons` over Apple's (`userland/src/init.c:196`).
  - `com.apple.CommCenter.plist` (rootfs) has `_LimitLoadToDeviceTree = baseband`, runs as `_wireless` with `KeepAlive=true`,
    and hosts `com.apple.basebandd.xpc`: on 16.5, ABM (basebandd) runs **inside** CommCenter.
  - The `-noncellular` variant is picked when there is no `/baseband` node.
- [E] With no service, lookups spin. In v21 (`02:09:15` to `02:14:29`), SpringBoard, lockdownd, mobile_diagnost, Preferences
  and promotedcontent all fail `com.apple.commcenter{,.coretelephony}.xpc`, and `mg_test: HasBaseband = false`.
  vmd is denied for this reason (`mkjobs.py:225`).
- [E] Earlier attempt (`mkjobs.py:69-90`): with the noncellular job, CommCenter crash-looped. There were 11 SIGSEGV after
  `Could not find AppleIPAppender service` and 9 SIGABRT after `Unable to communicate with AppleBaseband driver`.
  Those are exactly the services the bridge publishes.
- [E] Our kernel already handles the modem itself:
  - Sahara boot, QRTR HELLO/NS, servreg locator, read-only TFTP and RAM-only EFS sync;
  - its own UIM/DMS/NAS clients, and its own WDA/WDS dial on `cmnet` to pdp_ip0 (`umi_modem_mhi.inc:1000-1060`, `umi_modem_data.inc`).
  The data session has `default_route=manual` and no configd service, so iOS apps don't use it today.

## 1. The iPhone10,3 path between CommCenter/ABM/BBUpdater and the baseband

### 1.1 Hardware identity (D22AP device tree, decompressed from `sysimage/DeviceTree.d22ap.im4p`)

| Node / property | Value | Evidence |
| --- | --- | --- |
| `/baseband` | `compatible = baseband,n41`, `device_type = baseband`; `function-bb_on`, `bb_rst`, `pmu_exton`, `pcie_port_control`, `coredump`, `bb_ap_time_sync`, …; `device-imei` (zeroed in the IPSW) | [E] |
| `/product/baseband-chipset` | `mav17` | [E] |
| `/arm-io/apcie/pci-bridge2/baseband-pcie` | `device_type = pcie-device` | [E] |
| `/filesystems/fstab/baseband-vol` | `Baseband-Data` → `/private/var/wireless/baseband_data` | [E] |
| IPSW `Firmware/Mav17-8.50.04.Release.bbfw` | zip of `sbl1, restoresbl1, rpm, tz, mba, apps, acdb, qdsp6sw (59 MB), bbcfg.mbn` and Info.plist `ChipId = 0x0003B0E1` | [E] |

`launchd` picks the cellular CommCenter job when `/baseband` exists. MobileGestalt's `HasBaseband` is derived from the same
node [S]. Our loader builds the DT itself (`loader/loader.c:3579`), so it has no `/baseband` node today.

### 1.2 Kernel classes on the real phone (kernelcache.release.iphone10b, strings)

[E] Bundles:

- `com.apple.driver.AppleBasebandPCI`
- `AppleBasebandPCIMAVControl`, `AppleBasebandPCIMAVPDP`
- `AppleBasebandD20` (the D20/D21/D22 board driver)
- `AppleIPAppender`
- ICE variants for the Intel iPhone10,6

[E] Classes:

- `AppleBaseband`, `AppleBasebandUserClient`
- `AppleBasebandPCIControl`, `AppleBasebandPCIControlUserClient`
- `AppleBasebandPCIInterface`, `AppleBasebandPCIUserClient`
- `AppleBasebandPCIMAVPDPManager` (base `AppleBasebandPCIPDPManagerBase`)
- `AppleIPAppender`, `AppleIPAppenderUserClient`
- the data plane is Skywalk: `AppleBasebandPCIPDPSkywalkInterface`, `…PDPQMAPSkywalk`, `…QMAPControl`, `…QCTunSkywalkInterface`

The user-client class names are the ones CommCenter's sandbox profile allows, so a bridge must use these exact names [R].

### 1.3 User-space stack (dyld cache 20F66, `tools/tmp/dsc-20F66`)

```
SpringBoard/Preferences ──XPC com.apple.commcenter{,.coretelephony}.xpc──▶ CommCenter (+ ABM "basebandd" in-process)
  CommCenter QMI command drivers (libCommCenterMCommandDrivers: QMIMav13ActivationCommandDriver,
     QMIBasebandSettingsDriver, QMIDataContextIPAggregator, …) ── qmi::ClientRouter ──┐
  ABM (libBasebandManager: QMITransportService, data::TransportService)               │
  BBUpdater (libBBUpdaterDynamic, BBUEUR* loaders/programmers, in-process)            │
        │ libTelephonyBasebandDynamic ─ IOServiceMatching("AppleBaseband") ─ IOConnectCall* (board control)
        │ libPCITransport / libIOABP ─ IOServiceMatching("AppleBasebandPCIInterface") + ABPInterfaceProtocol=<name>
        │                              ─ IOServiceOpen type 0, sel 0 open, 2 write, 3 read (async), 4 sendImage, 5 readRegister …
        │ ApplePDPHelper ─ "AppleIPAppender" user client (flows, dormancy, Block PDP traffic, …)
        ▼
   kernel: AppleBaseband / AppleBasebandPCIControl / AppleBasebandPCIInterface{pbl,sahara,…,qmux1,qmux2,…} / AppleIPAppender
```

- [E] Channel table in `libPCITransport.dylib` @ `0x20c64c430`, 16-byte `{id, name*}` entries:
  `0 pbl, 1 sahara, 2 diag, 3 refs, 4 qmux1, 5 qmux2, 6 rom, 7 boot, 8 ari, 9 trace, 10 at, 11 dpl, 12 mediactl, 13 vcom,
  14 gnss, 15 qdss, 17 virtual_timesync, 20 boot_logger`. This is identical to the reference's `coachz_bb_channels.h`.
- [E] `libIOABP.dylib` matches `AppleBasebandPCIInterface` and `ABPInterfaceProtocol`, and calls selectors 0 to 0xc through
  several IOConnectCall* variants. This is consistent with the reference's 13-entry table (open, close, write, read,
  sendImage, readRegister, abort, start, timeSync, mapMemory, unmapMemory, register/deregister time event). It is not
  verified selector by selector. [R]
- [E] `AppleBaseband` board selectors (`libTelephonyBasebandDynamic` exported functions, the `w1` immediate before the
  call-method helper) match the reference's `coachz_bb_control.h` numbering:

  | Selector | Function |
  | --- | --- |
  | 0 | ForceResetModem |
  | 1 | PowerOnModem |
  | 2 | EnableModemMux |
  | 6 | Powercycle |
  | 8 | VBUS |
  | 0xb | PMUExtOn |
  | 0x10 | DiagReady |
  | 0x11 | FSEraseDone |
  | 0x12 | ResetReq |
  | 0x13 | CoredumpGPIO |
  | 0x16 | SetManagerState |
  | 0x19 | ChargerDesense |
  | 0x20 | ClearFreqList |
  | 0x2b | SPMI crash |

  The 16.5 strings `Failed to self-identify with AppleBaseband` and `AppleBaseband IOService is not found. Finding it via
  matching service` say ABM waits for the service to appear.
- [E] ApplePDPHelper's 16.5 strings list the same AppleIPAppender operations the reference stubs: "Enable metadata trailer
  append", "Set dormancy", "Block PDP traffic", "Get Keepalive Offload Frames", and so on. The reference answers selector 16
  (block) and zero-fills the rest. [R]
- [E] BBUpdater firmware location (`libBasebandCapabilities`): `/usr/local/standalone/firmware/Baseband/Mav17`, and the same
  path under the personalized preboot base (`Failed to obtain the default personalized FW base path`, `libBBUpdaterDynamic`).
  Mav17 shares the Mav16+ code paths (`mav16Aggregator`, `Mav16QMIDataContextIP`, `fLegacyPreMav16*`).

### 1.4 Boot and firmware handshake (what BBUpdater needs) — [R], mechanism confirmed present in 16.5

1. ABM powers and resets the modem through `AppleBaseband` (selectors 1, 0, 6, 22, 26/27 state). The bridge only records
   these and never touches the real SDX55.
2. BBUpdater opens `pbl` and reads the BHI registers with selector 5:
   - reg 0: serial;
   - reg 3: MSM HW id, with the chip id at +4. **Must be Mav17's `0x0003B0E1`** (bbfw Info.plist; the reference uses Mav16's
     `0x9540E1`) [S];
   - reg 4: PK hash. The reference returns BBULoader's default (unfused) hash, so unpersonalized images are allowed
     (`kBBUReturnUnPersonalizedImageError` otherwise) [R].
3. BBUpdater does `sendImage` (sel 4) of SBL on `pbl`. The bridge drops it and arms Sahara.
4. On `sahara`, the device side says HELLO. In command mode it answers execute 2 (HW id) and execute 1 (serial), then asks for
   image id 0x15 (SBL1, the same id our real Sahara uses in `md_image_index(21)`): ELF header, phdrs, segments. It discards the
   bytes and sends DONE "complete".
5. BBUpdater reports success, ABM's boot state machine moves to "booted", and it opens `qmux1`/`qmux2` and starts QMI CTL.

Which BBUEUR loader and programmer Mav17 uses (EUR10/13/16/20 classes exist in 16.5) is not yet known [S]. The bridge's Sahara
logs every request, and that log decides it.

### 1.5 QMI transport (QMUX over channels qmux1/qmux2) — [R], framing is standard QMUX

- Frame: `01 | len16 | flags(00 host / 80 svc) | service | client-id | SDU`. The SDU for services ≠ 0 is byte-identical to QMI
  over QRTR. Bridge rule: strip 6 bytes to the modem, add 6 bytes back.
- CTL (service 0) is answered locally, as libqmi's QRTR backend does:
  - GetClientId: allocate a cid per (channel, service), bound to one local QRTR port (reference base `0x4100`; ours uses
    `0x4000–0x4005` and `0x4010–0x4013`, so no clash [E]);
  - ReleaseClientId: QRTR DEL_CLIENT;
  - Sync: release all of the channel's clients;
  - GetVersionInfo: from the NEW_SERVER table;
  - power-save CTL messages: accepted.
- The reference's ABM queue is 1024 frames per channel. With 64, boot-time indications were dropped and CommCenter reset the
  baseband (ATCS_TIMEOUT) [R].

### 1.6 State to CommCenter, activation and the carrier bundle — [E] present in 16.5, values [R]

16.5 has `QMIMav13ActivationCommandDriver`, `QMIBasebandSettingsDriver`, and the properties `baseband_activated` and
`is_postponement_ticket_available` (strings in `libCommCenterMCommandDrivers.dylib` and `CommCenter`).

The reference answers these BSP (private service `0xe2`) messages itself:

| Message | What the bridge does | Effect |
| --- | --- | --- |
| `0xb00c` ActivationRegister | answers success, then indicates TLV 0x01 = Activated(0), 0x4f/0x50 = Provisioned(2), 0x56/0x57 = slot accepted(1), 0x42 ICCID (10 B BCD reading order), 0x43 IMSI (8 B) | Without it: no operator name, no attach, no data context |
| `0xb001` ChipInfo | `{chip id, serial}` | Without it: logs "Unable to get chip id and serial number" on every query |
| `0xa000` SendFile (PRI/GRI carrier config, 1 KiB segments) | acks with TLV 0x40 = next segment, drops the data, then sends indication `0xa001` FileTransferStatus `{type, 0}`, sub 1 | Otherwise CommCenter resets the baseband every ~12 s |
| `0xe00d` SetConfig | ack | — |

The bridge sniffs ICCID and IMSI from CommCenter's own UIM ReadTransparent answers (EF 2FE2/6F07). They are never logged.

16.5 contains the matching log strings: "Received FileTransferStatus indication", "Reading chip id and serial number",
"Activation request succeeded. Waiting for indication" and "Missing activation info TLV!".

## 2. QMI: Apple-private vs standard, and the minimum for bars and data

Service names known to 16.5 libQMIParser [E], in table order: `CTL WDS DMS NAS QOS WMS PDS AT VS CAT UIM PBM 787 AUDIO BSP CIQ AWD
COEX DSD MFSE VINYL MAVIMS ELQM PDC P2P MS WDA APPS SFT DFS SSCTL`. The Apple-private ones sit in `0xe1..0xea`
(reference: AUDIO 0xe1 to SFT 0xea, BSP 0xe2). The exact 16.5 numbers are [S], to be confirmed by the bridge's
"private QMI service" log.

[E] Collision: the SDX55 itself announces service **228 (0xe4)**. The bridge must treat `0xe1–0xea` as private and never forward
them; the modem's own 0xe4 is simply unused.

Minimum set:

| Need | Services/messages | SDX55 standard QMI covers it? | Bridge action |
| --- | --- | --- | --- |
| Baseband "booted" | AppleBaseband board selectors, pbl/sahara handshake | n/a | emulate (§1.4) |
| QMUX clients | CTL | yes (QRTR has no CTL) | answer locally |
| Activated and provisioned | BSP 0xb00c, 0xb001 | no (Apple-private) | answer locally (§1.6) |
| Carrier bundle accepted | BSP 0xa000/0xa001, 0xe00d | no | ack and drop |
| SIM state, ICCID/IMSI | UIM 0x0b (card status, read transparent) | yes | forward, and sniff ICCID/IMSI for the activation TLVs |
| Registration, operator, bars | NAS 0x03 (serving system, sig info, sys info, PLMN name, indications register) | yes | forward |
| | NAS `0x555b` (Apple vendor frequency reporting) | no (INVALID_QMI_CMD) | ack locally [R] |
| Radio on/off, IMEI | DMS 0x02 (operating mode, IDs, revision) | yes | forward. CommCenter owns airplane mode from now on |
| Cellular data | WDA 0x1a set data format, WDS 0x01 (bind mux data port 0xa2, set IP family, start/stop network, runtime settings, packet-service indications), QOS 0x04 bind | yes; endpoint `{3,4}` is native on SDX55 | forward. WDS `0x555b` link-stats: ack locally [R] |
| Everything else private (AUDIO, CIQ, AWD, MAVIMS, ELQM, P2P, MS, APPS, SFT, VINYL) | — | no | `QMI_ERR_NOT_SUPPORTED` (94); log each id once |

[E] ABM sets the data format itself: "Success setting DataFormat" and "Success setting QMAPSettings", with WDA TLVs
`UpLinkDataAggregationProtocol`, `DownLinkDataAggregationProtocol`, `…MaxDatagrams`, `…MaxSize` in libBasebandManager.

Our IP_HW0 path only parses basic QMAP (5) (`umi_modem_data.inc`, `umi3_qmap_next`). Whatever ABM asks (QMAPv5 with checksum
header, or a low-latency second format) must be supported directly in the parser, not rewritten [S]. The first trace answers
it (`qmi: host … svc 0x1a msg 0x20`).

Safety: the modem's EFS is loaded read-only (Sahara/TFTP) and its EFS syncs are discarded in RAM (`umi_modem_mhi.inc:862-930`).
So no QMI request CommCenter sends (PDC, NV-type DMS/NAS settings) can persist anything to modemst/fsg/fsc/persist. The bridge
does not need a write filter for that. The bridge's trace prints QMI header fields only, never TLVs, so IMEI, IMSI, ICCID,
numbers and APNs never reach the log.

## 3. Minimal implementation and phases

### Architecture

```
CommCenter/ABM ─IOUserClient─▶ hakios.umi_baseband.cpp (port of coachz_baseband.cpp)
                                   │  queues (IOLock); CTL + Apple-private answered here
                                   ▼  umi_bb_poll() / umi_bb_qrtr() / umi_bb_new_server()
                    md_service thread (umi_modem_mhi.inc): the only owner of MHI DL20/21 + md_qrtr_send
                                   │
                              SDX55 QRTR node 3 (NAS 47, UIM 50, DMS, WDS, WDA …)
data: pdp_ip0..3 (bridge-owned ifnets, mux 1..4) ⇄ QMAP on MHI IP_HW0 100/101 (existing md_data_poll)
```

### Phase 1 — bars, carrier and Settings → Cellular, with CommCenter owning WDS (the data switch)

Changes:

| File | Change |
| --- | --- |
| `drivers/umi/hakios.umi_baseband.cpp` (new) + `overlays/xnu/files/iokit/Kernel/` and `bsd/dev/umi_bb_*.h` | Port of `coachz_baseband.cpp` and `coachz_bb_{qmux,control,sahara,channels,data,pdp}`, with: **no** eSIM/VINYL module (VINYL answered NOT_SUPPORTED), **no** DPM/IPA/endpoint rewrite, `CBS_CHIP_ID = 0x0003B0E1`, and the log prefix `UMI BB:`. Keep the BSD-2 notice and attribution. |
| `drivers/umi/conf` | `file iokit/Kernel/hakios.umi_baseband.cpp iokitcpp` |
| `umi_modem_mhi.inc` | `md_qrtr_packet`: hand every `NEW_SERVER`/`DEL_SERVER` to `umi_bb_new_server`/`_del_server`, and every type-1 packet whose `dst_port` is in `0x4100–0x413f` to `umi_bb_qrtr`. `md_service`: call `umi_bb_poll()` each loop and send its batch with `md_qrtr_send(1, port, node, svc_port, …)`; DEL_CLIENT goes as control type 6. Replace the 100 ms idle `delay_for_interval` with a timed wait that `umi_bb` wakes on a queued write. Otherwise ABM's QMI latency is up to 100 ms per request. Publish the services at `md_thread` start, so CommCenter never sees "AppleBaseband not found". Sahara HELLO is held until QRTR discovery is done (BBUpdater waits 40 s and ABM retries on a timeout). |
| `umi_modem_mhi.inc` / `umi_modem_data.inc` | Delete the kernel's own UIM/DMS/NAS clients, the WDA/WDS dial, pdp_ip0 address config and the DNS selftest. The kernel keeps boot, QRTR NS, servreg, TFTP, EFS and the IP_HW0 QMAP pump, and exposes `umi_modem_data_send(mux, …)` / `umi_modem_data_receiver(cb)` for the bridge's pdp layer (`coachz_bb_pdp.cpp` adapted: 4 ifnets `pdp_ip0..3`, MTU from the ifnet, PF_INET/INET6 plumbers). `umi_modem_net.inc` (the kernel's own pdp_ip0) is deleted. |
| `loader/loader.c` | When `umi_modem=1` (already on the whitelist, already in the boot line; **no new boot-arg**), emit `/baseband` (`name=baseband`, `device_type=baseband`, `compatible=baseband,n41`) and `/product/baseband-chipset = "mav17"`. |
| `overlays/userland/mkjobs.py` | Remove `'CommCenter'` from `DENY`, so the cellular CommCenter job ships (`ALLOW=CommCenter` exists for a trial build). |
| `overlays/userland` build + `init.c` | Package the unzipped Mav17 bbfw (local only, proprietary; 64 MB, or only the files ABM opens once the trace shows which) at `/bbfw/Mav17` in the payload. `bind_dir()` it onto `/flash/usr/local/standalone/firmware/Baseband/Mav17` and the preboot-personalized path, using `synthetic_dir` for missing parents. Use bind mounts, not symlinks: CommCenter's sandbox doesn't follow them [R]. |

Grep on the next run (all header-only; no identities):

| Grep | Meaning |
| --- | --- |
| `UMI BB: N of 22 services published` | services up |
| `UMI BB: first user client on pbl` → `pbl sendImage … dropped` → `sahara hello` → `sahara finished` | BBUpdater accepted the emulated boot |
| `UMI BB: first user client on qmux1`, `qmi: host ch 1 svc 0x3 …` | ABM's QMI is reaching NAS |
| `UMI BB: activation register answered, state activated`, `slot one ICCID read from the card` (value never printed) | activation |
| `UMI BB: carrier file type … taken`, then no repeating `AppleBaseband selector 0` resets | carrier bundle |
| `private QMI service 0x… answered not supported` (once per id) | the actual private set |
| launchd | No more `failed lookup … commcenter` lines, and CommCenter is not respawned (no `exited due to SIGSEGV/SIGABRT` for `com.apple.CommCenter`) |
| `mg_test: HasBaseband = true` | MobileGestalt sees the baseband |
| Screen (user) | carrier name and bars; Settings → Cellular present |
| Data | `UMI BB: pdp: WDS … message 0x20 … error 0`, `pdp_ip0 active families 1`, Safari over cellular with Wi-Fi off |

Risks:

- **(a)** The postponement-ticket gate blocks the Internet context (§6.1). Bars and carrier still work in that case.
- **(b)** BBUpdater's Mav17 flow differs from Mav16 (more images, PK-hash check), visible as `sahara failed` or `item not found`.
- **(c)** ABM's WDA format isn't basic QMAP.
- **(d)** CommCenter crash-loops if any IOKit call shape differs. Mitigation: the `UMI BB:` "selector N unsupported" lines, and
  `KeepAlive` respawns are visible in launchd.
- **(e)** CommCenter resets the "baseband" on a timeout. The bridge then re-runs Sahara and replays nothing to the real modem,
  so the QMI state can go stale. The reference saw this only when frames were dropped.

Effort: 2–3 days of code (mostly mechanical porting plus hooks), then 3–6 short boots.

### Phase 2 — robustness and IPv6

- IPv4 and IPv6 handled per family.
- Flight-mode cycles (DMS operating mode from CommCenter).
- Modem DEL_SERVER/restart: send a CTL Sync indication to ABM.
- QMAP format from the WDA reply.
- APN from the CMCC carrier bundle instead of our hard-coded `cmnet`.

Verify: toggle Cellular Data and Airplane mode 10× each without `UMI BB: dropped frame`, and `pdp_ip0 active families 3`.
Effort: 1–2 days.

### Phase 3 — calls and SMS (later; big unknown)

China Mobile LTE voice is VoLTE. On Mav, CommCenter drives IMS through Apple-private services (`MAVIMS`, `AUDIO` 0xe1) that
SDX55 doesn't have.

- Voice needs a MAVIMS ⇄ Qualcomm IMSA/IMSS/VOICE translation and an audio path (BasebandVoice HAL ⇄ SDX55 PCM/I2S, ADSP) [S].
- SMS over IMS or SGs through standard WMS (0x05) may work with forwarding alone [S].

Effort: weeks.

## 4. Data path: hand dialing to CommCenter

Recommendation: **hand it over**. Keep the transport, delete our dial.

- The Cellular Data switch, airplane mode, APN and carrier settings, reconnection, the "LTE" status-bar glyph, and
  configd/DNS/default-route publication only work when CommCenter's data context owns the call. Our kernel dial has
  `default_route=manual` and no configd service, so iOS apps don't use it today. Removing it loses nothing user-visible.
- Two WDS owners on one APN conflict. A second Start Network typically returns NoEffect (26), and CommCenter treats it as a
  failure and retries [S]. So the kernel must not dial once CommCenter runs. The bridge observes WDS only (it parses
  bind/start/stop/status for the mux→ifnet mapping) and never invents addresses: CommCenter configures pdp_ipN itself.
- What stays in the kernel: MHI IP_HW0 channels 100/101, QMAP framing, the RX/TX pump, and the four pdp_ip ifnets (mux 1..4)
  owned by the bridge's pdp layer.
- As built (§8), the kernel's dial is not deleted: it is `umi_modem=1`, and `umi_modem=2` is the bridge, where the kernel
  keeps only the transport and its WDA link format.

## 5. Notes carried over from the Codex draft

- `com.apple.commcenter.dm-helper` is a different job and is already in the payload.
- Of the AppleIPAppender/ApplePDPHelperInterface/AppleBasebandPCI implementations, none existed in the tree. This design adds
  them in the kernel; CommCenter is unmodified (except §6.1, only with approval).
- The `hakios: umi-idle telephony reconnect retry dropped (no CommCenter)` hook (in the v21 log) and the vmd entry in `DENY`
  become obsolete once CommCenter runs. Other agents own them; they are listed in §7.

## 6. Open questions and risks (ordered)

1. **Postponement-ticket gate.**
   - On 17.7.11 CommCenter blocks the Internet context without Apple's postponement ticket, and the reference patches one
     branch (`coachz_cellular_compat.py`, ad-hoc re-sign and trust cache).
   - 16.5 has the same `is_postponement_ticket_available` property [E]. Whether the same gate exists in 16.5's data-context
     selection is unknown.
   - First check whether data comes up unpatched. If not, find the 16.5 equivalent branch in `CommCenter`
     (rootfs `System/Library/Frameworks/CoreTelephony.framework/Support/CommCenter`, 24.7 MB).
   - Patching is not "stubbing", but it changes an Apple binary, so it needs the user's explicit OK first.
2. **Mav17 BBUpdater flow.** The loader/programmer class, the image ids requested, and whether the PK-hash default is accepted.
   The bridge's Sahara and register-read logs answer all three in one boot.
3. **ABM's WDA data format** (QMAPv5 or a low-latency second format) vs our QMAP(5) parser.
4. **Timing.** CommCenter starts at launchd (`02:07:34` in v21); the SDX55 reaches QRTR services a few seconds after mission
   mode. Services are published early and HELLO is held, as designed above.
5. **Lockdown activation state, eSIM and SIM-lock checks** that the reference's iPad didn't hit, for example
   "BB activated, but there is no SIM" if the ICCID sniff misses.
6. **ICCID/IMSI and IMEI** stay in kernel memory and pass to CommCenter, as on a real phone. The trace never prints TLVs.
   Keep it that way in the port.

## 7. Outside this task

- The UMI idle-timer telephony hook and the `vmd` DENY entry, once CommCenter runs.
- `data selftest icmp 1.1.1.1 … no reply` in v21 (DNS works, ICMP does not). This goes away with the dial removal, but may
  point to a QMAP TX issue worth checking when CommCenter data is first tested.

## 8. Phase 1, step 1 — the first bootable bridge build (2026-11-11)

The value of the existing `umi_modem` boot-arg picks the mode. No new boot-arg name.

| | `umi_modem=1` | `umi_modem=2` |
| --- | --- | --- |
| Kernel | today's path unchanged: UIM/DMS/NAS, WDA + WDS dial on `cmnet`, `pdp_ip0`, selftest | boot, QRTR NS, servreg, TFTP, EFS, and the WDA link format (raw IP, QMAP 5, endpoint PCIe/4) on IP_HW0. No UIM/DMS/NAS/WDS of its own |
| Loader DT | as before | adds `/baseband` (`baseband,n41`) and `/product/baseband-chipset = mav17` |
| CommCenter | not loaded (no `/baseband`) | loaded by launchd (`_LimitLoadToDeviceTree = baseband`, KeepAlive) |
| Bridge | absent | `AppleBaseband*`, `AppleIPAppender` and the Mav PDP manager published at modem-thread start; `pdp_ip0..3` = QMAP mux 1..4 |

One payload serves both modes. `com.apple.CommCenter` is a named job that keeps its `_LimitLoadToDeviceTree`, and the
noncellular variant is denied. The helpers (CommCenterRootHelper/MobileHelper, abm-helper) come with `--rest`, on demand.

Apple's Mav17 files are unpacked locally by `tools/stage-baseband-firmware.py IPSW` into `<repo>/fw-baseband/Mav17`, which
git ignores. They are 62 MB, 34 MB compressed. That does not fit beside the 18 MB kernel under the 63 MiB payload limit
(`flash/prepare.py` refused a 58.6 MB CA payload), so they go in the resident **flashboot.apfs**. See §9 for the folder (the personalized path) and the `/usr/local` bind.

A CA-only build leaves flashboot.apfs untouched. A non-CA `flash/build.sh userland` makes the new one, and it is then
uploaded with `xl ramdisk` / `RAMDISK=`. The personalized preboot path is not provided: ABM's `FW base path` line in the first
run says whether it is needed.

Files:

- `drivers/umi/hakios.umi_baseband.cpp` and the `conf` entry.
- `overlays/xnu/files/bsd/dev/coachz_bb_{qmux,control,sahara,channels,data,pdp}.h`, imported unchanged except the Mav17
  chip id.
- `bsd/dev/umi_baseband.h`.
- `umi_modem.c` (mode 2, wakeable idle), `umi_modem_mhi.inc` (QRTR routing, no kernel QMI in mode 2),
  `umi_modem_data.inc` (`md_data_link` split out; any mux and IPv6 to the bridge, muxed uplink).
- `loader/loader.c` (whitelist `umi_modem=2`, DT nodes).
- `overlays/userland/{mkjobs.py,build.sh}`, `flash/flash-init.patch`, `flash/prepare.py`, `prepare.py`.
- Tests: `tools/test-modem.py`, and `tools/modem-tests/{bb.c,mhi.c}`. `bb.c` is the reference's protocol model without eSIM.

The v21 ICMP finding (`data selftest icmp 1.1.1.1 … no reply`, DNS over the same path answered) is not a framing bug. The
36-byte echo uses the same QMAP header path as the 59-byte DNS query that got its reply, and the IPv4/ICMP checksums are
computed as for the query. 1.1.1.1 is commonly unreachable from mainland-China carriers, so nothing was changed.

Expected `UMI BB:` order on a good run (grep `UMI BB:\|UMI MODEM: start\|data link`):

1. `UMI MODEM: start external-X55 mode=bridge (CommCenter dials)`, then `UMI BB: pdp: pdp_ip0 mux 1 registered result 0`
   (×4) and `UMI BB: 22 of 22 services published`.
2. `first user client on AppleBaseband`, `first user client on pbl`, `pbl register 3 read`, `pbl sendImage … dropped`.
3. `sahara opened` → (`hello held` →) `X55 QMI is up (N services known): the held sahara hello is sent` or `sahara hello`,
   then `sahara finished, image 1 of 1 (id 0x15)`.
4. `qmux1 opened`, `first user client on qmux1`, `qmi: host ch 1 svc 0x2 …`, `qmi: to-host ch 1 svc 0xb …` (DMS/UIM/NAS
   answers).
5. `activation register answered, state activated`, `activation mode disabled (0x15=1) sent` (each indication carries
   ActivationMode kDisabled; COMMCENTER-ACTIVATION-umi.md gate 10), `slot one ICCID read from the card`, `slot one IMSI read from the card`,
   `carrier file type … taken`.
6. `qmi: to-host ch 1 svc 0x3 … type 4` (NAS indications: registration and signal), then on the screen: carrier and bars.
7. `data link: IP_HW0 raw IP over QMAP ready`, `data format asked by ABM …` / `data format now …`, `pdp: WDS type 2 message
   0xa2 …` (bind), `pdp: WDS … message 0x20 … active 1`, `pdp: pdp_ip0 active families 1`, then
   `pdp: 60 s pdp_ip0 … tx N rx M`.

Failure markers: `channel … selector N unsupported`, `AppleBaseband selector N unsupported`, `sahara failed`,
`dropped frame for ABM`, `private QMI service 0x… answered not supported`, `pdp: start failed … reason`,
`data format now … not basic QMAP`, and CommCenter respawns in launchd's log.

## 9. First hardware run of umi_modem=2 (v22a, 2026-11-11 11:23) and the fixes

What happened:

- 22 of 22 services were published, X55 QMI came up, and the IP_HW0 link was ready.
- CommCenter (pid 32) opened AppleBaseband and made 15 board calls, none unsupported.
- It never opened pbl. ABM's `log-bb-live-stats.txt` says
  `reset: reason=modem boot up failure [Baseband Firmware Path Not Found]`, and SpringBoard showed "Cellular Issue Detected".

Why [E]:

- `BootControllerLegacy::prepare` (libBasebandManager `0x1b7ecd2d0`) stats
  `capabilities::bb::radio::getPersonalizedFirmwarePath()`, not the plain path.
- That function (libBasebandCapabilities `0x1ae176168`) calls MSUDataAccessor `lookupPathForPersonalizedData(1, buf, 0x400)`.
  On a nonzero result it logs `MSU error: 0x%lx, FW base path` and returns "". Otherwise it appends `/Mav17` (suffix table
  `0x1d87a1628`, radio type 5).
- MSUDataAccessor (`copyPathForPersonalizedData:error:`) first finds the preboot volume with `copyMountPointForVolumeType:`.
  The default mount point for type 0 is `/private/preboot`, but the lookup goes through libpartition2 `LPMedia`/`LPAPFSVolume`
  `findVolumesWithRole:` (IOMedia `AppleAPFSVolume` with APFS role Preboot). Failure there is "Failed to locate preboot
  volume", error 0x1773.
- It then appends `IODeviceTree:/chosen boot-manifest-hash` as `%02X`. Failure there is error 0x1775.
- So the folder is `/private/preboot/<HASH>/usr/local/standalone/firmware/Baseband/Mav17`, provided the preboot lookup
  accepts our md0 volume. [S] It may not: our APFS publishes no AppleAPFSVolume IOMedia with a role.
  `tools/msu-path-probe.c` (built to `out/xiaomi-mi-10/probe/msu-path-probe`) asks MSU directly on the phone.

Fixes in this round:

- **flashboot** keeps the files at `<HASH>/usr/local/standalone/firmware/Baseband/Mav17`. HASH is the loader's
  `boot-manifest-hash` in uppercase hex: `726564776F6F64` and 82 zeros. That folder is
  `/private/preboot/<HASH>/...` after the root switch.
- `baseband-local` links to it, and init binds it onto `/usr/local` as before.
- **loader**: `/baseband` gets D22AP's `class = 3` and its 40-byte `config`.
  - lockdownd logged `no class property of IODeviceTree:/baseband`.
  - libTelephonyBaseband `_TelephonyBasebandControllerLoadConfigOnce` (`0x1e9a4adfc`) reads `config`, at least 24 bytes,
    from the service matched by `IOServiceNameMatching("baseband")`.
- **bridge**:
  - AppleBaseband is named `baseband` and carries the DT node's properties.
  - It accepts `StartDevice`/`StopDevice`, which libTelephonyBaseband sets (CFSTRs at `0x20c6e70c0`/`0x20c6e70e0`).
  - Every board call is logged.
  - `SIOCGIFMEDIA` (`0xc02c6938`, configd polling) is answered without a log line, and any other unknown ioctl is logged once.

Not the cause: `PPM Error:No matching driver found` is printed by wifid as well, so it is unrelated to the baseband.
`mg_test HasBaseband = false` can be MobileGestalt's cache in the persistent `/var`, written on the first boot without
`/baseband`.


## 10. v23: ABM boots the bridge, then resets it in a loop (ATCS_TIMEOUT)

What happened [E] (`runtime/logs/114605-run.log`):

- The firmware path, board calls, SAHARA, qmux1, activation and the carrier files all worked.
- Forwarded QMI was answered until t ≈ 58.7 s.
- Then CommCenter sent "Resetting baseband after pushing files (due to kCarrierBundleChange)". It is the expected one-time
  reset after the first carrier-file push.
- ABM's sequence for it includes DMS Set Operating Mode, and the X55 then reported mode `kReset_Mode`. The X55 ran its EFS
  sync (`EFS sync … acknowledged in RAM`) and never answered QMI again: from t ≈ 60 s, only locally answered messages reach
  ABM. Only umi_modem.c can boot the X55, so it stayed silent.
- Every later request (WDS 0xa2, DMS 0x25/0x54) timed out, and ABM reset again every ~26 s: `ATCS_TIMEOUT … svc=… client=…`.

Separately, WDS Bind Mux Data Port (0xa2) and QOS Bind (0x2b) name only a mux (the 11-byte SDU has one 1-byte TLV). The X55
refused them with QMI_ERR_INVALID_ARG (0x30), so radio.health logged "Error binding to mux data port 1..4".

Fixes:

- `umi_bb_pending.h`:
  - DMS Set Operating Mode 4 (reset) or 5 (shutting down) is sent as 1 (low power). The reset itself is ABM's through
    AppleBaseband/SAHARA, as before.
  - Every forwarded request is remembered until the X55 answers. A request still unanswered after 8 s is answered with
    QMI_ERR_INTERNAL.
  - A late answer to such a request is dropped.
  - A request that cannot be routed (unknown client, unannounced service, full queue) is answered at once.
- `coachz_bb_data.h`: `cbd_endpoint_fix` takes the target endpoint, so a mux-only bind gets the X55's `{PCIe 3, IP_HW0 4}`.
- `coachz_bb_qmux.h`: client slots, and so QRTR ports, are handed out round robin, so a port is not reused right after its
  DEL_CLIENT.

Grep:

- `UMI BB: req svc … -> X55 node … port …`
- `UMI BB: rsp svc … after N ms`
- `UMI BB: req … answered here: …`
- `UMI BB: no answer from the X55 in 8000 ms: …`
- `UMI BB: DMS operating mode 4 (reset) sent as low power (1)`
- `UMI BB: data port: service 1 message 0xa2 endpoint 0/0 added as 3/4`
- the 60 s `requests answered by the X55 N, by timeout M, in flight K`

## 11. v24: after the carrier-bundle reset ABM gets no QMI clients

In v24 the X55 answered everything (262 requests answered, 0 timeouts) and the DMS reset was taken as low power. ABM's
reset then went AppleBaseband selector 0, selector 1 twice, pbl, and SAHARA again.

The problem [E]: CommCenter logged `qmux: [qmux1::QMIControl] Baseband did not allocate client for svc type: WDS/NAS/UIM/QOS…,
error: 0x5 [QMI_ERR_CLIENT_IDS_EXHAUSTED]`. Every command driver stayed "not ready" (`ATCS_CLIENT_NOT_READY`, SIM
`kSimStateUnknown`), so the radio stayed kOffline.

Why: an Apple baseband loses all its QMI clients when it is reset, so ABM never releases them. The bridge kept all of them, and
its 64 slots were full.

Fix: AppleBaseband reset, power-cycle or power-on (selectors 0, 6, 1) now does three things:

- releases every ABM client (`cbq_reset_host_clients`), and `umi_bb_poll` sends a DEL_CLIENT for each one to the X55;
- drops what was still queued on qmux1 and qmux2;
- waits for the next activation registration.

Model test: `PASS answers … baseband reset frees ABM clients`.

Grep: `UMI BB: AppleBaseband selector 0 (reset or power) recorded; … N QMI clients of ABM released`. After it, there should be no
`CLIENT_IDS_EXHAUSTED`, and the `req`/`rsp` lines should continue past the reset.

## 12. v25: registered, data works, but the baseband resets every ~6 minutes, CommCenter crashed once, and NAS went silent

What worked: LTE registration, 4 bars, data on pdp_ip0 (tx 867 / rx 1079 packets in the first 91 s), and an MT SMS
(13:06:19, delivered to imagent).

1. **First reset (13:00:00).** The IMS context's (PDP 1) IPv4 half never completed.
   - Its Start Network failed at once: QMI 0xe, verbose 3GPP #51 "PDN type IPv6 only allowed". The response reached
     CommCenter, but its IPv4 BBReset timer (360 s) was never cleared.
   - This is not network throttling: no #26/#27/#33/back-off occurs, and the bridge passes the call-end TLVs unchanged.
   - After the reset, the same failure came as internal 208 (IPv4 call disallowed), and CommCenter did clear the timer.
   - Open: why CommCenter keeps waiting after #51.
2. **Second reset (13:06:03).** After the 13:00 reset, the Internet context's Start Network calls succeeded at once, but no
   Packet Service Status (WDS 0x22) followed. The X55 had kept the old calls: the reset only DEL_CLIENT'ed ABM's clients.
3. **NAS went silent.** After that reset the X55's NAS answered QMI_ERR_CLIENT_IDS_EXHAUSTED (0x5) and then nothing (78 → 116
   timeouts). The DEL_CLIENTs had not freed the X55's clients, so its NAS client table filled up. After CommCenter restarted:
   bars 1, "No Service".
4. **CommCenter SIGTRAP (13:07:39.75).** The log shows `QMI parsing error in message with msgId=0x0022 during tlvId=0x01
   (tlv_len=2)`, then `ABM ASSERT … QMITransportService.cpp Line: 57`, right after DMS set to low power.
   - [S] A WDS 0x22 indication for a torn-down call arrived on a QRTR port that by then belonged to a client of another service.

Fixes:

- **The X55's clients are never deleted.** When ABM lets go of a client (release, sync, baseband reset), the client and its
  QRTR port are pooled. ABM's next client of the same service reuses them; there is no DEL_CLIENT and no cross-service port
  reuse. There are 96 slots.
- **A baseband reset first stops ABM's started calls** (WDS Stop Network with each call's handle). Then a new Start Network
  connects afresh and gets its 0x22.
- **A message from the X55 reaches a client only from that client's service's node:port.** Anything else is dropped and logged.
- An error answer for a timed-out request goes only to a client that still holds the port.

Grep next run:

- `UMI BB: AppleBaseband selector 0 (reset or power) recorded; … N QMI clients of ABM pooled, M data calls stopped`
- `UMI BB: X55 message for port … not its service's … dropped` (the cross-service case)
- `requests answered by the X55 N, by timeout M`: M should stay near 0, and no NAS `no answer` lines
- no `ABM ASSERT`
- the first `[DATA.QMIContext.1:1:] fireResetBasebandTimer` at about boot + 360 s (the open IMS IPv4 item)

## 13. The IMS IPv4 half: CommCenter waits for an indication after a 3GPP refusal

Code [E]: in `libCommCenterMCommandDrivers` (20F66), `QMIDataContextIP::handleActivateResponse` (around `0x1f0ea8f70`) reads
the verbose call-end reason type of a refused WDS Start Network.

- For type 6 (3GPP) or 1 (Mobile IP), it only logs `wds::kErrorTypeMobileIP or wds::kErrorType3GPP received handling those
  in indications` and returns. It waits for the WDS Packet Service Status indication (0x22), "disconnected", carrying the
  reason.
- For any other type, it fails the activation from the response alone.
- That is why internal reason 208 (after the 13:00 reset) cleared BBReset timer[1] at once, and 3GPP #51 at boot did not.

What the X55 did: no 0x22 for the refused client (cid 10). Its only 0x22 on that mux went to another WDS client (cid 14) and
reported the IPv6 call as connected. So the IPv4 half stayed activating until the 360 s timer reset the baseband.

Fix: when a Start Network response carries an error with verbose type 6 or 1, the bridge sends that client the Packet Service
Status indication Apple's baseband sends. Its TLVs:

- 0x01 {1 disconnected, 0};
- the response's own 0x10 call-end reason and 0x11 verbose reason;
- 0x12, the client's IP family.

Nothing is invented: the failure and its reason are the X55's. The builder is `ubp_start_failure_indication`, with a model test
in `PASS answers … 3GPP/MIP start refusal indicated`.

Grep: `UMI BB: pdp: start refused (cid N family 4, verbose type 6 code 51): disconnected indication sent with the X55's reason`.
Then no `[DATA.QMIContext.1:1:] fireResetBasebandTimer` at boot + 360 s.

## 14. MT SMS stops at MessagesBlastDoorService (SIGTRAP): see SANDBOX-umi.md

BlastDoor refuses to run unless `sandbox_check(getpid(), NULL, 0) == 1`. The kernel Sandbox stand-in now records
processes with a `seatbelt-profiles` entitlement (and sandbox_init/inheritance) and answers that question as a device does.
