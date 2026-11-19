# Circuit-switched voice on umi, without IMS

Research date: 2026-11-11. **Conditionally feasible, but neither a connected CS call nor call audio is demonstrated.** CommCenter already issues a standard QMI Voice dial through this bridge; the recorded attempt fails with `QMI_ERR_NO_NETWORK_FOUND`. The first gate is usable GSM/CSFB service for this SIM/location. The second is audio: Android has a concrete X55–ADSP PCIe mailbox path that HakiOS does not implement. Avoiding IMS removes the IMS translation project; it does not remove telephony audio integration.

Research only: no builds, source changes, phone commands, or phone access. This report is the only created file. No subscriber identifiers, dialed numbers, or message contents are reproduced. `/Users/xm666/X/umi-backup` was unavailable; no backup contents were read. Existing audio configuration copies under `work/umi-mic` were used. The requested `work/umi-v23/MODEM-MAINLINE.md` is absent; its existing counterpart is `work/umi-v23/devices/xiaomi-mi-10/MODEM-MAINLINE.md`.

Evidence notation: paths are relative to the repository unless absolute. **V28** = `logs/test-1111-172537-umi-v28.kernel.log` (the requested latest log exists); **V25** = `logs/test-1111-125250-umi-v25.kernel.log`. Numbers after a colon are original line numbers. **B** = `work/umi-v23/devices/xiaomi-mi-10/drivers/umi/hakios.umi_baseband.cpp`; **Q** = `work/umi-v23/devices/xiaomi-mi-10/overlays/xnu/files/bsd/dev/coachz_bb_qmux.h`; **A** = `work/umi-mic/audio-kernel`. Binary `file+0x…` references are offsets in the named extracted Mach-O, not runtime addresses.

## 1. Network: GSM is the only relevant CS fallback

For China Mobile, the proposed path is **LTE → GSM circuit-switched voice**. CMCC has no WCDMA network to fall back to; retired TD-SCDMA is not a practical alternative. X55 is a multimode modem, and our local evidence reports GSM capability: **V25:126799** lists supported GSM bands, while **V25:126850** reports an empty supported TD-SCDMA mask. Configured/active masks are broader than supported masks (**V25:126868,126884**), so an “active band” setting alone proves neither radio support nor camping. GSM support makes CSFB plausible; it does not establish that the loaded firmware/carrier configuration and this network complete it.

**Whether this SIM can still make a GSM CS call here is unknown.** There is no successful GSM registration/call trace or current operator confirmation in the inspected material. A nationwide “2G remains” assertion would not establish local coverage, subscriber voice entitlement, or working LTE-to-MSC interworking. Do not promise a 2G workaround until the supervisor demonstrates it on stock Android with this SIM and location.

| Observation | Evidence | What it establishes |
|---|---|---|
| X55 advertises Voice service 9, instance `0x2`, node 3, port 46 | **V28:2137** | A real QMI Voice endpoint exists. |
| NAS indications arrive as service 3, `0x24` and `0x4e` | **V28:18265–18266** | Serving-system/system-info updates reach CommCenter. |
| CommCenter changes CS attached from false to true, PS/data attached to true, then registers home on LTE | **V28:18272–18277,18337** | iOS interprets the updates as CS-capable LTE registration. It does not prove a GSM traffic channel or completed CSFB. |
| Earlier log explicitly says CS Registered true / PS Registered true on LTE | **V25:131039,131042** | Consistent registration-model evidence, with the same limitation. |
| IMS voice support on LTE becomes supported | **V28:18269,18274–18276** | A network capability flag, not IMS registration or a successful voice bearer. |
| SMS service ready while IMS is not registered | **V25:131041–131043** | Compatible with non-IMS SMS. Actual received SMS is established by the task context; these lines alone are readiness evidence. |

Word-boundary searches of V25/V28 found no explicit `SGs`, `CSFB`, combined EPS/IMSI attach, or voice-domain-preference decode. SMS reception through WMS does **not** identify its radio transport. Even confirmed SMS-over-SGs would not by itself prove local GSM voice/CSFB: an SMS-only registration/service can work while voice fallback does not.

The missing measurements are raw NAS serving-system CS/PS attach states, per-RAT system-info service domain/capability, current mode/band and voice-domain preferences, and the LTE EMM attach/TAU result. Establish **combined EPS/IMSI registration and whether it is SMS-only**, plus a CSFB extended-service request and GSM redirection/reselection outcome. QMI's CS-attached presentation is insufficient to reconstruct those air-interface exchanges. Read current NAS System Selection Preference (`0x0034`, with corresponding set operation `0x0033`) rather than guessing that CommCenter chose a CS-friendly preference.

## 2. Call control: standard Voice is already exercised

The strongest evidence is the existing outgoing attempt, not the mere presence of voice-related strings:

| Stage | Evidence |
|---|---|
| iOS regards the subscription as registered home and allows CS calls | **V25:187955,188026** (`OperatorBlocksCallsOverCS: false`) |
| CommCenter sends service `0x09`, request `0x0020` | **V25:188085** — standard Voice Dial Call |
| A service `0x09`, `0x002e` indication is delivered | **V25:188122** — the standard All Call Status indication ID; its payload is not decoded here |
| Matching dial response returns error `0x0d` | **V25:188125–188128** — CommCenter decodes `QMI_ERR_NO_NETWORK_FOUND` and ends the call |
| No IMS registration; CS call activity returns false | **V25:188196–188198,188212** |

`kRatLTE CS call active` (**V25:187912,188023**) is set during the attempted call, before successful setup. It is **not evidence of a connected CS bearer on LTE**. The response arrives about 14 ms after the dial request; no completed GSM fallback is shown. The error identifies a network/service-selection failure class, not its exact cause: coverage, CS registration, modem policy/firmware configuration, subscriber provisioning, or request options remain candidates.

The bridge has no Voice-specific refusal or dial translation: **B:455–486** forwards ordinary service requests; its exceptions concern vendor reporting, data endpoints and DMS reset/shutdown. **Q:61,303–325** classifies only `0xe1..0xea` as private, then selects the advertised server. Unimplemented private messages receive NOT_SUPPORTED via **B:489–494**. Thus standard Voice can pass through, while Apple AUDIO does not.

Latest boot confirms the current Voice client path: **V28:10992–10993,11457–11458** forwards `0x44` to node 3/port 46 and receives success; **V28:11640–11646,11800–11803** forwards Voice indication-registration `0x0003` from two clients and receives success. This is stronger evidence than a missing “Voice client registered” log string. Conversely, Apple AUDIO `0xe1/0x002f` is rejected with `0x5e` (**V28:18084–18088**).

The iOS 16.5 binaries support the same split:

- `tools/tmp/commcenter-umi-research/libQMIParserDynamic.dylib.strings:221,226` names QMI Voice and QMI Audio separately (VM addresses `0x1e9a895f8`, `0x1e9a8966a`).
- `tools/tmp/commcenter-umi-research/libCommCenterMCommandDrivers.dylib.strings:3454–3465` contains Voice/Audio client startup and Voice indication registration; **:3488–3505** contains CS-voice checks, normal dialing and call-ID response handling; **:3521–3550** contains All Call Status and incoming/origination/alerting/conversation/end processing.
- In `tools/tmp/dsc-20F66/System/Library/Frameworks/CoreTelephony.framework/Support/libCommCenterMCommandDrivers.dylib`, RTTI names identify `vs::EndCall::Request` at **file+0x214823**, `vs::AnswerCall::Request` at **file+0x21dd75**, and `EurekaCallCommandDriver::dial` callbacks taking `vs::VoiceDialCall::Response` at **file+0x22a810**. This is binary evidence for the Voice protocol family, beyond generic CallKit strings.
- Expected standard operations are Dial `0x20`, End `0x21`, Answer `0x22`, All Call Status indication `0x2e`. Dial is observed; successful answer/end and compatibility of optional TLVs remain untested. Do not call the whole call-control lifecycle proven.

