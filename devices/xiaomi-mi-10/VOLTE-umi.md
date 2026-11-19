# VoLTE on umi: feasibility and comparison with GSM CS

Research: 2026-11-11, iOS 16.5 / 20F66, external SDX55, China Mobile.

**Conditionally feasible, substantially harder overall than GSM CS, but a full modem-IMS translator is not necessarily required.** This iOS image contains Apple's own SIP/IMS client. CommCenter already requests the CMCC IMS PDN through our WDS/QMAP path, and its IPv6 activation initially succeeds. The immediate failure is completing iOS's bearer lifecycle (`kDADMissing`); later retries also receive authentication/service rejections. No successful IMS registration or VoLTE call is demonstrated. GSM remains the quicker call-control experiment where CS service exists; VoLTE is the durable LTE voice target.

Research only: this report is the only file created; no source changes, builds, device commands, or phone access. Read `CLAUDE.md`, `VOICE-CS-umi.md`, and `COMMCENTER-umi.md`. Logs were filtered for identity fields, with additional redaction of identifier-shaped values. No subscriber identifiers, telephone numbers, SIP identities, or message contents are reproduced here. Public-web verification was unavailable because DNS failed; current local GSM coverage and operator retirement dates remain unverified.

Evidence: paths below are relative to the repository unless absolute. **V28** = `logs/test-1111-172537-umi-v28.kernel.log`; **V25/26/27** = the corresponding `test-1111-125250/140149/153851-umi-v25/v26/v27.kernel.log` files. Log references use original line numbers. **IPT** = `tools/tmp/dsc-20F66/System/Library/PrivateFrameworks/IPTelephony.framework/Support/libIPTelephony.dylib`; **QP** = `tools/tmp/dsc-20F66/usr/lib/libQMIParserDynamic.dylib`. `file+0x…` is an extracted Mach-O file offset; `VM 0x…` is an unslid binary address.

## 1. Which IMS stack can run here?

### iPhone: AP SIP/IMS, with selectable media backends and modem cooperation

The AP-client hypothesis is supported by executable imports, exported functions, protocol strings, and disassembly—not just an "IMS" plugin name:

| Inspected evidence | Meaning |
| --- | --- |
| `tools/tmp/cc-activation-20F66/CommCenter`: `otool -L` links IPT; its `.nm:398,1001–1019` imports `InitializeIPTelephony`, `IMSStartCall`, `IMSAnswerCall`, `IMSEndCall` | Native CommCenter uses the AP IP telephony library. |
| IPT: `SipStack` at file+`0x36db4f`, `REGISTER` at `0x3aad3b`, SIP wire-version strings at `0x3cc704` and `0x3d6ba2`; registration handling at `0x3c7018` explicitly processes REGISTER responses and the 401 challenge | This is a SIP client implementation, not merely a proxy for modem registration status. |
| IPT: `ImsUdpSocket`, `ImsTcpSocket`, `ImsTlsSocket`; interface binding at file+`0x3aa8a4`; `IMSWrapperDelegate::handleAuthenticate` at VM `0x1b7c1f4c0`; `IMSSendAuthChallengeResponse` at VM `0x1b7d03380` | It has host networking and SIM-authentication integration. |
| IPT: `NEIPSecDBAddSA failed` at file+`0x3b5455`, registration failure on IPsec SA initialization at `0x3c6fa1` | The AP implementation also handles IMS security associations. Working public Internet is insufficient. |
| IPT: `LegacyQMIRTPStackController`, `QMIRTPStackController`, `AVCRTPStackController`, `AVCRTPAudioSession` | SIP ownership and speech/RTP ownership are separate decisions. |

The inspected rootfs/cache has `IPTelephony.framework`, not a standalone `IMS.framework` or `IMSCore.framework` under those names. `IMShared*` belongs to a different framework family and is not evidence of cellular IMS. The concrete 20F66 implementation is IPT plus CommCenter and its command drivers.

**Therefore, iOS could run its own IMS client over an X55 IMS bearer.** An external Qualcomm modem does not inherently require replacing Apple's SIP client with the modem's SIP client. However, its private modem contracts still matter: NAS IMS preferences/status, WDS IP/P-CSCF settings, SIM AKA, QoS/dedicated bearers, media initialization, and call/power/handover coordination. Which are mandatory versus optional needs a trace after the PDN stays usable.

### Apple-private interfaces: distinguish signaling assistance from the SIP stack

Disassembly gives a useful correction to broad "MAVIMS translation" claims:

| Interface | Verified 20F66 evidence | Implication |
| --- | --- | --- |
| MAVIMS = **`0xe6`** | QP VM `0x1e9a719d4–0x1e9a719e4`; IPT's `LegacyQMISipLogger:MAVIMS` startup passes `0xe6` at VM `0x1b7b69010` | At least one MAVIMS consumer reports/logs AP SIP and call events to the modem. Its existence does not relocate the SIP client into the modem. |
| ELQM = **`0xe7`** | QP VM `0x1e9a719fc–0x1e9a71a0c` | V28's rejected `0xe7/0x0004` and `0xe7/0x0001` at lines 11149–11151 and 11471–11473 are **ELQM, not IMS registration requests**. |
| AUDIO = `0xe1` | IPT VM `0x1b7d69564`; V28:18084–18088 rejects AUDIO `0x002f` with `0x5e` | A real unresolved Apple audio interface; its effect on a VoLTE session has not yet been measured. |
| RTP/media service variants | IPT constructs one RTP client with service `0x28` at VM `0x1b7d68eec`, and the legacy driver with `0x52` at `0x1b7be048c`; QP maps `MS` to `0x52` at `0x1e9a71a84` | Do not classify every media service as Apple `0xe*`, or assume X55 implements Apple's message/TLV dialect. Neither service 40 nor 82 appears in V28's logged server announcements. |

The current bridge forwards ordinary QMI and returns NOT_SUPPORTED for unimplemented private services (`drivers/umi/hakios.umi_baseband.cpp:455–494`; `coachz_bb_qmux.h:61,303–325`). V28's logged host requests include AUDIO and ELQM, but none for MAVIMS, IMSA, IMS settings, or these RTP service variants. Header logging is bounded, so absence from this trace is not proof of a permanent architectural prohibition.

**`COMMCENTER-umi.md` §3's MAVIMS↔IMSA/IMSS/VOICE proposal is one possible redesign, not an established prerequisite.** A one-to-one service-number rewrite would be wrong: translating a logger or RTP operation into modem SIP registration is a semantic mismatch.

### X55 native IMS: an alternative, with substantial AP work

V28:2183–2184 advertises service **33 (`0x21`, IMSA)** at node 3/port 71 and **18 (`0x12`, IMS settings)** at node 3/port 73. This supports a modem-IMS route; service availability alone does not prove registration. IMS presence/video/other endpoints depend on firmware and need separate enumeration; do not assume the entire IMS service family is exposed.

A particularly relevant local reference is `/Users/xm666/X/mainline/umi-linux/work/modem-data/refs/modem-userspace/README.md:127–152,176–183`. On another SM8250/external SDX55M device, it documents:

- `qcom-imsd` binds/enables IMSA/settings and configures voice-domain preference.
- `81voltd` supplies host **IMS Data service 770 (`0x302`)**, satisfying modem-originated bearer requests with a separate QMAP IMS connection.
- IMS registration succeeds and outgoing VoLTE reaches `ringing-out`, then hits an audio/codec-routing blocker.