This differs from the Apple NAS cell-info case: `LOCATION-umi.md:66–71` identifies `nas::GetCellInfo` as private `0x5556`, rejected with INVALID_QMI_CMD `0x47`. The recorded voice dial uses standard `0x20` and gets a network error instead. **A MAVIMS↔IMSA/IMSS translator is not a demonstrated prerequisite for this CS dial path.** The IMS-oriented scope in `COMMCENTER-umi.md:307–315` should not be applied wholesale to CS calls.

Two qualifications matter. Latest boot initially selects `TrekC2K` as its signal model and starts a C2K radio module (**V28:5725,5734**), while selecting `EurekaCallCommandDriver` (**:5729**); Eureka later logs invalid LTE call technology/no formatter update (**:15235**). Audit the final GSM personality/formatter before changing policy; the initial signal-model name alone is not proof of the dial failure. Also V25 predates stricter service-source validation: current **B:1433–1446** documents and rejects foreign-service packets after client-port reuse. Repeat the dial on the current bridge with correlated QRTR source/destination metadata before treating every V25 indication as authoritative.

CallKit/callservicesd already reaches the call module in the V25 attempt (**V25:187958,188027–188030,188190**). Latest callservicesd voicemail/tincan service lookup failures (**V28:26288,26810–26812**) concern auxiliary services and do not establish that ordinary cellular dialing is blocked there.

## 3. Audio: Android provides a PCIe/ADSP reference

### iPhone side: control is distinct from samples

On the Qualcomm/Mav path, the inspected code exposes an `AudioRoutingCommandDriver.qmi`, vocoder, sample rate and **I2S clock rate** (`libCommCenterMCommandDrivers.dylib.strings:1725–1732`, including VM `0x1f0f8d928`). This supports a separately configured baseband audio link; QMI Voice messages carry call control, not the speech stream. It does **not** prove the exact physical PCM/I2S wiring of every iPhone model. No board schematic or extracted hardware audio-driver evidence establishing this payload's precise electrical path was found; audio over an Apple media transport is not established either.

The AP software contract is clearer. `work/umi-v23/userland/src/audio_plugin.c:4–32,216–221` describes mediaserverd/CoreAudio HAL → VirtualAudio routing and implements only `Speaker`, `Hawking`, `Actuator`, and microphone `Codec` devices. There is no cellular speech endpoint in that device table. The bridge publishes a `mediactl` channel but leaves non-QMUX channels without a backend (`…/overlays/xnu/files/bsd/dev/coachz_bb_channels.h:20–26`; **B:443**); publishing its name does not implement audio.

Binary anchors in the extracted 20F66 cache:

- CommCenter: `AVAudioSessionCategoryPhoneCall` at **file+0x123fcdd**; RTTI for `TelephonyProviderDelegate provider:didActivateAudioSession:` issuing `start_media` at **file+0x111f843**.
- `System/Library/Frameworks/AVFAudio.framework/AVFAudio`: `_AVAudioSessionCategoryPhoneCall` at **file+0x17efa1** and `setVoiceProcessingEnabled:error:` at **file+0x13b4d7**.
- `System/Library/Frameworks/AudioToolbox.framework/AudioToolbox`: “Cannot enable VoiceProcessingIO with invalid formats” at **file+0x249aad**; `_AudioToolboxServerAccessTelephonyMutes` at **file+0x2e4326**.

These establish telephony session/processing/mute interfaces, not a modem sample transport. Selecting PhoneCall or enabling VoiceProcessingIO cannot connect X55 speech to the existing speaker/mic. Discover the actual VirtualAudio cellular device/stream/property contract and match activation, route changes, sample-rate/vocoder changes, mute and teardown. Do not invent a device UID from a descriptive name such as “BasebandVoice”.

### umi: voice over PCIe exists in the vendor design

**The evidence points to X55 ↔ PCIe/MHI offload ↔ shared mailbox ↔ AP ADSP voice services ↔ codec/amp backends.** It does not support treating tertiary MI2S as a physical X55-to-codec cable.

| Evidence | Consequence |
|---|---|
| `/Users/xm666/X/mainline/umi-linux/work/recon/live.dts:11471–11479`: channel **80 (`0x50`)**, `AUDIO_VOICE_0`, `mhi,offload-chan` | A vendor voice offload channel exists; it is not an ordinary UL/DL PCM ring pair. |
| Same tree's `work/recon/mhi-net-platform.txt:8` | Archived Android enumeration places `AUDIO_VOICE_0` on the actual PCIe2 X55 device. This is Android reconnaissance, not proof of mainline call audio. |
| `work/umi-mic/live.dts:32750–32754,35586–35592`; `work/umi-mic/android-dmesg.txt:5515` | Voice-MHI driver has a **128 KiB mailbox** allocation pool, below 4 GiB; Android associates the platform device with it. |
| **A/dsp/voice_mhi.c:104–115,282–320,348–379,455–464,509–541** | Matches `AUDIO_VOICE_0`, maps shared memory into PCIe and ADSP address spaces, registers ADSP **MVM**, and sends `VSS_IPKTEXG_CMD_SET_MAILBOX_MEMORY_CONFIG` with both IOVAs. |
| **A/dsp/voice_mhi.c:214–280**; **A/dsp/q6voice.c:7334–7344,6987–6998** | Call start/end votes keep PCIe available; q6voice creates/destroys MVM/CVS sessions and vocproc. |
| `work/umi-mic/mixer_paths_overlay_static.xml:109–116,177–186` | VoiceMMode RX routes to handset codec or tertiary MI2S speaker, TX to `TX_CDC_DMA_TX_3`; speaker mode also selects an echo-reference path. |
| `work/umi-mic/audio_platform_info_intcodec.xml:199–204,223–240` | Handset downlink uses `RX_CDC_DMA_RX_0`; speaker uses `TERT_MI2S_RX`; microphone uses `TX_CDC_DMA_TX_3`. |

The mainline data recipe is **not** a ready voice implementation. `mnt/mainline-src/linux/drivers/bus/mhi/host/pci_generic.c:410–420` has Sahara/DIAG/QMI/IPCR/IP_HW0, with no voice channel; `devices/xiaomi-mi-10/MODEM-MAINLINE.md:113–122` adds the mission EFS/data profile but still documents no voice transport. The generic v6.12 SDX55 profile likewise contains no audio channel (`work/modem-umi/linux-v6.12/drivers__bus__mhi__host__pci_generic.c:248–263,319–328`). Do not infer “no hardware audio” from this omission, or confuse `IP_HW_MHIP_*` network/IPA offload with `AUDIO_VOICE_0`.

The vendor source is a strong implementation reference, not a captured successful call. Missing details include offload channel/context ownership and doorbells, mailbox ABI, current firmware's APR/GPR variant, session/VSID and modem/AP coordination, calibration/topology, and cleanup after modem/ADSP restart. The mailbox driver itself exposes no CPU read/write PCM loop; **do not assume its packets are plain PCM** or that merely starting MHI channel 80 supplies samples.

### What HakiOS still needs

Existing playback is AP ASM/ADM → AFE `0x1004` → tertiary MI2S → CS35L41: **V28:2016,2174–2182,2416–2420** shows 48 kHz stereo S16 PCM and working clocks. `MIC-umi.md:5,51–71,105` documents WCD9385 ADC1 → SoundWire → TX macro → AFE `0xb037`, with mono 48 kHz ASM capture and no voice processing/calibration. These are useful endpoints, not a modem voice session.

Implement the shared PCIe/ADSP voice mailbox with valid mappings in **both** DMA domains, the vendor offload setup, and MVM/CVS/CVP session/routing/calibration control. Reuse the proven physical speaker/mic setup while arbitrating it against media playback/capture. Handset mode needs the WCD receiver/RX route too; the current speaker path does not establish it. Provide call volume/mute, echo cancellation/reference, sample-rate handling, ringtone/media interruption and bounded teardown. Current playback queues eight 20 ms periods (`…/overlays/xnu/files/bsd/dev/hakios.sound.c:579–582`); routing speech through that media queue would add 160 ms before other latency, so it needs a separate latency assessment.

The cheapest audio experiment is the vendor ADSP voice route with one output and one microphone, under supervised low-volume conditions, followed by minimal iOS telephony lifecycle/route adaptation. A CPU PCM bridge through the existing HAL is a fallback only if a real PCM tap/injection API is found; it is not supplied by the QMI bridge. Keep the existing speaker protection constraints when experimenting with direct DSP routing.