This is reference-project testimony for Meizu 18X, not a reproduced umi result. Its pinned source submodule directories are empty locally; the README, patches, and units are available. It nonetheless shows why "Android only configures it" is incomplete: modem IMS also depends on AP bearer services, vendor configuration, subscription binding, audio and lifecycle management.

Using this route would require reproducing those host services and adapting CommCenter's IMS API/events/call model to a modem-owned SIP client. The existing QMUX bridge does not do that. Select **one registration/bearer owner**: do not let Apple and modem IMS independently register the same subscription and compete over the IMS PDN.

## 2. CMCC bundle and actual boot behavior

Read directly, without mounting or extraction, from `work/umi-v23/sysimage/rootfs.dmg` using the existing read-only APFS reader. Bundle: `/System/Library/Carrier Bundles/iPhone/CMCC_cn.bundle`, version **54.0.1**. For the emulated Qualcomm D22 device, relevant settings are in **`overrides_D20_D21_D22.plist`**, not just `carrier.plist`. The D201/D211/D221 override also has IMS settings.

| D20/D21/D22 key | Value / significance |
| --- | --- |
| `SupportsImsCapability` | `true` |
| `ShowVolteSwitch` | `true` |
| `IMSConfig/Voice/EnableVolteByDefault` | `true` |
| `apns/.../configuration` IMS entry | APN **`IMS`** (case-insensitive network APN `ims`), type-mask **131072 / `0x20000`**; blank username/password; `WaitForBringUpComplete=true`; default/allowed/roaming protocol masks `3` |
| `IMSConfig/Signaling/UseIPSec` | `true` |
| `IMSConfig/Signaling/DefaultAuthAlgorithm` | `AKAv1-MD5` |
| `IMSConfig/Media/PreferredAMRWBMode` | `8`; AMR-WB media configuration exists |
| `AttachAPN/3GPP/PcscfAddressRequired` | `false` for **attach**; this does not mean the IMS client can register without a P-CSCF |

VoLTE is already enabled by carrier policy: V28 first reports disabled during bootstrap, then **enabled provisioning/feature** at **14403,14428,14450**. A forced enable flag is not the missing implementation.

| V28 observation | Evidence and interpretation |
| --- | --- |
| IMS APN loaded, assigned `Settings.1:2` | **13435,13548**: `IMS`, mask `0x20000`, IPv4v6 allowed. This is a distinct context from Internet `cmnet`. |
| Apple IMS manager asks for a PDN | **19513–19520**: `[5wi.ct.1.1]` attempts activation with `kDataContextBB`. Here **BB is the cellular bearer transport**, not evidence that SIP executes in the baseband. |
| Actual initial IPv6 IMS WDS activation succeeds | **20493–20502**: mux 2 IPv4 fails; **21865–21867**: mux 2 IPv6 Start Network succeeds; **22184**: IPv6 runtime settings succeed; **22261** explicitly associates this with APN `IMS`. |
| iOS does not complete a stable IMS interface | **19864–19867** transiently lists `pdp_ip1`; **22301–22302** reports empty IPv6 service information; **34318** stops the IPv6 WDS call; **34658,34927** report `kDADMissing`. A named interface or bridge "active families" bitmap is not proof of completed IP configuration/IMS registration. |
| Later retries have real modem failures too | **187532–187548**: IPv4 verbose type 2/code 208; IPv6 verbose type 6/code **29**, decoded by CommCenter as `kGprsPdpAuthenticationFailure`. **134536** also reports service-option-not-supported. This is PDN establishment, **not a SIP 401 authentication challenge**. |
| No demonstrated IMS registration | Boot logs explicitly say SIM not IMS registered; no positive registration, SIP REGISTER exchange, or usable P-CSCF exchange was found in the filtered search. SIP packet logging may be disabled, so missing SIP text alone proves no wire-level absence. |

V28 contains **13 "Attempting to activate IMS" entries** and **12 activation-error entries**: five `kDADMissing`, six authentication failures, one service-option-not-supported. Entries include overlapping/retried state-machine activity, not necessarily 13 distinct network transactions. V25/26/27 also attempt IMS; V26:31306 and V27:32665 show `kDADMissing`. Earlier traces have known bridge compatibility issues, so V28 is the strongest baseline.

**First practical gate:** preserve the successful IMS IPv6 bearer and complete the actual iOS readiness contract. Determine whether the missing element is Apple's WDS DAD-complete indication, IPv6 runtime-setting interpretation, configd publication, or real IPv6/ND setup. The M command-driver strings explicitly contain `handleWdsDadCompleteInd` and IPv6 configuration timers (`libCommCenterMCommandDrivers.dylib.strings:1611–1624`). Do not simply fabricate DAD success. Then obtain a real interface address/route and P-CSCF/PCO, and separate subsequent network rejects from local teardown/retry effects. Internet IPv6 success does not establish this separate context's correctness.

## 3. Audio: shared CS work versus an AP-media alternative

**For X55 modem-owned VoLTE, the vendor audio architecture is substantially shared with CS.** `work/umi-mic/audio-kernel/dsp/q6voice.c:193–200` accepts both CS and VoLTE sessions; `include/dsp/q6voice.h:2040–2069` defines passive voice/VoLTE and VoiceMMode sessions. `q6voice.c:7334–7344` starts voice-MHI before MVM/CVS session creation. `dsp/voice_mhi.c:104–115,282–320,348–379,455–464` matches `AUDIO_VOICE_0`, maps the mailbox into PCIe and ADSP DMA domains, and gives it to ADSP MVM.

That supports reusing the CS research's **MHI offload channel 80 → shared mailbox → ADSP MVM/CVS/CVP → codec/speaker/mic** work for modem VoLTE. It does not prove identical VSID, vocoder, topology or packet format, or a successful call on HakiOS. It is not a plain PCM stream on the QMI/data bridge. Keep implementation ownership with the existing CS agent.

**On iPhone, AP SIP does not imply AP speech processing.** IPT contains QMI RTP/AUDIO backends with VSID binding and codec/session operations. It also contains a genuine **AP AVC media backend**: `AVCRTPAudioSession` at file+`0x3d085d`, socket-based RTP/RTCP handling, `Create AVC RTP Stack` at `0x3df0fb`, alongside legacy/modem RTP creation in the same function (VM `0x1b7d80650–0x1b7d8178c`).

`UseAPMediaStack` at file+`0x3c490a` is an **Internal** preference: its descriptor initialization at VM `0x1b7bbcfbc–0x1b7bbd054` supplies zero defaults. `IMSSetPreferredMediaStack` and AP/baseband preference constants also exist. This establishes an implementation avenue, **not a verified user-facing carrier switch or a proven cellular configuration on D22**. It may have device, transport, codec and policy constraints; creating a controller does not prove its selection.