## 4. Ordered path and stop conditions

Engineering estimates below assume one engineer, usable references, and supervisor-provided hardware windows; they exclude waiting for operator changes.

| Order | Work and acceptance gate | Estimated effort |
|---|---|---|
| 1 | **Prove the network first.** Supervisor verifies the same SIM can register and complete ordinary GSM CS calls on stock Android, with IMS inactive; then captures LTE CSFB. Direct GSM success with LTE CSFB failure isolates the interworking/policy problem. If neither is available, stop this workaround for this SIM/location. | 0.5–2 days of measurements; operator outcome has no engineering ETA |
| 2 | Compare Android and HakiOS NAS mode/band/voice-domain preferences, CS/PS attach, subscription and combined attach result. Correct only demonstrated runtime policy/translation mismatches. Allow GSM and prefer a CS-capable voice policy; avoid an accidental LTE-only setting. Recheck final CommCenter GSM formatter and QRTR source validation. | 1–5 days if preferences explain it; 1–2+ weeks if firmware/carrier behavior is involved |
| 3 | **Control-only calls.** Repeat standard Voice Dial; require a real call ID, origination/alerting/conversation on GSM, remote ringing/answer, and end. Then incoming indication/paging, answer/end, caller-ID handling, DTMF and return to LTE/data. Correlate QRTR ports/transactions without logging identities. | 1–3 days after working CS service; optional TLV differences may add several days |
| 4 | **One working audio route.** Trace Android `voice_mhi`/q6voice mailbox and session setup during a real CS call; reproduce dual DMA mapping, offload and one bidirectional ADSP voice route. Require intelligible audio both ways and clean stop. | 2–6+ weeks, highest technical uncertainty |
| 5 | Integrate iOS PhoneCall activation/VirtualAudio routing and private AUDIO status where actually required; validate mute/volume, interruptions, speaker/receiver switching, echo/latency and repeated-call cleanup. | 1–3+ additional weeks; overlaps step 4 |

**Supervisor measurements that settle the unknowns:** retain only redacted metadata/decoded enums, never full telephony objects or raw dial/SMS/UIM payloads.

1. Stock Android: successful GSM-only call in this location with this SIM, or explicit rejection/no service; verify actual GSM RAT and IMS inactive. Any temporary mode change is a supervisor-controlled experiment with original settings restored, not an instruction to change persistent NV.
2. LTE case: pre/during/post NAS serving-system/system-info and system-selection preference; EMM combined attach/TAU result and SMS-only status; CSFB extended-service request, redirection/camping and reject/call-end cause. Distinguish MO and MT CSFB. Confirm operator-side 2G voice entitlement/coverage if modem evidence is inconclusive.
3. Current HakiOS: Voice request/response IDs, result, call state and sanitized end reason, with source/destination QRTR ports and transaction IDs. Resolve `NO_NETWORK_FOUND` before spending weeks on audio. No emergency-number testing is needed.
4. Android audio: active VoiceMMode/VSID and ALSA mixer routes, ADSP MVM/CVS/CVP commands/results, mailbox allocation/IOVA/size, MHI offload state, power votes and teardown. Observe descriptor/counter changes rather than recording conversation audio. Generic MHI byte counters alone may miss DSP offload traffic.
5. iOS audio: PhoneCall activation success, selected VirtualAudio/HAL route/device UIDs and formats, required AUDIO messages/status indications, and non-content stream timing. Confirm whether mediactl is used rather than assuming it carries speech.

No step requires storage repair or modem provisioning experiments. **Any proposal to write modem NV or touch modemst/fsg/persist/EFS partitions requires the user's explicit OK.** Existing RAM-backed modem services are not permission to alter their persistent backing. The practical recommendation is to pursue the inexpensive network/control gates now; commit to the audio port only after GSM CS voice is demonstrated.

## Implementation — 2026-11-11, milestone 1a

**Implemented: switch-gated control diagnostics and independent, read-only NAS snapshots. No connected GSM call, CSFB repair, or call audio is claimed.** The new bridge compilation unit compiles for XNU and its actual observer/query code passes ASan/UBSan host tests. The full kernel build is currently blocked by unrelated WLAN/network compilation errors described below. There is no new bootable `kernel-voice` yet.

### Files and behavior

- `overlays/xnu/files/bsd/dev/umi_voice.inc`: all new runtime logic. `drivers/umi/hakios.umi_baseband.cpp` has only an include, worker allocation, outgoing/incoming observer calls, dedicated-port dispatch and polling/send/release hooks. The concurrently added NAS cell-info implementation is left to cell-positioning.
- `tools/test-voice.py`, `tools/voice-tests/voice.cpp`: compile and execute that same include with host VFS/clock/thread-call models and the real QMUX implementation. Independent-port checks cover GNSS `0x4200`, cell-info `0x4201`, and Voice `0x4202`.
- `/Users/xm666/X/umi-ios/work/voice-cs/libqmi-{voice-nas,system-info,set,enums}.dis`: local protocol reconnaissance from the cached Debian libqmi 1.36 library. No phone traffic or subscriber data is in these files. Reference binary SHA256: `f1b81265e7d2992ff9b78947b2ace78592e403a04e39f06825a165841e502cfd`.

The regular file `/var/db/hakios-voice/on` enables diagnostics. It is sampled about once per second on a separate thread call, outside `bb_lock` and outside the modem's MHI/QRTR/data service thread. With the file absent, the new code sends no QMI messages, changes no policy, and leaves the existing call/data/SMS/audio paths intact. Removing it cancels the diagnostic sequence and releases its QRTR client; it does not end an existing CommCenter call. Wait at least two seconds between switch changes. A worker-allocation failure logs once and leaves diagnostics disabled.

On enable, and after observing a queued Dial or an incoming All Call Status indication, the independent NAS client reads System Selection Preference `0x0034`, Serving System `0x0024`, and System Info `0x004d`. One request is pending at a time, with matched source node/port, message and transaction. A QMI failure, malformed mandatory result, or five-second timeout stops this reader and logs once; remove/recreate the switch to retry. It does not occupy an ABM client, add NAS subscriptions, wait for a snapshot before dialing, or send Set/Attach/Dial commands itself. Snapshots may overlap a RAT transition; they are observations, not an atomic radio-state dump.

Voice Dial/Answer/End and registration requests remain byte-for-byte CommCenter's. Standard Voice indications still follow the existing source validation → QMUX framing → host queue path; they do not require a pending request. Tests exercise incoming/conversation/end indications for two clients on each QMUX channel, verify client/channel ownership and exact preserved SDUs, and verify that the observer never mutates a frame. A `to-host` diagnostic marks that delivery boundary; supervisor must still confirm CommCenter consumes the indication and that the host queue reports no drops.

Logs whitelist only headers, call IDs/states/types/directions/modes, numeric end reasons, band masks and NAS registration/policy enums. They never decode Dial's mandatory number TLV or number/name/SIM/PLMN/location fields. In All Call Status, Call End Reason is the standard optional **`0x14`** counted `{u8 call_id, u16 reason}` array; `0x10/0x16/0x18/0x19` number fields stay opaque. The host privacy fixture specifically makes redirecting-number TLV `0x19` resemble a counted array and verifies that it is ignored. No raw QMI/TLV dumps are added. Optional Call End Reason parsing still needs a live firmware check.

### What the network evidence does and does not establish

Additional header-only inspection of V25 finds successful NAS `0x33/0x34` exchanges at lines 12234–12275, 12900–12969 and 13069–13764. `0x33` requests have several different lengths; the old trace does not contain their TLV values. The bridge's existing exceptions do not rewrite these standard NAS operations or standard Voice Dial. CommCenter's displayed GSM band mask also does not prove the X55's actual RAT preference. **There is no demonstrated translation mismatch to repair from these logs.** Forcing GSM, CS-only or combined attach now would be an unverified modem-policy experiment and could break working LTE data.

The decoder was checked against the cached libqmi parser/getters, including these easily confused layouts:

| NAS field | Set `0x33` TLV / width | Get/changed `0x34` TLV / width |
|---|---|---|
| Mode preference | `0x11` / u16 | `0x11` / u16 |
| Legacy band mask | `0x12` / u64 | `0x12` / u64 |
| LTE band mask | `0x15` / u64 | `0x15` / u64 |
| Change duration | `0x17` / u8 | absent |
| Service domain | `0x18` / u32 | `0x18` / u32 |
| Usage preference | **`0x21` / u32** | **`0x1f` / u32** |
| Voice domain | **`0x23` / u32** | **`0x20` / u32** |

System Info request/response is **`0x4d`**, while its indication is **`0x4e`**. Its GSM/LTE service-status TLVs are `0x12/0x14`; only the initial domain/capability/roaming validity/value pairs of `0x17/0x19` are inspected. LTE voice support/domain/registration domain are `0x21` u8, `0x2a` u32 and `0x31` u32. These do not expose EMM combined-attach acceptance, SMS-only registration, the CSFB extended-service request, or GSM redirection results. Those remain supervisor measurements.

Useful numeric interpretation: mode bit `0x04` allows GSM and `0x10` allows LTE; service-domain preferences `0/1/2` mean CS-only/PS-only/CS+PS; voice-domain preferences `0/1/2/3` mean CS-only/PS-only/CS-preferred/PS-preferred; usage `1/2` means voice/data-centric. These are policy values, not proof of current service. Serving CS/PS attach `1` means attached and `2` detached. Extended LTE/NR masks and acquisition-order lists are not decoded in this milestone; GSM allowance is checked in the mode and legacy-band masks.

**Voice and NAS RAT enums differ.** The Voice call-mode values logged as `RAT` are `1 CDMA, 2 GSM, 3 UMTS, 4 LTE`. NAS Serving System radio-interface values include `4 GSM, 5 UMTS, 8 LTE`. Voice states include `1 origination, 2 incoming, 3 conversation, 5 alerting, 6 hold, 8 disconnecting, 9 end`. `-1` in the new logs means an absent/invalid optional field, never a synthesized success.

### Audio and iOS: exact stop point

Steps 2 and 3 are **not implemented or exercised**. Existing media playback/capture continues to use its working stack. No channel-80 context, mailbox allocation/DMA mapping, MVM/CVS/CVP session, receiver/mic route, calibration, or speech mute/volume implementation is added. No Apple private AUDIO message is newly acknowledged, and no PhoneCall activation or VirtualAudio UID is invented.

The vendor `voice_mhi.c` confirms MVM mailbox command `0x0001333b`: payload `{u64 ADSP_IOVA, u64 PCIe_IOVA, u32 size}` for the same allocation, source APR port `0x0103`, destination `0`, and call-lifetime PCIe votes. Current `hakios_adsp_apr()` uses one port argument for **both** source and destination, and `hakios_apr_answer` does not expose the response source/session port. Reproducing the vendor control path requires extending that contract, adding voice-service response/session ownership, and mapping the mailbox in both DMA domains. The current MHI profile owns ordinary boot/control/data rings and does not implement channel-80 offload/context ownership. These are concrete missing interfaces; they do not establish that the firmware's ADSP voice services are unreachable.

There is consequently no justified PCM fallback yet: neither inspected mailbox driver nor current MHI implementation provides a proven CPU PCM tap/injection ABI. Sending assumed PCM to channel 80 would not implement a voice path. A future verified PCM bridge must assess the existing eight 20-ms playback periods (up to 160 ms queued), capture buffering, clock drift and echo cancellation/reference. Prefer the vendor DSP voice route after call control passes.

For iOS, the next integration must observe actual `AVAudioSessionCategoryPhoneCall` activation and `start_media`, identify the real cellular VirtualAudio stream/property contract, bind its start/stop to a working DSP bearer, and propagate receiver/speaker changes, telephony mute and call volume. Present `Speaker`/`Codec` media devices and media volume are not evidence of an active cellular voice route. Supervisor must test these lifecycle events after a real sample transport exists.

### Validation and build status

Passed locally, with no phone access:

```sh
cd /Users/xm666/X/umi-ios/work/umi-v23
python3 devices/xiaomi-mi-10/tools/test-voice.py
python3 devices/xiaomi-mi-10/tools/test-modem.py
python3 devices/xiaomi-mi-10/tools/test-gnss.py
python3 devices/xiaomi-mi-10/prepare.py verify
cd /Users/xm666/X/umi-ios
python3 work/umi-mic/check-host.py
python3 work/umi-mic/check-capture3.py
```

The modem suite includes existing baseband/QMUX/pending-request, PCIe, MHI, data, firmware-staging and loader-gate checks. The existing sound/microphone host checks cover DMA completion handling, codec/SoundWire constants, reset, PCM statistics, BOB and shared RPMh behavior; their referenced mic and sound sources match the current overlays. They do not demonstrate acoustic playback or call audio. Voice tests additionally cover worker scheduling outside the modem thread/lock, absent-file behavior, read-only request bytes, source/transaction matching, cancellation/late responses, failure latch, multi-call end-reason association, privacy and truncated arrays under ASan/UBSan. Changed files pass whitespace checks; repository-wide `git diff --check` reports pre-existing whitespace in `xnu-device.patch`.

Build command used exclusively:

```sh
cd /Users/xm666/X/umi-ios/work/umi-v23
/Users/xm666/X/umi-ios/build-lock.sh bash devices/xiaomi-mi-10/build-kernel.sh
```

Latest build log: `/Users/xm666/X/umi-ios/logs/build-umi-voice.log`. The actual `hakios.umi_baseband.cpo` compiles with `umi_voice.inc` included. Full linking is blocked by the shared tree's `iokit/Kernel/hakios.wlan.cpp:195` (`setProperty` rejects `const uint8_t *`) and `bsd/dev/hakios.net.c:555` (`ifnet_init_eparams` has `detach`, not `detached`). Earlier attempts also encountered concurrent display errors. Those forbidden areas were not edited for this task. **No kernel was copied to `out/xiaomi-mi-10/flash/kernel-voice`; kernel SHA256 is unavailable. The pre-existing output kernel is stale for these changes.** After the owners repair their compilation errors, rerun the locked build successfully before copying that build's `mach.development.bcm2837` to the requested `kernel-voice` and hashing it. No other `kernel-*` file was overwritten, no full userland/flash build was run, and no modem NV/storage setting was written.

### Exact supervisor test checklist — after a successful linked kernel exists

1. Verify the successful locked build, freeze **that** kernel as `out/xiaomi-mi-10/flash/kernel-voice`, and record `shasum -a 256` for it. Boot it only through the supervisor's normal `phone-test.sh` procedure with the established complete driver arguments and existing userland. Add no boot-args. This milestone requires no new userland payload.
2. With the same SIM/location on stock Android, use a **non-emergency number chosen by the user** to prove a genuine GSM CS call with IMS inactive. Confirm the actual GSM call RAT; an LTE/VoLTE call is insufficient. Restore any temporary UI-selected mode afterward. Record only decoded service/RAT/reject enums, never the chosen number, subscriber identity, full telephony objects or conversation audio. If GSM service itself is unavailable, stop the CS workaround for this location/SIM.
3. First test the candidate in iOS with `/var/db/hakios-voice/on` absent. Confirm the stock Phone/SpringBoard remain usable, cellular data transfers, incoming/outgoing SMS, Voice Memos capture and ordinary receiver/speaker playback behave as before. Expect no `voice:` enable/snapshot lines. Do not restart or kill CommCenter.
4. In the supervisor-controlled **on-phone root shell**, enable the diagnostic file with `mkdir -p /var/db/hakios-voice` followed by `: > /var/db/hakios-voice/on`. Wait at least two seconds. Expect `voice: enabled`, followed by successful snapshot messages `0x34`, `0x24`, `0x4d`. Export only `voice:` lines and safe generic transport counters from the already captured log; do not export raw QMI or full CommCenter call objects.
5. Inspect `NAS current` mode/service/voice-domain and band masks, serving CS/PS attachment, GSM/LTE status/domain and LTE registration-domain before dialing. Compare with Android's read-only measurements. GSM excluded, PS-only policy, detached CS, or unavailable GSM are **investigation findings**; do not automatically change modem settings. Obtain combined EPS/IMSI attach/TAU and SMS-only evidence separately. If a demonstrable translation mismatch appears, implement just that runtime mapping in this include and repeat the data/SMS regression checks.
6. From stock Phone, dial the **user-chosen test number**. Require `tx ... msg 0x20`, the correlated response and call ID, remote ringing, and All Call Status progression to GSM (`RAT 2`) conversation (`state 3`). The remote participant should answer once and hang up; confirm `state 9`, end reason if provided, return to LTE/data and no stuck call. This is a control-only test: silence is expected until audio work is implemented. If error `0xd` recurs, retain the sanitized snapshot/state sequence and stop; the diagnostic reader does not repair the network.
7. Place an incoming call from the remote phone to this SIM. Require `all-call-status ... to-host` and `state 2`, actual ringing UI in stock Phone, Answer request `0x22` for that call ID, conversation and End `0x21`/remote end. Confirm no baseband host-queue drops or CommCenter QMI parse failure. Caller information may be displayed normally by the OS but must not be copied into diagnostic logs. Repeat one missed and one rejected incoming call and confirm cleanup/data/SMS afterward.
8. Remove `/var/db/hakios-voice/on` in the on-phone shell and wait at least two seconds. Expect `voice: disabled`; no further diagnostic queries or call metadata. Verify ordinary data, SMS, media capture/playback again. For a reader failure/timeout, leave it off for two seconds before recreating the file; expect a new snapshot transaction, not acceptance of a late old response.
9. Only after outgoing and incoming GSM call control passes, implement/test the dual-domain mailbox and one DSP speech route, then PhoneCall lifecycle/routing. Final acceptance still requires intelligible two-way speech, receiver↔speaker changes, microphone mute, call volume, remote/local hangup, media interruption/resume, echo/latency assessment and repeated-call teardown. None of those audio checks passes at milestone 1a.

Illustrative **synthetic** metadata, not a hardware result:

```text
voice: enabled; control diagnostics only, audio unavailable
voice: snapshot msg 0x34 txn 1 from 3:47
voice: NAS current mode 20 service-domain 2 usage 1 voice-domain 0 duration -1 (-1 absent)
voice: NAS serving reg 1 CS-attach 1 PS-attach 1 network 2 RAT-count 1
voice: NAS serving RAT 8
voice: all-call-status calls 1 to-host ch 0 cid 1
voice: call-id 7 state 3 type 0 direction 2 RAT 2 end-reason -1 (-1 absent)
```

Bad/control-blocking examples:

```text
voice: dial call-id -1 error 0xd
voice: NAS current mode 16 service-domain 1 usage 2 voice-domain 1 duration -1 (-1 absent)
voice: NAS serving reg 1 CS-attach 2 PS-attach 1 network 2 RAT-count 1
voice: snapshot timeout msg 0x34; toggle switch to retry
voice: all-call-status malformed metadata; forwarded unchanged
```

Mode 16 excludes GSM; mode 20 allows it. Neither value establishes successful GSM camping/CSFB. A `to-host` line without Phone UI progression still requires investigating host delivery and CommCenter parsing; it is not a completed incoming-call test.

## Implementation: call audio — 2026-11-11, mailbox milestone 1b

**Partial implementation: the vendor global voice mailbox path is implemented and host-tested; speech/session routing is not implemented or hardware-verified.** This does not deliver call audio. Supervisor's new stock-Android result supersedes the earlier GSM-first recommendation: with VoLTE disabled, calls fail immediately at this location. Do not spend further time on GSM/CS fallback here. The mailbox and interfaces below serve modem-owned VoLTE as well as CS. The independent IMS investigation owns `umi_ims.inc`; this change does not edit it.

### What is implemented and where

All runtime additions live in `overlays/xnu/files/bsd/dev/`:

- `umi_voice_audio.h`: C kernel interface for QMI/IMS/future iOS glue: asynchronous start, stop, receiver/speaker route, microphone mute, volume and status. Network accepts `UMI_VOICE_GSM` and `UMI_VOICE_LTE`. There is one call owner, with a 32-bit internal call key; no key, VSID, address, number or subscriber identity is logged by `voice-audio:`.
- `umi_voice_audio_control.inc`, included by `umi_voice.inc`: CONVERSATION on QMI Voice types voice/voice-IP and GSM/LTE modes queues mailbox bring-up; END cancels that call's work. Duplicate delivery through multiple ABM clients does not duplicate initialization. The existing switch-file worker executes memory and APR work outside `bb_lock` and outside the modem service thread. Switch-off cancels the call request. Stop is request cancellation at this milestone; there is no running DSP voice session to stop.
- `umi_voice_audio_mhi.inc`: an asynchronous request to the modem service thread reserves 128 KiB of PCIe IOVA space inside the existing protected modem pool and maps the mailbox's physical pages into its SMMU context. PTEs and TLBI/TLBSYNC use the existing SID `0x1d01` domain. A service heartbeat rejects an absent/stopped modem. The AP asserts that channel 80's context is untouched and the link is in M0. Only the modem service thread allocates its IOVA slot or writes these tables.
- `umi_voice_audio_memory.inc`: allocates a physically contiguous, zeroed 128 KiB range with the allocator's maximum physical page constraint below 4 GiB. It pins audio-domain user 2 and adds a separate ADSP mapping at IOVA `0x10020000`, beyond the existing playback and capture buffers. The returned ADSP address includes the established SID selector `1 << 32`, as vendor `msm_audio_ion_dma_map()` does. Existing audio page tables, SID `0x1801`, lock and context ownership are reused. Bypass uses the same low physical allocation. Initial cache clean and DMA attributes match the existing domains. The CPU never touches the live mailbox or interprets it as PCM.
- `umi_voice_audio_apr.inc`: implements `VSS_IPKTEXG_CMD_SET_MAILBOX_MEMORY_CONFIG` (`0x1333b`) with the exact packed 20-byte payload `{u64 ADSP address, u64 PCIe address, u32 131072}`. APR goes to **ADSP MVM service 9**, source port `0x0103`, destination port zero. Modem-domain MVM service 4 would address the wrong ADSP service (AFE). Reply ownership checks both services/domains, destination/source ports, token, basic-result opcode and echoed command. APR_RSP_ACCEPTED is not completion. A 500 ms timeout or malformed/nonzero DSP result ends the attempt. Voice replies, including late ones, are consumed separately from media/microphone replies; the existing APR API and two media handlers are unchanged.

Hook-only changes: two lines each in `hakios.sound.c` and `hakios.adsp.c`; an include and service-poll call in `umi_modem_mhi.inc`; three audio hooks plus the status wording in the owned `umi_voice.inc`. No new bridge hook is needed: the existing milestone-1a observer/worker hooks suffice. `umi_cellinfo.inc`, the bridge's cell-info implementation, Wi-Fi/network files, display/GPU, loader, mkjobs and userland are untouched by this implementation.

### Channel 80: why there is no ordinary ring or START_CHAN

The vendor reference resolves a misleading assumption in the original task. Android runtime DT (`work/umi-mic/runtime.dts:4613`) describes channel 80 `AUDIO_VOICE_0`, direction 0 (bidirectional), offload, data type 3 and event ring 0. The locally available vendor kernel object `HEAD:drivers/bus/mhi/core/mhi_init.c`, lines 639–642, **skips channel-context initialization for offload channels**; its probe also permits absent transfer callbacks. `voice_mhi.c` registers the offload device, maps shared memory, votes PCIe/MHI awake and sends the MVM mailbox command. It does not call mhi_prepare_for_transfer, construct a channel-80 TRE ring, send START_CHAN or ring its transfer doorbell. Mainline MHI's equivalent skip is in `mnt/mainline-src/linux/drivers/bus/mhi/host/init.c:332`.

This implementation follows that path. Inventing a host channel-80 ring/doorbell exchange would not reproduce the vendor ABI. The existing HakiOS MHI driver already keeps PCIe and DMA allocations alive, rejects M3 and returns M1 to M0, so a call does not need to add another hardware power vote. If runtime power management is later added, voice needs a real lifetime vote before M3 becomes possible. All existing IPCR/data/event/command rings remain owned by their current paths. A mailbox ACK establishes only firmware acceptance of the memory configuration, not channel traffic, audio samples, or intelligible speech.