| Chosen media route | Work beyond SIP registration | Relationship to GSM |
| --- | --- | --- |
| Apple's existing modem RTP backend | Implement/translate its real RTP/AUDIO session contract on X55; verify codec and packet transport, QoS and ADSP coordination | Can reuse vendor mailbox/audio work, but requires more than standard Voice forwarding. X55's full modem SIP client is not necessarily required for RTP offload. |
| Apple's AP AVC media backend | Prove cellular selection and AMR/AMR-WB encode/decode, RTP/RTCP, packet timing, dedicated-bearer filtering; provide low-latency CoreAudio capture/playback, PhoneCall routing, AEC and interruptions | Could bypass X55 voice mailbox entirely. Existing speaker/mic endpoints help, but ordinary media playback is not a validated telephony pipeline. |
| X55 modem IMS and speech | AP modem-IMS host services plus CommCenter call/state adapter, then vendor voice-MHI/ADSP path | Most direct reuse of Qualcomm Android behavior; largest change in who owns IMS signaling. |

`VOICE-CS-umi.md` remains the audio reference: HakiOS's current HAL lacks a cellular speech endpoint, and its normal eight × 20 ms playback queue is unsuitable as an assumed low-latency speech path. AP media would trade modem reverse engineering for substantial AVC/CoreAudio integration; it is not established as easier overall.

## 4. China Mobile and the GSM horizon

China Mobile's practical CS fallback is GSM; WCDMA is not a CMCC fallback, and retired TD-SCDMA is not a useful plan. GSM spectrum retirement/refarming and reduced legacy coverage make dependence on CSFB progressively less durable. **No verified nationwide GSM shutdown date or location-specific coverage conclusion is available in this offline research.** Remaining 2G service can vary by province, site and subscription; the failed CS attempt does not identify which limitation applies here.

Where this SIM has LTE service but no usable GSM/CSFB, **ordinary cellular calls require working IMS/VoLTE on LTE**. LTE itself supplies no circuit-switched traffic channel. Data, bars, CS-attached flags and working SMS do not replace IMS voice registration. VoNR on a different 5G path does not solve the present LTE port. Thus GSM is a conditional near-term milestone, while VoLTE should be planned as the long-term requirement rather than assuming GSM will remain locally available.

## 5. Ordered work, estimates and decision

Estimates are engineering judgment for one experienced engineer with supervised measurements and local references, excluding operator delays. They are not commitments; private media contracts dominate uncertainty.

1. **Keep the existing agent's cheap GSM network/control gates first** if stock Android can complete non-IMS GSM calls here. Standard Voice Dial already reaches X55; the recorded `NO_NETWORK_FOUND` is a much narrower starting point than an IMS port. Reuse that agent's audio findings. If usable GSM is absent, move VoLTE to first priority immediately.
2. **Resolve the separate IMS bearer, 2–7 working days if the mismatch is local.** Correlate WDS transactions/muxes and original failure enums; compare the working vendor IMS profile/IP family/PCO without copying identities. Require stable `pdp_ip1`, IPv6 configuration, P-CSCF reachability, coexistence with Internet and clean reconnect. Network provisioning/MCFG rejection has no defensible fixed ETA.
3. **Retain Apple SIP for the first IMS experiment, approximately 1–3 additional weeks.** Trace readiness/registration policy, UIM AKA exchange metadata, IPsec SA creation and SIP result codes. Verify custom XNU's IPsec/NetworkExtension interfaces. Implement only observed missing modem contracts. Acceptance: authenticated IMS registration and refresh; neither a true feature flag nor an IMS PDN is sufficient.
4. **Choose media with a bounded 3–5 day investigation.** Identify the selected RTP stack and whether AP AVC can operate for CMCC cellular AMR-WB. If viable, budget **2–6+ weeks** for one bidirectional AP audio route and QoS. Otherwise use the CS agent's voice-MHI/ADSP work and budget **3–8+ weeks** for the RTP/AUDIO adaptation and integration, with shared audio work counted once. A modem-IMS adapter is a fallback architecture, not the default first task.
5. **Integrate and harden, 2–4+ additional weeks.** Require outgoing/incoming ringing, answer/end, remote-confirmed speech both ways, codec/DTMF, hold, mute/volume, data coexistence, bearer loss, airplane-mode/restart recovery, suspend and repeated-call cleanup. Ordinary calls first; emergency behavior is a separate unproven requirement. A production phone also needs validated emergency calling/location, handover and operator compatibility.

A first ordinary VoLTE call is plausibly **6–12+ weeks** if Apple SIP and AP media can be reused; modem media/IMS replacement can push this to **several months**. Stable daily use is a further project. By comparison, after a proven GSM network, standard CS call-control validation is measured in days; the existing CS report estimates **2–6+ weeks for first audio plus 1–3+ weeks for iOS audio integration**. Both paths can be blocked by audio, but VoLTE adds bearer readiness, P-CSCF, AKA/IPsec, SIP, codec/RTP and QoS dependencies.

**Recommendation:** GSM first for the fastest achievable call milestone when local CS service is proven; fix the already-observed IMS IPv6 readiness failure next. Preserve Apple's AP SIP client until evidence demands replacing it. Decide AP versus modem media early, and budget VoLTE as a larger integration effort with a much longer useful network lifetime.

## Implementation — 2026-11-11, IMS bearer/readiness milestone

The supervisor's new Android observation changes the call choice: stock Android with VoLTE disabled cannot place a call at this location, and the X55 CS dial ends with `NO_NETWORK_FOUND`. GSM is therefore not a usable fallback here. This milestone keeps CommCenter and Apple's AP SIP/IMS client as the signaling owner and adds the missing bearer plumbing behind `/var/db/hakios-voice/ims-on` (default off).

### Cause isolated from V28

The initial IMS transaction was locally far enough along to start WDS IPv6 on the second context: CommCenter loaded APN `IMS` as `Settings.1:2`, named `pdp_ip1`, and received successful IPv6 runtime settings. Before this change, `bb_pdp_ioctl` returned success without invoking either XNU protocol owner for `SIOCAIFADDR`/`SIOCAIFADDR_IN6` or applying `SIOCSIFFLAGS`; that left the interface lifecycle incomplete even though the modem later supplied an IPv6 address/RA and CommCenter briefly reached `active`. The empty service snapshot at V28 lines 22301–22302, followed by the local stop and `kDADMissing` at 34658/34927, identifies the missing readiness handoff as the local failure to remove first. It does not justify fabricating a DAD QMI indication. A cellular point-to-point interface does not need an Ethernet RA for this path: address installation and duplicate-address detection remain XNU responsibilities.

The same trace contains independent network failures after the local teardown: IPv4 IMS is refused with 3GPP verbose type 2/code 208, and a later IPv6 retry is refused with verbose type 6/code 29 (`kGprsPdpAuthenticationFailure`). The CMCC profile allows IPv4v6, but this X55/network combination has already demonstrated that its IPv4 IMS half is unusable. The gated request helper changes only an IMS Start Network request's IP-family TLV from IPv4v6 (3) to IPv6 (6); it does not change the APN, credentials or response contents. WDS runtime settings and PCO/P-CSCF TLVs are forwarded unchanged. The helper records P-CSCF presence from the WDS IPv4/v6 P-CSCF fields and the X55's 17-byte IPv6 settings field, without logging addresses.

### Files and behavior

- `devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_ims_protocol.h` contains host-testable QMI TLV parsing, IMS APN recognition, the IPv6-only request rewrite, and P-CSCF-presence detection.
- `devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/umi_ims.inc` samples `/var/db/hakios-voice/ims-on`, calls `in_control`/`in6_control` for address ioctls, applies interface flags, and emits only sanitized `ims:` state/result lines. UIM AKA result metadata and private service numbers are logged without payloads. The bridge logs MAVIMS `0xe6`, ELQM `0xe7`, and AUDIO `0xe1` as unsupported; no one-to-one service-number rewrite is invented because those services are signaling/media assistance contracts, not SIP registration.
- `devices/xiaomi-mi-10/drivers/umi/hakios.umi_baseband.cpp` has the include, switch worker allocation, request/response hooks, and the small PDP ioctl hook. `umi_cellinfo.inc`, `umi_gnss*`, and `umi_voice.inc` remain untouched.
- `devices/xiaomi-mi-10/tools/ims-tests/ims.c` and `tools/test-ims.py` cover the IMS APN IPv6-only rewrite, non-IMS preservation, and P-CSCF marker. Existing modem, voice, GNSS and cell-info tests remain green.

Good lines expected with the gate on are:

```
ims: switch on; IPv6-only IMS bearer and address ioctls enabled
ims: WDS start IMS family 3 -> IPv6; APN and identities redacted
ims: address ioctl 0x... result 0; DAD delegated to XNU
ims: WDS rx msg 0x2d result 0x0 family 6 pcscf present
```

Bad lines remain actionable:

```
ims: private svc 0xe7 msg ... unsupported by bridge/X55
... kDADMissing ...
... WDS ... result 0x... pcscf absent
```

The existing XNU configuration already builds PF_KEY and transport ESP (`DIPSEC` and `DIPSEC_ESP` in `configs/umi.config`; `bsd/netkey` and `bsd/netinet6/esp_*` in `bsd/conf/files`). No kernel IPsec source change was needed for this milestone. Apple SIP still needs a supervised registration trace to prove UIM AKA, PF_KEY SA creation, SIP REGISTER/401 handling and its readiness gates. The bridge deliberately does not kill, patch or stub CommCenter.

### Media decision

Use the modem media path for the first VoLTE call. IPT contains an AP AVC RTP backend, but the local evidence only proves that the code exists; it does not prove that CMCC cellular policy selects it on this D22 personality, that AMR-WB/RTP/RTCP and dedicated-bearer filtering are wired, or that the PhoneCall CoreAudio/AEC route is available. The working audio agent has the stronger device-specific evidence: `AUDIO_VOICE_0` MHI, the shared mailbox in both DMA domains, and ADSP MVM/CVS/CVP voice sessions. The AP AVC route remains possible later, but it would still need `IMSSetPreferredMediaStack` selection, AMR-WB negotiation, RTP timing/QoS, low-latency capture/playback, PhoneCall activation, AEC, interruption and receiver/speaker routing. Ordinary speaker and microphone playback alone does not establish that route.

### Validation and supervisor checklist

`python3 devices/xiaomi-mi-10/tools/test-modem.py`, `test-gnss.py`, `test-cellinfo.py`, and `test-ims.py` pass. `test-voice.py` is currently blocked by the parallel voice-agent edits (`UIO_SYSSPACE`/`vfs_context_*` undeclared and a missing `uva_config.tx_topology` initializer); those files are outside this change. The locked kernel build compiles `hakios.umi_baseband.cpp` and the new IMS translation unit, then stops in the parallel microphone/audio edits (`umi_mic.c:1683` has `continue` outside a loop and `umi_voice_audio_receiver.inc` is missing). No `kernel-ims` artifact or SHA-256 is reported.

On-phone, without using a new boot argument:

1. Boot once with `/var/db/hakios-voice/ims-on` absent. Confirm LTE data and SMS still work and no `ims: switch on` appears.
2. Create the regular file, toggle the cellular data context or reboot under supervision, and confirm `switch on`, a stable `pdp_ip1` IPv6 address, successful address ioctls, and a P-CSCF-present runtime-settings line. Do not print identities, addresses, SIP identities, numbers or keys.
3. Confirm there is no repeated `kDADMissing`, no 360-second IMS reset, and no baseband reset loop. Capture sanitized SIP state/result and status lines only: AKA result, IPsec SA result, REGISTER status (401 challenge then 200), refresh and teardown.
4. With registration stable, place and receive one ordinary VoLTE call. The audio acceptance belongs to the modem-media work: two-way speech, receiver/speaker selection, mic mute, volume, remote/local hangup, media interruption/resume and repeated-call teardown. Remove `ims-on` and verify that ordinary data, SMS and media return to the prior behavior.

## Media architecture and call path (v29)

### 结论与当前交付边界

**D22/Qualcomm 默认是 Apple AP SIP + modem RTP/codec + modem/ADSP 音频。AP SIP 并不意味着 AP 负责 RTP 或编解码。** 原报告把 `AUDIO_VOICE_0 + MVM/CVS/CVP` 当成完整媒体方案，漏掉了 SIP/SDP 与 modem RTP session 的绑定。被动语音音频接口只解决后半段；当前代码仍未实现能接打电话的完整媒体链路。

本次实现的是 IMS 前置条件修正、独立 IMSA 绑定/查询状态机、真实媒体服务分配结果日志，以及恢复原生 AVC daemon job。**没有完成 RTP 翻译、AP AVC 的 cellular 选择、呼入到通话界面的端到端验证，也没有声称已经禁止 firmware 自主注册。** 这些是剩余代码工作，不能归为仅待真机验收。

### 反汇编证据

以下均为 iOS 16.5 / 20F66 的 unslid VM 地址。研究记录位于工作区外层 `work/ims2/`，可用该目录的 `dis.py` 对照本地原始二进制复现。

| 二进制 | UUID |
| --- | --- |
| `libIPTelephony.dylib` | `9C10AEFF-16A2-3A0B-9346-E717AD6E55F9` |
| `libQMIParserDynamic.dylib` | `A8747511-D50F-3670-ADD3-0360DB74A9E1` |
| `CommCenter` | `5A907E0E-BF03-376B-94E6-E737BDD40300` |

1. `libTelephonyUtilDynamic` 的设备表构造 `0x1937a3368`、D22 项 `0x1937a35a4`，匹配分支 `0x1937a4424 → 0x1937a5240`，在 `0x1937a5250` 选择 radio 5 / vendor 1（Qualcomm）。IPT helper `0x1b7c462ec` 对该组合返回 false。
2. IPT RTP factory `0x1b7d80650–0x1b7d8178c`：`0x1b7d80b18` 调用上述 helper，false 分支到 `0x1b7d80e10`，`0x1b7d80e48` 引用 `Create legacy QMI RTP Stack`。legacy controller ctor `0x1b7b9474c` 在 `0x1b7b948bc` 创建 driver `0x1b7d68de4`，使用 RTP **`0x28`** 和 AUDIO **`0xe1`**。
3. 另一条 `QMIRTPStackController` ctor `0x1b7d270e4 → 0x1b7bdfa84` 使用 RTP **`0x52`**，session initialize 的消息号为 **`0x41`**（`0x1b7be135c`）。`0x1b7be01ec` 是 `voiceSystemIdRetrieved` 回调，不是 legacy driver 构造函数；不能据此把 `0x52` 叫做 legacy 服务。
4. `libQMIParserDynamic` 的服务名称映射在 `0x1e9a719d0–0x1e9a71a1c`：MAVIMS=`0xe6`、ELQM=`0xe7`。IPT 的 MAVIMS 使用者是 `LegacyQMISipLogger`。requested、ringing、confirmed、termination 分别在 `0x1b7b69380`、`0x1b7b6a928`、`0x1b7b6b238`、`0x1b7b6bab4` 构造 **message `1`**，由 TLV 区分事件。incoming 的 response handler 在 `0x1b7b69d28` 要求 TLV `0x12` 的 call ID，空成功响应不满足契约。logger confirmed 事件也不能证明 RTP 或 ADSP 已启动。