The global mailbox and both DMA mappings deliberately remain pinned until reboot, even on END, switch-off or a lost APR reply. The vendor reference has no mailbox-unset handshake; freeing/reusing this allocation while either firmware may still reference it is not a valid teardown. This costs 128 KiB plus one consumed IOVA reservation and holds the shared audio domain. Transport failures latch until reboot; toggling the diagnostic switch only resets the older NAS reader. There is no automatic transport retry or DSP/modem reset. ADSP/modem restart recovery is outside this milestone; reboot before another attempt after either restarts.

### IMS integration and network/vocoder ownership

An IMS implementation should invoke this interface **only when it selects modem media and its call has connected**. AP RTP/CoreAudio media must not start a competing modem speech path. Use a stable internal call key, never a telephone number or SIP identity:

```cpp
// umi_ims.inc is included in the bridge. Its dispatch holds bb_lock:
int pending = umi_voice_audio_start_locked(call_key, UMI_VOICE_LTE, UMI_VOICE_RECEIVER);
// On termination, still holding bb_lock:
int stopped = umi_voice_audio_stop_locked(call_key);
```

Callers outside `bb_lock` use `umi_voice_audio_start()` / `umi_voice_audio_stop()`. The normal status/route/mute/volume functions also acquire `bb_lock` and must be called outside it. `start` returns EINPROGRESS for queued work, EPERM if the sampled file switch is off, EBUSY for a different call owner, or the existing result for an identical request. `status` eventually returns ENOTSUP when mailbox setup succeeded but the session route is unavailable; it never returns audio-running success. A reused owner with a changed network/route is unsupported. Controls return ENOTSUP rather than acknowledging hardware changes. Volume range is 0–100. No new sysctl/device/boot argument or Apple AUDIO acknowledgement is invented.

The LTE enum is intentional; there is no forced GSM network command. Vendor `q6voice.c:906` creates **passive** MVM/CVS sessions for modem voice, VoLTE and VoiceMMode. Legacy VoLTE uses the name `default volte voice`; the mixer XML also exposes VoiceMMode1/2, whose bindings are `11C05000`/`11DC5000`. These are not interchangeable merely because a QMI call-mode field says LTE. In `voice_setup_vocproc()` (`q6voice.c:4524`), explicit network/media-type/vocoder/timing setup is guarded by `is_voip_session()`, the **full-control AP VoIP** session. It is not the passive modem VoLTE branch. `voice_send_mvm_cal_network_cmd()` sets the calibration network to `VSS_ICOMMON_CAL_NETWORK_ID_NONE`; that is a calibration selector, not a GSM/LTE RAT enum. The passive modem negotiates network and AMR/AMR-WB/EVS with its peer; imposing a fixed AMR-WB or EVS mode here would contradict that ownership. Negotiated codec/rate and the actual modem/AP session binding must be established before routing speech.

### Exact stop point and evidence needed to proceed

**MVM/CVS/CVP session creation/attachment, AFE voice routing, START/STOP_VOICE, speech mute/volume, minimal voice calibration, echo reference and media arbitration are not implemented.** There is no evidence yet that this ADSP firmware lacks voice services; source inspection and host tests cannot determine whether this firmware accepts even the mailbox command. The first hardware milestone is its real APR result. Firmware incompatibility is not claimed.

The next clean milestone requires a correct passive-session binding and a CVP/AFE route, which the current stack does not expose. Current sound/mic starts ASM/ADM media paths and does not supply a voice calibration loader or a telephony route owner. Starting a second AFE owner over those active paths would not establish a safe bidirectional voice route. Concrete missing evidence/interfaces:

1. **Active Android VoLTE session tuple:** MVM/CVS creation names and response handles, VoiceMMode/VSID selection, dual-control setting and call-lifetime command ordering. The static XML lists both VoiceMMode1 and 2; it is not a trace selecting one. Collect command metadata/results, not raw telephony objects or conversation data.
2. **CVD/CVP version and command layout:** vendor `voice_send_cvp_create_cmd()` (`q6voice.c:2948`) switches CREATE_FULL_CONTROL_SESSION V2/V3 at CVD 2.2. The current APR media API also does not return the response source/session handle. The mailbox-only receiver added here intentionally does not invent session handles or a voice event registry.
3. **Actual handset/speaker calibration/topologies and AFE formats:** `voc_get_tx_rx_topology()` (`q6voice.c:2510`), `voice_get_topology()` (`:8865`), CVP device config, common/volume/stream calibration blocks and their column descriptors, selected RX/TX AFE IDs/formats, echo reference, plus the protected-speaker route. `voice_get_topology()` has generic RX-default/TX-none fallbacks if calibration is absent; their existence does not establish a calibrated WCD9385 handset/speaker route or echo cancellation. Do not treat XML ACDB numeric IDs as calibration payloads. Existing speaker protection ceilings still apply.
4. **Route arbitration and cleanup:** a concrete ownership API must suspend media/capture, reuse the established codec/amp setup, open the voice AFE ports and restore media on END or failure. Confirm restart/timeout teardown before releasing any session-mapped memory. None of these requires NV, EFS, persist or protected partition writes.

If Android confirms these tuples and supplies the local non-identity calibration/format metadata, extend the addressed APR voice receiver to return created session handles, create the matching passive MVM/CVS sessions, configure dual control, create/calibrate CVP and attach the vocproc, then start voice. Only after that should controls begin returning success. Passive sessions must follow the modem's LTE/codec negotiation rather than converting the call into the vendor full-control VoIP path.

### Validation and kernel artifact

Passed: `tools/test-voice.py` (actual observer/control worker, ASan/UBSan), `tools/test-voice-audio.py` (actual mailbox APR and ADSP mapping code, ASan/UBSan), `tools/test-modem.py` (including the actual PCIe voice mapper), `tools/test-gnss.py`, `prepare.py verify`, `/Users/xm666/X/umi-ios/work/umi-mic/check-host.py` and `check-capture3.py`. New cases cover exact APR wire bytes, foreign/late/accepted/malformed/error/timeout replies, low-physical-memory constraints, nonoverlapping ADSP PTEs/SID selector, PCIe mappings across a 2 MiB L3 boundary, unchanged offload context, one-call ownership, LTE/GSM/IMS entry points, duplicate indications, END during setup, switch-off and failure latching. The legacy mic checks still read the research checkout: its mic sources match the current overlays; the new memory tests exercise the changed sound include directly. ARM cache maintenance is removed only from the host-generated test copy, so DMA coherency and TLB behavior remain hardware acceptance checks.

Kernel builds always ran under `/Users/xm666/X/umi-ios/build-lock.sh`. The default build linked but sandbox denied the dSYM recipe's `/dev/fd` process substitution. Passing the existing make option `BUILD_DSYM=0` completed the kernel build; no source/build-script/boot-argument change was required. Full linking is no longer blocked by the earlier WLAN/network errors in the current shared tree. The final release build explicitly removes only the four owned compilation products before running the normal device build, forcing independent compilation of `hakios.adsp.o`, `hakios.sound.o`, `hakios.umi_modem.o` and `hakios.umi_baseband.cpo`. Log: `/Users/xm666/X/umi-ios/logs/build-umi-voice-audio-release.log`. Candidate path: `out/xiaomi-mi-10/flash/kernel-voice-audio`. The kernel necessarily includes the shared tree's other agents' current work. No payload/loader build or phone access was performed.

### Supervisor acceptance checklist

1. Verify the candidate hash recorded below and boot the frozen `kernel-voice-audio` through the supervisor's existing procedure, using the complete established driver set and userland; add no boot arguments. First leave `/var/db/hakios-voice/on` absent and check data, SMS, Voice Memos and ordinary receiver/speaker playback. No mailbox allocation/handshake or `voice-audio:` stage log is expected while disabled.
2. Enable the existing switch as a regular file and allow two seconds for sampling. This alone does not send the mailbox command: a connected QMI voice indication or the IMS owner's explicit modem-media start is required. Do not infer an audio failure from the absence of these triggers, and do not resume the failed GSM experiment at this location.
3. On a real connected modem-media VoLTE call, require `memory rc=0`, `mhi-offload rc=0`, and especially the real `mailbox rc=0 dsp=0` ACK. Check for new SMMU/MHI faults and verify data/SMS continue. An ACK is a mailbox milestone only: **silence is expected**, followed by `session-route-unavailable rc=45` (Darwin ENOTSUP). Do not report two-way audio or a working VoLTE media route from this kernel.
4. End the call or remove the switch. Require one `stop rc=0` for the owned call, no subsequent session work, no repeated initialization on duplicate indications, and continued data/SMS/media operation. Mailbox/domain memory remains pinned by design. Reboot before retrying a transport failure or testing after an ADSP/modem restart.
5. If `mailbox rc=60` (timeout), a nonzero `dsp=` result, memory/PCIe mapping failure, MHI fault or media regression occurs, retain only the `voice-audio:` lines and generic transport fault/result metadata. No raw mailbox, QMI dial/SMS, SIM, SIP identity or conversation capture is required. Collect the Android tuples/calibration/AFE metadata above before implementing CVP. Future acceptance remains intelligible two-way speech, protected receiver/speaker switching, mute/volume, echo/latency, media interruption/resume and repeated call cleanup.

Illustrative **synthetic** good milestone logs (not phone evidence):

```text
voice-audio: memory rc=0
voice-audio: mhi-offload rc=0
voice-audio: mailbox rc=0 dsp=0
voice-audio: session-route-unavailable rc=45
voice-audio: stop rc=0
```

Bad examples, all distinct from the expected ENOTSUP stop point:

```text
voice-audio: memory rc=12
voice-audio: mhi-offload rc=6
voice-audio: mailbox rc=60
voice-audio: mailbox rc=5 dsp=3
```

Frozen artifact: `out/xiaomi-mi-10/flash/kernel-voice-audio`, SHA-256 **`fa87894c741eef2ecad2a6ad0754bf5e6fd9cec22c2082a2c2aacacff1dbdb54`**. Final locked build exit: **0**. Review patch and source/object manifest: `/Users/xm666/X/umi-ios/work/voice-audio/milestone-1b.patch` and `manifest.json`. These describe only the audio continuation; the old `work/voice-cs/milestone-1a.patch` remains unchanged.

## Milestone 2 (voice sessions + route)

2026-11-11 continuation. 实现了 modem-owned VoLTE 的双向 DSP 路径；上一节的 `session-route-unavailable` 停点已移除。当前交付是可编译、可上机验收的实现，双向可懂语音由 supervisor 真机验收。默认不加载 ACDB，不向 modem 设置网络、AMR/EVS 编码或时序。

### 实现与来源

- `umi_voice_audio_apr.inc`：独立接收 ADSP MVM=9、CVS=10、CVP=11，检查 domain/service/port/token/回显 opcode，返回真实 create response source handle。GET CVD 等待 `0x11379` 版本响应；普通 basic ACK/ACCEPTED 不冒充版本响应。每条语音 APR 均写 `voice-audio: apr send/result` 到 loader live log，包含命令、服务、错误和 DSP 原始结果，不记录通话 key 或身份数据。
- `umi_voice_audio_session.inc`：按 `audio-kernel/dsp/q6voice.c` passive 分支，GET CVD → passive MVM/CVS → MVM dual-control（packed bool=1）→ 独占物理 AFE → CVP create → channel/media format → topology commit → enable CVP → attach vocproc → CVP volume/CVS TX mute → MVM START_VOICE。CVD <2.2 用 CREATE V2 `0x112bf`，>=2.2 用 V3 `0x13169`，沿用厂商相同的 packed payload。passive 会话不执行 full-control VoIP 的 ATTACH_STREAM、SET_NETWORK、SET_MEDIA_TYPE、packet exchange 或 AP RTP。
- 默认绑定 VoiceMMode1 `0x11c05000`，MVM/CVS name 均为 `11C05000`。legacy VoLTE VSID `0x10c02000` 对应厂商名 `default volte voice`；VoiceMMode2 对应 `11DC5000`。没有把 LTE RAT 枚举当作会话 VSID。
- 免提按 q6voice 的 MFC channel mixer 将单声道语音以 Q14 单位增益复制到 L/R，并设置 48 kHz stereo 输出格式。
- 默认 TX topology=`0x10f70`（NONE）、RX=`0x10f77`（RX_DEFAULT），来自 `voice_get_topology()` 无校准 fallback。CVP 使用 calibration network NONE=`0x1135e` 和 internal EC mixing=`0x10f7c`，参考端口 NONE=`0xffff`。TX NONE 是直通；可选择厂商 SM_ECNS_V2 `0x10f89` 做回声处理试验。端点固定 48 kHz/16 bit，modem/CVD 负责协商的窄带/宽带语音与端点间转换。
- `umi_voice_audio_route.inc` / `hakios.sound.c`：`take/set/release` 是明确的路由所有权 API。先暂停媒体写入，等待 feeder 和 watcher 均停稳，再停 amp、关闭媒体 ASM/ADM、停止 AFE，最后交接给通话。通话音频直接由 CVP 驱动 AFE，不进入媒体的 8-period PCM 队列。END 或 setup 失败按 STOP → DETACH → DESTROY CVP → DESTROY CVS → DESTROY MVM → 关闭通话物理端点 → 重建媒体 ASM/ADM/AFE 的顺序恢复；恢复时丢弃中断期间的媒体积压。
- `umi_mic.c` 的最小所有权改动：先 publish voice claim，使正在运行的媒体 capture 正常退出/关闭；取得 `mic_lock` 后复用已经验证的 BOB、codec reset、TX macro、ADC1、SoundWire 和 `TX_CDC_DMA_TX_3=0xb037`，不创建录音 ASM/ADM、不把通话麦克风交给 `/dev/hakios-mic`。释放后仍打开的媒体 capture 自动恢复。
- `umi_voice_audio_receiver.inc`：厂商 static overlay 的 handset 为 WCD9385 EAR：RX0 → RX INT0 → SoundWire RX master1/HPHL → WCD RX1 → RDAC3 → EAR，AFE=`RX_CDC_DMA_RX_0=0xb030`（来自厂商 `apr_audio-v2.h`）。DT 指定 RX macro `0x03200000`、RX SWR `0x03210000`、LPI GPIO3/4/5、RX slave ID `000d01170224`。采用厂商 EAR Class AB 供电/PA 顺序，独立 RX clock IDs `0x30e/0x30f`，保存并恢复修改过的 codec、macro、引脚与时钟 mux，保留 TX 时钟位；默认 EAR 数字增益最高 -6 dB，PA gain 为厂商 G_6_DB。没有把扬声器当作听筒。
- 免提复用 `TERT_MI2S_RX=0x1004`、CS35L41 的既有启动、OTP/board limit、固件保护及健康状态（worker 每秒检查，fault 后停止语音并恢复媒体）。直接语音不经过 CPU guard，因此附加至少 -30 dB 数字衰减，且不能突破既有 `sound_attenuation()` 上限。音量 0 时听筒数字 mute，扬声器设为 -102 dB；CVP 同时接收音量请求。
- `umi_voice_audio_control.inc`：QMI/IMS caller 只排队；文件读取、内存、APR、物理路由都在原 switch worker，且不持有 `bb_lock`。所有权、重复 CONVERSATION、END、开关关闭、mute/volume 和 route revision 均由状态机处理。route 变更重建会话，会短暂中断语音。失败返回实际 errno，不确认未完成的控制。

MVM/CVS/CVP create timeout 或无法解析的 create reply 会留下未知句柄，记录 `unresolved-create` 并拒绝重入。STOP/DETACH/DESTROY 失败保留当前资源及 AFE 所有权；显式 STOP 可重试剩余步骤。此时不会让媒体同时接管 DSP 端口。全局 mailbox/DMA 映射仍沿用 milestone 1b 的 pinned lifetime，不释放给可能仍访问它的 firmware。

本轮未改 IMS、baseband bridge、Wi-Fi/热点、显示/GPU、loader、CommCenter 或 boot-args。MHI 和 mailbox 的双域映射沿用 1b，无需另建 channel-80 ring。