Legacy RTP 的消息构造调用如下；公共 Qualcomm `ip_multimedia_subsystem_rtp_v01.c` service object 也明确为 `0x28`，消息范围与 Apple legacy backend 一致。因此 `0x28` 本身并非全部属于 Apple 私有协议，关键缺口是本机 X55 尚未发布可用端点。

| RTP `0x28` 操作 | Message ID | IPT 构造点 |
| --- | --- | --- |
| Initialize stack / Uninitialize stack | `0x20` / `0x21` | 公共 Qualcomm IDL；Apple 字段兼容性尚未逐项完成 |
| Session initialize | `0x22` | `0x1b7d6f8ec` |
| Session uninitialize | `0x23` | `0x1b7d712c8` |
| Session configure | `0x24`（公共 IDL） | configure 函数 `0x1b7d7179c`；serializer 尚未逐项核对 |
| Start / Stop | `0x25` / `0x26` | `0x1b7d71da8` / `0x1b7d72348` |
| Pause / Resume | `0x27` / `0x28` | `0x1b7d728e8` / `0x1b7d72e30` |
| RTCP report | `0x29` | `0x1b7d73404` |
| RTP / RTCP monitor | `0x2a` / `0x2b` | `0x1b7d73dd4` / `0x1b7d74378` |

历史 X55 NEW_SERVER 列表未见 `0x28` 或 `0x52`。桥在它们缺席时于 CTL GetClientID 阶段返回真实 `INVALID_SERVICE_TYPE=31`，根本不会收到 session initialize。新增日志覆盖这一层；若后续 firmware 真正发布端点，已有标准 QMUX 转发可发送请求，但 session 字段和音频绑定仍需实现/核验。IMSA `0x21` 是状态服务，IMSS `0x12` 是配置服务；没有证据支持把 RTP 改号后送给它们。也没有把 RTP 改成 VOICE Dial，后者会启动 modem 自己的呼叫/SIP 控制。

进一步检查本地 `work/modem-fw/qdsp6sw.mbn`：文件偏移 **`0x284e7b4`** / ELF VA **`0xc91fc7b4`** 确有 RTP `0x28` 的 QMI IDL service object（tool 6、major 1、minor `0x25`、max message 8204、34 commands / 34 responses / 20 indications）。command table VA `0xc519e098` 包含 `0x20–0x31` 及后续标准 RTP 操作，`0x22` session init 的最大长度为 `0x824`。这把缺口进一步缩小为**服务如何在外部 AP transport 发布/启用**，不能断言固件没有 RTP 实现。已对 executable LOAD 段补 section headers 作本地 Hexagon 反汇编；尚未定位可用的服务启动命令，未 patch 固件、改 NV 或硬编码猜测的 QRTR port。未读取 EFS/calibration 数据。

### AP AVC 路径的实际选择条件

IPT 确实有 AP 实现。`AVCRTPAudioSession::initialize` 在 `0x1b7c87630–0x1b7c88918` 创建 RTP/RTCP sockets，并在 `0x1b7c8857c` 调用 `AVCAudioStream initWithNetworkSockets:isOriginator:callID:error:`，保留真实 NSError。AVConference 中 AMR-WB payload/mode 处理为 `0x1cb82faf0`、`0x1cb82fdb8`，EVS decoder 的 AudioConverter 属性设置为 `0x1cb872ba4`、`0x1cb8730ac`，EVS AMR-WB decoder 选择为 `0x1cb898668`。这是实际软件媒体代码，尚不证明 cellular 策略已选择它。

`UseAPMediaStack` descriptor `0x1b7bbcfbc` 属于 **Internal** section，默认 0。override plist reader `0x1b7a874d8` 受 `os_variant_has_internal_content` 限制；carrier/prefs parser `0x1b7d64024–0x1b7d641a4` 也检查 Internal gate，并回溯 descriptor 的 parent section（`0x1b7d63cd0 → 0x1b7d63ed0`）。改扁平 key 或换 section 不会自动绕过。没有写入一个无效 plist 然后声称 AP 已启用。

`IMSSetPreferredMediaStack` 为 `0x1b7d04a60`，CommCenter wrapper 为 `0x100178810`，使用 CommCenter 自己的 `ClientHandle`（receiver `+0x88`）。这是进程内请求入口，独立 helper 的 handle 不能直接控制 CommCenter 的 stack。stack config `0x1b7c5ae18` 还检查 NetworkConfig 内嵌 network `+0x110` 字段；其上游语义尚未追实，未伪称某个 carrier key 已控制它。

即使选 AP，仍会通过 AUDIO `0xe1` 发送 **message `0x38`** `sendNotifyIpVersionReq`，serializer `0x1b7c750a4–0x1b7c750d0` 的 TLV `0x01` 为两个 u32（IP version 在前、context 在后）。现有桥对此返回 unsupported。恢复 `avconferenced` job 只补齐一个依赖，不能替代 stack selection 或该 AUDIO 契约。

`overlays/userland/mkjobs.py` 已允许原生 `com.apple.avconferenced` 经 `--rest` 按需加载；生成结果保留 `/usr/libexec/avconferenced`、mobile 用户、原生 MachServices 及 `AVConference/UseAvconferenced` feature gate，不改签名或 entitlement。当前仅验证 job 生成，未重打 userland 包。

### 呼叫控制与注册前置条件

CommCenter 的原生 IMS 呼叫接口已经存在，无需新增 VOICE Dial 适配：

| 动作 | CommCenter wrapper → IPT export |
| --- | --- |
| 拨出 | `0x100178600 → IMSStartCall 0x1b7d03638` |
| 接听 | `0x100178648 → IMSAnswerCall 0x1b7d03d84` |
| 挂断 | `0x100178728 → IMSEndCall 0x1b7d03980` |

这些 wrapper 同样从 `+0x88` 取自己的 ClientHandle。AP 收到 SIP INVITE 后的完整 callback → CommCenter → 系统通话界面尚未完成追踪/验收。IPT 提供 Incoming、Ringing、Active、MediaActive、Disconnected 状态；不把 X55 VOICE 来电 indication 当作 Apple AP SIP 的来电来源。

| 前置条件 | v29 实际状态 |
| --- | --- |
| IMS PDN | `ims-on` 下仅 IMS IPv4v6 Start 请求改为 IPv6；WDS 生命周期按 client/channel 保存；地址 ioctl 调用 XNU owner 并返回真实错误 |
| P-CSCF | WDS `0x23` IPv4、`0x24` domain、**`0x2e` IPv6** 按实际 schema 判断非空列表；`0x22` discovery flag 和 `0x25` 本机地址均不算；响应原样交 Apple parser |
| AKA | UIM Authenticate **`0x34`** 原样转发，记录方向/type/result，不输出 payload；未实现或宣称 Apple 私有 AKA 翻译 |
| IPsec | 内核已有 PF_KEY/ESP 配置和源码；真实 SA 安装和 REGISTER 401→200 尚未取得证据 |
| CMCC bundle | `CMCC_cn 54.0.1 / overrides_D20_D21_D22.plist` 已有 `SupportsImsCapability=true`、`ShowVolteSwitch=true`、`IMSConfig/Voice/EnableVolteByDefault=true`、`Media/PreferredAMRWBMode=8`；未用 Internal carrier override 冒充 AP 选择 |

更正前文：17-byte WDS `0x25` 只能证明本机 IPv6 配置，不能证明 P-CSCF。Qualcomm 公共 `wireless_data_service_v01.c` 的 runtime settings descriptor 明确将 IPv6 P-CSCF 列表放在 `0x2e`（u8 count + 每项 16 bytes）。

### 注册所有权与开关

唯一产品开关仍是常规文件 `/var/db/hakios-voice/ims-on`，默认关闭，无新增 boot-args。开启后拦截 host IMSS `0x12` 配置和 IMSA `0x21` 非查询命令，**不启用 modem IMS**。诊断使用独立 QRTR local port `0x4203`，先发 IMSA **BindSubscription `0x33`**（TLV `0x10`、u32 primary subscription 0），成功后查询 **GetRegistration `0x20`**。这只绑定该诊断 client，不更改注册配置。

绑定的必要性有本地 SDX55 参考实证：`/Users/xm666/X/mainline/umi-linux/work/modem-data/refs/modem-userspace/README.md` 的 IMS 节说明，未绑定的新 client 返回 InvalidOperation，甚至 modem 已注册时也如此。公共 IMSA IDL 确认 `0x33`/TLV `0x10` 的 u32 enum schema。

事务须同时匹配 node/port、transaction、message；5 秒超时后清除绑定，5 秒后重试，成功查询每 10 秒一次。只有已绑定且最近 15 秒真实查询为未注册，才允许新的 AP IMS Start；发出下一次查询不会延长旧结果的有效期。服务撤销、端点变更和开关关闭均使 audit 失效/释放。

**该门控尚不足以保证整个通话期间绝无双注册**：尚未禁止 firmware 自主注册，也未在 modem 后来注册时撤销已存在的 AP bearer。未发送可能落入持久配置的 IMSS `ims_test_mode_enabled`，未写 NV/EFS/persist/modemst。因此不能将 `ims-on` 描述为“已经关闭 modem IMS”；当前只保证桥不发起其注册，并按独立查询门控新的 AP bearer。

### 音频接口与剩余实现

保留 D22 原生 modem RTP 为架构结论；没有可工作的 RTP 端点/翻译前不启动被动音频。`umi_voice_audio*` 和其他音频 agent 文件未修改。需要在真正完成 session init/config/start、取得绑定 VSID 且 call connected 后调用：

```cpp
umi_voice_audio_start_locked(key, UMI_VOICE_LTE, UMI_VOICE_RECEIVER);
// 在 bb_lock 外读 umi_voice_audio_status(key)：0 才代表音频就绪；36 为进行中。
umi_voice_audio_stop_locked(key);
// mute/volume/route 同样在 bb_lock 外，使用现有接口。
```

当前接口以现有 LTE VSID 为前提。如果 RTP 协商返回其他 VSID，后续需要向音频接口传入显式 u32 VSID，并将同一 VSID 绑定到 RTP、MVM/CVS session；不能只用 logger call ID 推导。v29 没有添加无可达成功条件的 start/stop 调用，也没有新增参数。

### 测试、构建与 supervisor 验收

本次 `test-ims.py`（C 协议 + 实际 include `umi_ims.inc` 的 C++ bridge，ASan/UBSan）、`test-modem.py`、`test-gnss.py`、`test-cellinfo.py`、`test-voice-audio.py`（含 `test-voice.py`）、`prepare.py verify` 均通过。IMS 新覆盖 IPv6 P-CSCF、绑定成功/失败、错 message/txn/source、超时后重绑定、结果过期、端点变更、真实媒体 CTL 分配拒绝/成功、地址 ioctl 错误透传、payload canary 不进日志。host 测试没有验证实际 SIP/RTP 或音频内容。

通过 `/Users/xm666/X/umi-ios/build-lock.sh` 执行 `BUILD_DSYM=0 bash devices/xiaomi-mi-10/build-kernel.sh -j8` 成功，同一锁内复制冻结为：

```text
work/umi-v23/out/xiaomi-mi-10/flash/kernel-ims2
SHA256 a99bf046defedcdb455f331776c3358b92b633228881a747043660ffa47fe259
构建日志 work/ims2/build-kernel-v29.log
```

未访问手机。以下步骤由 supervisor 执行；它们用于定位当前缺口，不代表本内核已具备接通能力：

1. 不存在 `ims-on` 时确认 LTE 数据/SMS 基线。开启后看 `ims: modem IMS audit bind result 0; registration unchanged`，随后应为 `ims: modem IMS audit result 0 registration 0; AP bearer allowed`。失败/非零时停止本次 AP 注册实验，不能绕过门控。
2. 检查真实地址 ioctl result、稳定 `pdp_ip1`、WDS family 6 和 `pcscf-list 1`，不输出地址或身份。若只有 `0x25` 而没有 `0x2e/0x23/0x24`，继续处理 PCO/P-CSCF 获取，不能宣布注册前置条件齐全。
3. 从 Apple 原生诊断提取**仅状态/结果**的 REGISTER 401 challenge → AKA result → PF_KEY SA result → REGISTER 200，及 INVITE/180/200、BYE/200。桥只看 QMI metadata，无法从 ESP 中生成 SIP 状态日志；v29 没有新增这些 SIP 日志。不得提交 raw SIP/pcap、身份、号码、AKA 响应或密钥。
4. 首次媒体选择看 `ims: media client svc 0x28 allocation result 1 error 31`（当前预期阻塞），或真实成功的 `allocation result 0 error 0` 后 `ims: RTP tx/rx svc ... msg ... result ...`。私有 AUDIO 请求仍明确输出 unsupported。**client 分配成功不等于 session/media 启动成功**。
5. 完成上述剩余媒体实现后，才验收真实 session start result 和 `umi_voice_audio_status=0`，再由双方人工确认双向语音、接听/拒接/挂断、静音/音量/路由、连续两次通话及释放；不记录音频。当前没有可供验收的成功媒体启动日志，不能把它的缺失归咎于真机测试未做。

## v29f — 合成 WDS "DAD complete"（0x5561）指示

日期 2026-11-12。完整诊断见 `work/v29e/INVESTIGATION-ims-dad.md`（含 "Follow-up: message id"）：CommCenter 的 QMIDataContextIP 在 IPv6-only IMS 上下文里等一条 Apple 私有 WDS 指示——service `0x01`、message `0x5561`、type 4（indication）、txn 0，TLV `0x01` = 16 字节 IPv6 地址 || 1 字节 prefix_len，共 17 字节；X55 从不发送，`fIPv6ConfigTimer` 10 s 到期 → 主动 Stop → 模型 state 2 下的断开被映射为 `kDADMissing`（CSIError 93）。

### 合成了什么