### 开关文件

所有新增启动行为要求 `/var/db/hakios-voice/on` 是常规文件。参数在创建新会话时读取；修改后结束当前通话再重拨。内容除说明外均为文本数字，不新增 boot-args。

| 文件（目录 `/var/db/hakios-voice/`） | 默认 | 用途 |
|---|---|---|
| `on` | 不存在 | 总开关；删除后排队 STOP 并恢复媒体 |
| `vsid` | `11c05000`，十六进制 | 可选 `11dc5000` 或 legacy VoLTE `10c02000`；选后者会使用其厂商专用会话名 |
| `tx-topology` | `10f70`，十六进制 | TX NONE；可试 `10f89` SM_ECNS_V2 |
| `rx-topology` | `10f77`，十六进制 | RX default |
| `cvp-api` | `2`，十进制 | 2 使用 generic TX/RX channel-info 参数；1 使用旧 TOPOLOGY_SET_DEV_CHANNELS。CVD 版本始终实查并决定 CREATE V2/V3 |
| `receiver-clock` | `19200000`，十进制 Hz | RX MCLK/NPL 采用 48 kHz 系列时钟；DT 原始 RX ref-clock `22579200` 可选择对照；记录实际 AFE result 和所选频率 |
| `volume-steps` | `5`，十进制 | CVP RX step=`(100-percent)*steps/100`，20 ms ramp；物理增益上限仍生效 |

ACDB 文件已在 host 检查，Handset/Speaker 包含 `VSTI*`（static）、`VDYI*`（dynamic）及 `DATAPOOL`，Global 包含 `VS2I*`（stream）索引。它们不是可以直接塞进 APR 的 calibration payload；本轮使用明确的无校准路径，没有引入 `acdb-on` 加载器。默认 TX NONE 没有回声消除，首次上机先验证双向语音，再用 topology 开关测试固件内置 ECNS。

### IMS / iOS 接口契约

头文件为 `overlays/xnu/files/bsd/dev/umi_voice_audio.h`。只在 **modem 媒体的已接通通话** 调用 START；不能与 AP RTP 音频并行。内部 call key 不使用号码或 SIP identity。

```cpp
// 已持 bb_lock 的 IMS/QMI dispatch：
int rc = umi_voice_audio_start_locked(key, UMI_VOICE_LTE, UMI_VOICE_RECEIVER);
// END / cancel，同样持 bb_lock：
rc = umi_voice_audio_stop_locked(key);

// 以下接口均在 bb_lock 外调用：
rc = umi_voice_audio_status(key);
rc = umi_voice_audio_mute(key, 1);                 // 只静音上行
rc = umi_voice_audio_volume(key, percent);         // 下行 0..100
rc = umi_voice_audio_route(key, UMI_VOICE_SPEAKER); // 免提
```

START、STOP、发生变更的控制首先返回 EINPROGRESS（Darwin 36），caller 在 worker 完成后查询 status。status=0 才是请求完成；START 的 0 表示 MVM START_VOICE、mute/volume 和物理路由均成功，STOP 的 0 表示销毁与媒体恢复均成功。status 不区分 running/stopped，caller 知道自己最后提交的是 START 还是 STOP。相同 START 幂等；其他 owner 返回 EBUSY，非法参数返回 EINVAL，未启用时 START/controls 返回 EPERM。STOP/status 可在开关删除后继续用于清理。caller 不可将 EINPROGRESS 当成 audio-ready。

### Supervisor 真机验收

1. 使用末尾冻结产物和 SHA256；首先不放 `on`，检查语音备忘录录放、媒体播放、数据和 SMS。随后创建 `on` 并等两秒采样。启用本身不创建语音会话；要由 IMS agent 的 connected/modem-media START 或 QMI CONVERSATION 触发。这里仅测试 VoLTE，不回到已失败的 GSM 实验。
2. 在普通 VoLTE 通话中，对照 live log：首次有 `memory rc=0` → `mhi-offload rc=0` → mailbox `op=0x1333b ... rc=0 dsp=0` → CVD GET `op=0x11378` 的完成与 `cvd=24` 等实查值 → MVM `0x110ff` / CVS `0x11140` 的非零 `handle` → dual `0x11327` → `owner media->voice rc=0` → receiver/route rc=0 → CVP CREATE、channel、commit、enable、attach、volume、mute、START 的 result → `running rc=0 ... running=1`。先测试听筒和主麦克风双向可懂语音，不采集音频。
3. 通过 iOS 通话界面/IMS glue 分别静音和解除上行：CVS `0x1138b` 应 `rc=0 dsp=0`，接收方确认静音时听不到麦克风，解除后恢复。调节音量：CVP `0x112c2` 和 `route=... volume=... rc=0`；0 应静音下行。切换免提，确认旧会话 STOP/DETACH/DESTROY、媒体交接、新 route/create/start 完成，且 CS35L41 无 fault、音量不突破上限。对比 TX NONE 与选用 SM_ECNS 时的回声。
4. 挂断后要求 STOP `0x11192` → DETACH `0x1123f` → CVP/CVS/MVM `0x1003c` → 接收/麦克风 AFE stop → `owner voice->media rc=0` → `stopped rc=0 ... running=0`。立即验证语音备忘录与媒体可再次使用；重复接打、切换听筒/免提，检查没有双 AFE owner、悬挂会话或 SMMU/MHI fault。
5. 通话中删除 `on`，要求同样的 teardown/恢复。失败时保留 `voice-audio:` 命令与结果元数据；不抓取音频、号码、SIM/SIP 身份或 mailbox 内容。未知 create、transport latch、DSP/基带重启后重启系统再试。调整开关值后在新通话验证，不通过 NV/EFS/persist 修改。

APR 结果含义：`rc=0 dsp=0` 为对应命令真实完成；create 还必须返回非零 session handle。`dsp=1/2/3/4/5/6/7` 分别是 ADSP failed/bad-param/unsupported/version/unexpected/panic/no-resource，完整 DSP 原值保留在日志，不能当作成功。Darwin `rc=5` 表示收到 DSP NACK（看 dsp），`rc=60` 为超时，`rc=100` 为 EPROTO（坏响应），`dsp=4294967295` 表示没有合法 basic result；CVD 的专用版本响应按专用格式验证。AFE 的 `reply` 为真实响应 opcode，basic 响应的 `dsp` 才是固件状态；`rc=0` 但非零 dsp 仍是 NACK。只有最终 `running rc=0` 说明全部启动步骤完成，不能从 mailbox ACK 推断出声。

### Host 验证与构建

`tools/test-voice-audio.py` 统一执行 APR、DMA、真实 session/control worker、路由及 receiver 测试（ASan/UBSan）。包含 MVM/CVS/CVP wire payload、CVD 2.1/2.2/2.4 分支、VSID/legacy session name、逐条 setup NACK、丢失 create、STOP 重试、重复 indication、END/switch-off、媒体暂停/恢复、听筒/免提增益限制、RX 写入失败及 codec/TX 状态恢复。另跑 modem regression 与 `prepare.py verify`。这些测试不连接手机。

构建只使用 `/Users/xm666/X/umi-ios/build-lock.sh` 包装 `bash devices/xiaomi-mi-10/build-kernel.sh -j8`，使用现有 `BUILD_DSYM=0` 选项。构建日志：`/Users/xm666/X/umi-ios/logs/build-umi-voice-audio2.log`。冻结产物与最终结果见下方。

最终锁定构建退出码：**0**。音频、modem、GNSS、既有 mic host checks、`prepare.py verify` 全部通过。冻结内核：`out/xiaomi-mi-10/flash/kernel-voice-audio2`（18165264 bytes），SHA256：**`c7371a2c70cdf5a40ef098526d2c62d2349e8fb1b7833bc12fdbf18df93f6846`**。锁内已核对 overlay 与实际 XNU 编译树一致，并确认内核晚于五个相关对象文件。源文件/对象 SHA256 清单：`/Users/xm666/X/umi-ios/work/voice-audio2/manifest.json`。本轮没有访问手机；待真机完成第 2–5 项的声学与生命周期验收。