- 仅在 `ims_enabled`（`/var/db/hakios-voice/ims-on`）时合成；开关关闭时日志与线上字节不变（新增的状态标志不可见）。
- 目标 client 不写死：在 `bb_pdp_clients[i]` 里找 IMS APN 已选中（`ims_owner[i].bearer.selected`）、family 6、active、runtime settings 带 TLV `0x25`，且 `mux == unit + 1` 的 slot；用同一 slot 的 `bb_qmux.client[i].cid/.channel`，经 `bb_push(channel ? CBH_QMUX2 : CBH_QMUX1, …)` 发出。
- payload 原样等于 `cbp_observe()` 存下的 TLV `0x25`（`struct cbp_client` 新增 `v6cfg[17]`）；全零和 link-local（fe80::/10）丢弃，与 CommCenter 的 "ignoring Link Local DAD" 一致。地址绝不写日志。
- 触发条件：TLV `0x25`（runtime-settings `0x2d` 应答）已知 **且** XNU 地址 ioctl 已在对应 pdp_ip 上成功（`ims: address ioctl … result 0`）。任一条件后到时由下一次 modem 线程轮询补齐；两者齐备后经 thread_call（`thread_call_enter_delayed`，~1 s 标准 DAD 时间）到点，在 `bb_lock` 下重新校验 client 仍 active、地址未变才发送。每次激活最多一条：Start 请求开启新激活，Stop/断开置 closed，直到下一个 Start 才允许再次发送。地址 ioctl 标志在开关关闭时也记录（无外部可见效果），因此开机早期装好地址、之后才出现 ims-on 的情况也能触发。
- 未修改 XNU nd6 代码，未 patch/stub/杀 CommCenter，无新增 boot-arg。改动文件：`umi_ims.inc`、`coachz_bb_pdp.h`、`hakios.umi_baseband.cpp`（仅 `bb_pdp_note` 的观察入口、`bb_pdp_ioctl` 的地址 ioctl 记录、thread_call 分配、`umi_bb_poll` 的一次调用）与 IMS 主机测试。

### 预期日志

好（唯一一条，合成成功）：
```
ims: DAD complete indication cid <n> mux <m> prefix <len> sent
```
可诊断（未发，状态在 1 s 内变化）：
```
ims: DAD complete indication cid <n> mux <m> not sent: state changed
```
原有的 `ims: address ioctl 0x… result 0; DAD delegated to XNU`、`ims: WDS rx msg 0x2d …` 仍照常出现。

### 测试

`python3 devices/xiaomi-mi-10/tools/test-ims.py`（协议测试 + 真实 include `umi_ims.inc` 的 C++ bridge，ASan/UBSan）新增覆盖：ims on、Start 应答后 0x2d 携带 TLV `0x25` 全局地址 → 恰好一条 0x5561/TLV `0x01`（17 字节与 TLV `0x25` 相同）发往正确的 cid/channel，重复 tick 不再发；ims off → 无；link-local → 无；定时器到期前 Stop → 无。`test-modem.py`、`test-voice-audio.py` 同次通过。内核构建仍由 supervisor 经 `build-lock.sh` 执行。

### 下一个预期阻塞

指示送达后，IMS 上下文应保持 active 越过 10 s（不再 `kDADMissing`）。之后剩余门槛是注册链路：SIP REGISTER（401 挑战→AKA→200）、IPsec（PF_KEY/ESP 已编入内核，真实 SA 安装尚未验证）、UIM AKA（bridge 只原样转发 UIM `0x34` 并记录方向/类型/结果，未实现 Apple 私有 AKA 翻译）。

## v29g — arm 条件修正：去掉 ioctl 与请求侧 APN 依赖

日期 2026-11-12。依据 `work/v29f/INVESTIGATION-ims-dad2.md`：v29f 的两条门槛在真机上都不可能成立。(A) XNU 只在接口的**第一个**地址安装时调 `SIOCSIFADDR`（`in6_ifinit()` 的 `IN6_IFAUPDATE_1STADDR`），pdp_ip1 从来没有这个回调，`ims_dad_ioctl_ok[1]` 永远为 false；(B) CommCenter 的 Start Network 请求里没有可识别的 APN 字符串 TLV（SDU len 19 = 12 字节 TLV），`umi_ims_apn()` 永不匹配，`bearer.selected/family` 恒为 0。数据面本身都满足：Start 应答 → 295 ms 后 0x2d 应答（含 TLV `0x25` 全局地址）→ `cbp_observe()` 记为 `family 6 / active 1 / settings&2`。

### 改动

- `umi_ims.inc`：DAD 状态改为 **per WDS client slot**（`struct ims_dad_state ims_dad[CBQ_MAX_CLIENTS]`），arm 条件只有
  `ims_enabled && WDS client active && family 6 && settings&2 && 本次激活的 0x2d 应答含全局 TLV 0x25`。
  - 不再要求地址 ioctl，也不再读 `ims_owner[slot].bearer.selected/family`。`ims_address_installed()`/`ims_dad_ioctl_ok[]` 仍保留，只作为 `DAD not armed` 行的诊断字段。
  - 每次激活每 slot 只尝试一次：0x2d 应答后约 1 s（`thread_call_enter_delayed` + 每次 modem 线程轮询的兜底），到点后在 `bb_lock` 下重查 client 仍 active、地址未变才发送；失败也标记已尝试，不再重发。
  - Start 请求开启新激活（清 armed/sent/closed/cfg_seen/noted），Stop 或 STATUS 断开关闭；客户端消失时轮询清除状态。
- IMS 判别：报告 §7.2 的三个候选都**不可靠**——本次 IMS 的 0x2d 应答只有 TLV `0x25`（没有 `0x2e/0x24` P-CSCF 字段），Start 请求多出的那个 1 字节 TLV 类型未知，0x2d 请求的 settings mask 未证实。因此按报告允许的兜底：**对所有 eligible 的 IPv6 上下文都发** 0x5561。CommCenter 的 `handleWdsDadCompleteInd` 只在模型 state==2 时处理，否则打印 `"Received DAD complete indication but the model is %s - Bailing out"`（v29e §1.3、Follow-up §3 的 `cmp w20, #2`），互联网 IPv4v6 上下文不会进入该路径。
- 日志（都不含地址）：
  - `ims: DAD complete indication cid N mux M prefix P sent` / `... prefix P not sent: state changed|no room`，每次激活最多一条。
  - `ims: DAD not armed cid N mux M: family .. active .. settings .. v6 .. ioctl .. closed .. worker ..`，0x2d 应答到达但无法 arm 时每次激活一条（去重）。
  - `ims: WDS start cid N fields 0x.. /0x..`，每次激活（按 Start txn）一条，只打印 TLV 类型/长度，用于下次确定 IMS 判别 TLV。
  - `ims: WDS settings field 0x.. bytes ..` 移出 `b->selected` 门（此前真机从未打印），仍然只打印类型/长度。
- `hakios.umi_baseband.cpp`：只更新地址 ioctl 处的注释（记录改为诊断用途），行为不变。`coachz_bb_pdp.h` 未改（`v6cfg[17]`、`settings&2` 已足够）。

### 测试

`python3 devices/xiaomi-mi-10/tools/test-ims.py`（协议 + 真实 include `umi_ims.inc` 的 C++ bridge，ASan/UBSan）新增/改写：
无任何地址 ioctl 仍 arm；无 APN/请求侧 bearer（真实 CommCenter 请求形状）仍 arm；0x2d 后恰好一条 0x5561/TLV 0x01（17 字节 = TLV 0x25）发往正确 cid/channel；重复 tick / 重复 0x2d 不再发；Stop+新 Start 为新激活可再发；ims off 不发；link-local 不发且 `DAD not armed` 只一条；0x2d 无 TLV 0x25 不 arm 且 ioctl 仅作诊断字段；定时器到期前 teardown 不发；settings schema 行不再被 APN 门遮挡。`test-modem.py`、`test-voice-audio.py` 同次通过。内核构建仍由 supervisor 执行。

### 风险 / 未实测

1. 互联网 PDN 也会收到 0x5561（其 0x2d 同样带全局 TLV 0x25，mux 1）。handler 在模型非 state 2 时丢弃，且互联网是 IPv4v6、`fIPv6DadRequired=false`；但“无害”仍未经真机验证。
2. 0x2d 应答不带 Start 的 txn，无法与激活严格关联；一个极晚的旧 0x2d 可能为新激活填上地址（概率低，且地址变化会在 1 s 到点时被 `cbp_v6_same` 拦下）。
3. 指示的 handler 是否还需其它 TLV 仍未验证（Follow-up §7.2）；若 CommCenter 仍报 no match，先核对发送地址/前缀与 TLV 0x25 完全一致。
4. 一次激活只尝试一次：若 0x2d 后地址在同一激活内变化，只会在 1 s 到点时以最新地址发一次，失败不再重试。

预期闭环：IMS 上下文在 0x2d 应答后约 1 s 收到 0x5561，模型离开 state 2，越过 10 s 不再 `kDADMissing`。之后剩余门槛仍是注册链路（SIP REGISTER/401→AKA→200、IPsec SA、UIM AKA 原样转发）。

## v29h — 多候选地址 + 非零 txn + 0x2e IMS 判别（仅代码与主机测试，未构建/未上机）

日期 2026-11-12。依据 `work/v29g/INVESTIGATION-ims-dad3.md`：0x5561 的时序、线上字节、cid/通道、payload 抄写都已排除，剩下两类原因中 **(B) 地址不等** 的未证实点是 `fIPv6ConfigAddress [ctx+0x148]` 到底来自 0x2d 应答的哪条 TLV（0x25 还是同样 16 字节的 0x27/0x28）；同时该报告指出 X55 真实 WDS 指示的 **txn 是 1,2,3…（2 字节）**，而合成帧是 0，是唯一确认的线上差异。`checkIPv6ConfigTimer` 的匹配是“自己的地址表里任意一项等于就取消定时器”，因此多发几条候选地址无害。

### 改动（`overlays/xnu/files/bsd/dev/umi_ims.inc`）

1. **候选地址收集**：`ims_dad_capture()` 在同一次激活的 0x2d 应答里取
   TLV `0x25`（必须 17 字节：16 地址 + 1 前缀）、TLV `0x27` / `0x28`（16 或 17 字节；没有前缀字节时借用 0x25 的前缀，否则 64）。
   按 16 字节地址去重，丢弃全零与 link-local（fe80::/10，复用 `cbp_v6_global()`），最多 3 条。
   记录 0x25/0x27/0x28 是否出现过（只用于日志计数，不打印值）。
2. **每条候选一条 0x5561**：到点后在 `bb_lock` 下重查 client 仍 active、family 6、候选集与 arm 时快照一致，然后对每条候选发一帧。
   txn 用静态 2 字节计数器 `ims_dad_next_txn()`：从 1 开始、每条指示 +1、回绕过 0（0 保留）。
   帧的其余部分与之前完全相同（同一个 `cbq_private_message()`/`cbq_frame_header()`，只有 txn 从 0 变成非零）。
   arm 快照改为 `cfg[3][17] + cfg_n`，比较包含前缀字节。
3. **IMS 判别**：0x2d 应答里 TLV `0x2e` 长度为 1 → 互联网上下文，**不发**；长度 > 1（P-CSCF 列表）→ IMS，发；
   **完全没有 0x2e** → 沿用 v29g 的兜底行为（发），并在日志里注明。
   判别在到点时判定，因此互联网上下文也能得到一条明确原因的日志，且仍遵守“每次激活一次”。
4. **日志**：每次激活一条（原来 `prefix P` 的行被替换）：
   `ims: DAD complete indication cid N mux M candidates K (tlv 0x25/0x27/0x28 present: a/b/c) txn T..U sent`
   或 `... txn 0..0 not sent: internet context (0x2e len <= 1) | state changed | no room`；
   0x2e 缺失而发送时末尾加 `(no 0x2e field: fallback send)`。绝不打印地址。
   `ims: DAD not armed …` 诊断行保留（0x2d 应答但无法 arm 时每次激活一条）。
5. `coachz_bb_pdp.h` 未改（`v6cfg`/`settings&2` 仍由 `cbp_observe()` 维护，v29h 的候选从原始 SDU 另行解析）。

改动文件只有 `umi_ims.inc`、`tools/ims-tests/bridge.cpp`、`tools/test-ims.py`（说明文字）与本文件。未构建内核，未访问手机。

### 测试

`python3 devices/xiaomi-mi-10/tools/test-ims.py`（协议 + 真实 include `umi_ims.inc` 的 C++ bridge，ASan/UBSan）新增/更新：
- 0x2d（0x25 + 0x27 + 0x28 且 0x28 与 0x27 同址 + 0x2e 长度 33）→ 恰好 2 条（去重后）0x5561，
  地址/前缀分别为 0x25(64) 与 0x27(借用 64)，txn 非零且严格 +1；
- 互联网形状（同样地址字段 + 0x2e 长度 1）→ 0 条，且日志恰好一条 `not sent: internet context (0x2e len <= 1)`；
- 0x2e 缺失 → 发送，日志带 `fallback send`；
- 仅 link-local → 0 条；ims off → 0 条；每次激活仍只一条（重复 0x2d 不重发、不再打第二条日志）；
- Stop → 新 Start 为新激活，txn 继续递增；`test-modem.py`、`test-voice-audio.py` 同次通过。

### 风险 / 未实测

1. txn 非零是照 X55 真实指示改的，Apple 的 QMI 派发层是否完全不看指示 txn 仍未动态验证（报告 §8.5）。
2. 三条候选在同一时刻（1 s 到点）背靠背发出，没有按报告建议的 ~100 ms 间隔；handler 匹配第一条就会取消定时器，多余的无害，但若运输层对同 client 突发敏感则未验证。
3. 若 `fIPv6ConfigAddress` 既不是 0x25 也不是 0x27/0x28（来自 Start 应答、0x8c 或 connected 0x22），本次改动仍不会命中；报告 §7 第三步（打印 0x8c/0x22/0x20 的 TLV 类型/长度）未做。
4. 互联网上下文现在也会 arm 一条定时器（只为一秒后打一行“不发”的原因），不发送任何帧；若 0x2e 缺失则按兜底仍会发送（与原行为一致）。
5. 单次激活仍只尝试一次；0x2d 后候选集变化会重排到最新集合，但同一激活内不会第二次发送。
