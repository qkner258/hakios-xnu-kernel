# Personal Hotspot on umi

Research date: 2026-11-11. Target: iOS 16.5 / 20F66, QCA6390, SDX55.
Local source and binary inspection only; no builds or device access. Existing
cellular data and USB NCM operation are inputs from the task, not retested here.

## Verdict

Feasible. The cheapest useful result is IPv4 USB tethering: route the existing
NCM `en2` through PF NAT to `pdp_ip0`. Budget roughly 2–4 engineer-days, assuming
the compiled PF implementation works at runtime. Full native Personal Hotspot
integration is a separate job, likely another 1–2 weeks or more.

Wi-Fi AP is supported by the imported QCA6390 driver, but is not implemented by
the shim. This is weeks of work, not an ioctl toggle. Reuse the native iOS
`hostapd` authenticator if its driver ABI can be satisfied. Bluetooth PAN is a
third option, pending investigation of its Ethernet interface handoff.

## Evidence notation

Paths below are relative to the repository root. Abbreviations:

- `R` = `work/umi-v23`.
- `D` = `R/devices/xiaomi-mi-10`.
- `A` = `R/xnu-deps/build/ath11k` (symlinks into the retained research wifi-build).
- `F` = `R/sysimage/rootfs`.
- `C` = `tools/tmp/dsc-20F66`.
- `IO80211` = `C/System/Library/PrivateFrameworks/IO80211.framework/IO80211`.
- `MobileWiFi` = `C/System/Library/PrivateFrameworks/MobileWiFi.framework/MobileWiFi`.
- `WMS` = `C/System/Library/PreferenceBundles/WirelessModemSettings.bundle/WirelessModemSettings`.

Source references use `file:line`. Binary addresses are unslid VM addresses;
`+0x...` denotes a file offset reported by `strings -t x`. Binary evidence came
from `nm`, `strings`, LLVM disassembly, and plist inspection. An available
request or string does not prove a successful runtime path.

## 1. Wi-Fi AP: hardware support, missing host logic

The actual shared shim files are `R/drivers/wifi/hakios.{lx,sta,apple80211,wifi}.c`
and `hakios.wlan.cpp`; there is no `D/drivers/wifi` directory. Device Wi-Fi
overrides are in `D/overlays/wifi/lx/src`; staging copies them over the shared
tree (`D/tools/stage-wifi.py:19`).

The imported ath11k has the necessary firmware-facing AP primitives:

- `A/src/ath11k/core.c:219` identifies QCA6390 hw2.0; its interface modes include
  AP at `:256–260`. `wmi.h:5085` defines `WMI_VDEV_TYPE_AP = 1`.
- `mac.c:7152` translates AP to WMI AP vdev type. Beacon template upload is at
  `:1621–1637`; beacon enable/vdev-up handling is at `:1693–1729` and
  `:3619–3622`. AP SSID configuration is at `:3607`.
- `mac.c:9947` implements station-state transitions, including AP peer
  association at `:10007–10010`; `:10100–10102` wires key/station operations.

This is driver support, not proof that the loaded firmware accepts AP operation
on this port. A concrete software blocker precedes that test:
`A/freebsd/sys/compat/linuxkpi/common/include/net/mac80211.h:1604–1609`
implements `ieee80211_beacon_get_template()` as TODO/NULL. ath11k rejects that
NULL at `mac.c:1624`. An empty generated stubs list does not catch inline stubs.

The device shim is explicitly STA-only:

- `D/overlays/wifi/lx/src/lx_80211.c:965` hardcodes station vif type; the radio
  holds one peer/WPA/join context (`:96–131`, `:427`).
- Its management RX handles the selected AP's authentication/association
  responses, not incoming client requests (`:852–869`).
- It consumes all EAPOL internally and passes ordinary data only when joined
  (`:803–847`). `lx_wpa.c:597–602` answers authenticator M1 with M2;
  `:654–669` receives GTK and answers M3/group-key messages. It does not generate
  AP M1/M3, manage authenticating clients, or originate/rekey a GTK.

Required AP work: vif/channel/start/stop; beacon/probe-response templates with
RSN/TIM/DTIM; client authentication/association, AIDs, station table and teardown;
ath11k peer/key lifecycle; per-peer TX/RX and controlled ports; group-key TX,
multicast and power-save buffering; EAPOL delivery to an authenticator. DHCP,
DNS and routing are separate IP services and can be shared with USB tethering.

Start with one AP, legal non-DFS 2.4 GHz HT20, WPA2-PSK/CCMP, cellular upstream.
Do not assume simultaneous STA+AP from firmware vdev counts: the shim's single
context needs refactoring even if firmware concurrency is supported.

## 2. Native iOS AP control surface

The native control chain includes Settings/Netrb, misd, MobileWiFi/wifid,
IO80211 and hostapd. `MobileWiFi` exports
`_WiFiManagerClientIsTetheringSupported` at `0x1b2fbf8d0`,
`_WiFiDeviceClientIsInterfaceHostAp` at `0x1b2fb9ad0`, and
`_WiFiDeviceClientRegisterHostApStateChangedCallback` at `0x1b2fbc154`.
WMS contains `SOFTAP_ENABLE`, `SOFTAP_IMMEDIATE_DISABLE` and
`SOFTAP_FORCE_2_4G_CHANNEL`.

IO80211's request descriptor table establishes these numbers. The call-site
column distinguishes disassembled calls from names merely present in the ABI.

| Request | Meaning | Binary evidence |
| --- | --- | --- |
| 25 | HOST_AP_MODE | wifid Set `0x100042fa8`, `0x100043000`; IO80211 table `0x1dab50050` |
| 26 | AP_MODE | Descriptor present; no enabling call established |
| 94 / 95 | VIRTUAL_IF_CREATE / DELETE | IO80211 handlers `0x1d30d70fc` / `0x1d30d71b4`; wifid delete `0x10004fe28` |
| 336 | HOST_AP_MODE_HIDDEN | wifid Set `0x1000461dc` |
| 347 | SOFTAP_PARAMS | wifid Get `0x1000462f0`, Set `0x100046380` |
| 349 / 350 | SOFTAP_TRIGGER_CSA / STATS | Descriptors; wifid stats Get `0x100046500` |
| 352 | SOFTAP_WIFI_NETWORK_INFO_IE | wifid Set `0x1000466bc` |
| 363 | USB_HOST_NOTIFICATION | Descriptor present |
| 403 | SOFTAP_EXTENDED_CAPABILITIES_IE | wifid Set `0x100046864` |
| 420 | NANPH_SOFTAP_CSA | wifid Set `0x1000469f0` |
| 42 / 73 | STATION_LIST / STA_IE_LIST | Descriptors; hostapd Get 73 at `0x100024ed8` |
| 74 / 75 / 76 | STA_AUTHORIZE / DISASSOCIATE / DEAUTH | hostapd RawSet `0x100023e80` / `0x100023c60` / `0x100023bdc` |
| 77 / 78 | RSN_CONF / KEY_RSC | hostapd RawSet `0x100023824`, RawGet `0x10002393c` |
| 3 | CIPHER_KEY | hostapd RawSet `0x1000229e0`, payload 148 bytes |

These are candidate/observed native paths, not a captured toggle trace or a
claim that all requests are mandatory on every activation. Preserve the actual
versioned ABI; request 25 is not safely treated as a simple Boolean. IO80211's
create/delete handlers construct raw payloads of 32/20 bytes respectively.
wifid's `__WiFiDeviceCreateHostApInterface` diagnostic and call near
`0x100048a5c` also show a virtual HostAP role/discovery path.

IO80211 imports `IOServiceMatching`, registry property APIs, `IOServiceOpen`,
`IOConnectCallStructMethod` and `IOConnectCallAsyncMethod`. Its binding code
contains “Falling back to IOCTL path” at `0x1d30dddfc`. Exact user-client selector
numbers were not established here; imports alone do not establish which branch
runs on umi. The existing fallback uses `SIOCSA80211=0x802869c8` and
`SIOCGA80211=0xc02869c9` (`R/drivers/wifi/hakios.apple80211.c:47–55`).

The current driver advertises STA (`hakios.apple80211.c:324`), supports only
associate/disassociate/power/scan setters (`:570–594`), and publishes only an
Infrastructure role (`hakios.wlan.cpp:151–164`). `newUserClient` returns
unsupported (`:86–94`). AP needs real ioctl behavior, virtual-interface
publication/discovery and events; names in the ioctl logger are insufficient.
Implementing a full IOUserClient may be avoidable if the fallback handles AP.

### Reuse the shipped authenticator

`F/usr/libexec/hostapd` already contains `wpa_auth` (`+0x2d6bd`) and
“rekeying GTK” (`+0x2e355`), and imports Apple80211 RawGet/RawSet, bind and event
monitoring. IO80211 builds a hostapd configuration with SSID/channel/WPA keys and
`wpa_group_rekey=3600` (`0x1d30df434`), writes `/var/run/hostapd.conf`
(`0x1d30df460`), and starts XPC service `com.apple.wifi.hostapd`
(`0x1d30df48c`). Its launch plist exists in `F/System/Library/LaunchDaemons`.
Include that on-demand job explicitly when integrating: the default job
generator filters labels containing `wifi` (`R/userland/mkjobs.py:216`).

Correction from the 20F66 event-name table: hostapd subscribes to STA_ARRIVE
(12), STA_LEAVE (13), RSN_HANDSHAKE_DONE (20) and AUTH_IND (152) at
`0x100024130–0x100024160`, plus power/link/SSID events. Events 2/4 are
SSID_CHANGED/LINK_CHANGED, not station arrival/departure.
Expose EAPOL to its userspace transport instead of swallowing it in the STA
supplicant, and map its key/RSN/authorization requests and station events to
ath11k. This can supply authenticator M1/M3, PTK/GTK generation and rekeying;
compatibility is unproven until exercised. Alternatively port hostapd with a
custom shim backend; a Linux nl80211 backend cannot work unchanged here.

## 3. misd, routing, provisioning and the Settings row

`F/usr/libexec/misd` identifies itself as MobileInternetSharing-271.120.2
(`+0x22a10`). Its launch plist runs it at load in `/var/run`, publishing
`com.apple.MobileInternetSharing`. Netrb.framework provides client APIs.

The native service needs:

- IPv4 forwarding and PF rules/state. misd imports `PFUserCreate`,
  `PFUserBeginRules`, `PFUserAddRule`, `PFUserCommitRules`, and state cleanup;
  it logs forwarding changes at `+0x29500`. It also uses SystemConfiguration
  preferences/dynamic state and Network interface state (`nwi_state_*`).
- DHCP configuration at `/Library/Preferences/SystemConfiguration/bootpd.plist`
  (`+0x2852c`) and `/usr/libexec/bootpd` (`+0x28562`). The binary is present;
  configuration strings include `dhcp_enabled`, `Subnets`, router and DNS.
- DNS proxy service: imports `mrc_dns_proxy_*`, including input/output interface
  selection. Merely assigning a private address does not provide DNS/DHCP.
- CoreTelephony tethering status and activation. Imports include
  `CTServerConnectionTetheringAssertionCreate`; selectors include
  `getTetheringStatusSync:` and `setTetheringActive:`. The status log at
  `+0x2a0ff` lists carrier enabled, user authenticated, connection available,
  maximum hosts and interface name. A working BSD `pdp_ip0` alone does not
  satisfy this state machine.

PF is already compiled: `R/out/xiaomi-mi-10/xnu/config/MASTER:66` enables PF,
and `BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/pf.h:1` defines PF=1.
In that generated XNU tree, `bsd/net/pf_ioctl.c:501–512` registers `/dev/pf`
and hooks. Runtime device availability and stock PacketFilter.framework ioctl
compatibility remain untested. Reuse PF before considering a new NAT engine;
a small helper using the target API is sufficient for the first milestone.

CommCenter is currently denied by `R/userland/mkjobs.py:216`. Cellular data
from the QMI bridge does not imply native CT tethering status exists. Native
integration will need a focused status/activation adapter or a supported
explicit-upstream Netrb path. misd contains non-PDP/external-interface logic,
but a carrier-independent invocation has not been proven. Porting all of stock
CommCenter should not be a prerequisite for the first USB router.

Carrier policy is separate from executable code-sign entitlements. The local
CommCenter binary (`tools/tmp/cc-activation-20F66/CommCenter`) contains
`AllowTethering` (`+0x125a9a2`),
`EnableTetheringWhileEntitlementStatusIsUnknown` (`+0x1258283`) and
`kCTCarrierEntitlementKeyTethering` (`+0x1263217`). misd's own entitlements
include `com.apple.pf.allow`, `com.apple.MobileInternetSharing.allow`,
CommCenter SPI/carrier-settings, SystemConfiguration writes and DNS proxy.
Any custom native client needs the appropriate trusted execution/entitlements.

China Mobile evidence comes from `R/sysimage/rootfs.dmg`, members
`System/Library/Carrier Bundles/iPhone/CMCC_cn.bundle/{carrier.plist,overrides_D20_D21_D22.plist,overrides_D201_D211_D221.plist}`,
read to stdout without extraction. Base `apns[0]/configuration` has `cmnet`
type-masks 51/50 (including bit 0x20); overrides have 1081395/1081394.
These are APN configuration evidence, not proof of subscriber permission.
No explicit `AllowTethering` was found in those three plists. Overrides contain
`CarrierEntitlements/SupportedEntitlements=1024`, protocol version 2 and an
entitlement service configuration; the meaning of bit 1024 was not decoded.
Runtime carrier entitlement, selected bundle/APN and plan permission remain
unknown; these image members also do not establish that the bundle is deployed
in the current generated root. Do not assume editing one carrier key enables
native tethering.

Settings visibility depends on Netrb/MIS state, not just hardware capabilities:

- `Preferences.framework/Preferences`, `shouldShowTethering` at `0x1a283bc04`,
  reads Netrb state and returns true for state >1021 (`cmp 0x3fd`, `cset hi`).
  `isTetheringEnabled` at `0x1a283bc3c` tests state 1023.
- WMS `_specifiersWithSpecifierMain:` at `0x208047fac` reads MISManager state;
  state 1020 omits its row. `updateSpecifiersForState...` at `0x208049fe8`
  handles state 1021 with `MAIN_SPEC_UNPROVISIONED`/setup, and provisioned state
  with `MAIN_SPEC_PROVISIONED`. The main Settings visibility test can therefore
  hide a row even when the bundle can construct an unprovisioned setup entry.
- `PreferencesUI` main controller `updatePersonalHotspot` at `0x2068f848c`
  also checks cellular/data/airplane state; modification restrictions apply.

## 4. USB first; Bluetooth third

Current NCM is a sufficient Ethernet transport for routed sharing.
`D/drivers/umi/hakios.usb.c:176–178` selects unit 2 (`en2`);
`:454–476` supplies CDC-NCM descriptors. `D/overlays/xnu/files/bsd/dev/hakios.net.c:456–481`
creates a legacy BSD Ethernet interface, MTU 1500 (`:509`) and BPF (`:521`).
`R/USB-umi.md:988` documents `172.16.42.1/24` on the phone and `.2` on the host,
with no DHCP in that milestone. Preserve that subnet for the first experiment.

The upstream is genuinely raw cellular IP:
`D/overlays/xnu/files/bsd/dev/umi_modem_net.inc:105–129` creates `pdp_ip0`,
family/type CELLULAR, no Ethernet header, modem MTU/address. Use routing/NAT,
not an Ethernet bridge to the modem.

However, NCM does not currently look like native iOS USB Internet sharing:

- `hakios.net.c:526–533` publishes IOKit network services only for wireless.
  There is no corresponding USB Ethernet registry tree for `en2`.
- `F/System/Library/UserEventPlugins/USBEthernetSharing.plugin/USBEthernetSharing`
  matches `IOEthernetInterface` (`0x1488`), looks for `AppleUSBEthernetDevice`
  (`+0x3d76`), uses `HostAttached` (`+0x3e32`), `LinkStatus` (`+0x3aa9`) and
  `kAppleUSBEthernet_Enabled` (`+0x3e9e`), and registers interest callbacks.
- It calls `_lockdown_is_host_trusted` (`0x1978`, `0x2540`), Netrb create/start
  (`0x1df8`, `0x1e74`), and requests the `AppleUSBEthernet` USB function
  (`0x1fb0–0x1fbc`). Existing AppleUSBDeviceMux publication is a different role.
- `F/System/Library/LaunchDaemons/com.apple.usb.networking.addNetworkInterface.plist`
  watches `IONetworkInterface`, with allowed classes including
  AppleUSBNCMData/AppleUSBDeviceNCMData and Apple USB Ethernet classes.

Native USB automation therefore needs registry publication, attachment/link
notifications, gadget enable/disable mapping, configd visibility, trust handling,
Netrb lifecycle and working upstream CT state. Full Apple USB-controller
emulation or changing the existing USB identity is unnecessary for direct NAT.
Host OS NCM driver compatibility is a separate concern.

Bluetooth: `F/usr/sbin/bluetoothd` contains BNEP/PAN-NAP/PANU code (e.g. BNEP
Ethernet processing at `+0x6e3595`) and imports Netrb create/start/add-host.
`D/overlays/xnu/files/iokit/Kernel/hakios.bluetooth.cpp:1–3,25–39` publishes
controller identity and describes H4 transport, not a PAN Ethernet interface.
Working pairing/HCI does not establish a BSD BNEP data path. Audit that handoff
first; PAN is likely slower than USB/Wi-Fi and not the cheapest starting point.

## 5. Ordered implementation plan and estimates

These are engineering estimates for one developer, not measured results.
Implementation and device validation are future supervisor work.

| Order | Deliverable / acceptance | Effort |
| --- | --- | --- |
| 1 | Static host `.2`, gateway `.1`; forwarding and narrow PF NAT from `172.16.42.0/24` on en2 to pdp_ip0. Verify host Internet and return traffic; retain diagnostics. | 0.5–2 days |
| 2 | DHCP, DNS, modem route/MTU handling, reconnect, explicit enable/disable and rule/state cleanup. Verify unplug/replug and cellular reconnect. | +1–2 days; usable USB about 2–4 days |
| 3 | Native USB/misd/Settings integration: CT status/activation adapter, registry/trust glue, Netrb and configd state. Verify native row, switch and upstream lifecycle. | +1–2 weeks or more |
| 4 | Wi-Fi AP proof: implement beacon helper/templates, AP vdev/channel and one open test-client association, isolated from production use. | 3–5 days |
| 5 | Per-client data/key path and native hostapd ABI/events/EAPOL; WPA2 client plus GTK rekey. Reuse USB DHCP/DNS/PF. | +1–2 weeks |
| 6 | Multi-client, power-save, teardown, regulatory behavior and native UI integration. | +1–3 weeks; WPA2 MVP roughly 2–4 weeks, polished native AP roughly 4–8 weeks total |
| 7 | Bluetooth PAN interface audit, then BNEP/NAP-to-BSD glue if practical. Reuse routing services. | Audit 0.5–1 day; implementation perhaps 1–2 weeks, conditional |

Main risks: unexercised firmware AP support; further mac80211 inline stubs;
singleton station assumptions; group rekey/multicast/power-save correctness;
PF/framework ABI compatibility; missing configd/NWI upstream state; native
CommCenter/trust gates; modem address/route changes and MTU/PMTU behavior.
Start explicitly IPv4-only; IPv6 sharing needs a separate addressing/routing
design. Keep rules scoped to the downstream interface/subnet and remove them
on shutdown. USB-first resolves the shared IP layer before spending weeks on
the Wi-Fi control and authentication paths.

## Implementation (Wi-Fi AP)

2026-11-11: step 4 **source/build milestone**, awaiting supervisor device tests.
No device access was performed. The code implements an opt-in AP-only open
network, one client/AID 1, firmware beacons, probe responses, open-system auth,
association/reassociation, peer authorization/removal and BSD Ethernet RX/TX.
Compilation does not establish that this QCA6390 firmware beacons or accepts a
client. In particular, scanning, association, ARP and ping are still untested.
Step 4's on-phone acceptance and step 5/WPA2 are NOT claimed complete.

Resume review (2026-11-11): retained the existing AP implementation and rebuilt
it independently. Fixed authentication retries deleting/recreating the peer,
duplicate association success logging, and incomplete management IE parsing;
monitor-only/bad-FCS RX and unsupported fragmented/protected management frames
are ignored in AP mode. Pass the client's capability/listen interval to the
ath11k association fields. Fixed config parsing that accepted a channel on a
third line through strtol's whitespace skipping, and reject truncated output.
Repeated `--off` while already off now leaves STA untouched. If IPv4 setup
partially fails, the helper removes the address after requesting AP teardown.
The earlier build/hash was superseded by the reviewed artifact below.

### Files and boundaries

All source paths below are relative to `devices/xiaomi-mi-10/`:

| File | Change |
| --- | --- |
| `overlays/wifi/lx/src/lx_80211.c` | AP state, AP management/data RX dispatch, single AP peer/channel iteration, block en0 TX while AP owns the radio, register runtime AP operations. Existing on-disk STA channel fixes are preserved. |
| `overlays/wifi/lx/src/umi_wifi_ap.h` | AP vdev/channel lifecycle, real beacon template with SSID/rates/DS/TIM/ERP, directed/wildcard probe responses, auth/assoc/disassoc, one ath11k peer and en3 data path. |
| `overlays/wifi/lx/include/net/mac80211.h` | Device-local include wrapper replaces the NULL beacon-template inline helper for ath11k; pinned upstream headers are unchanged. |
| `overlays/xnu/files/bsd/dev/hakios.lx.c` | Shared frame queue tags AP frames separately, serializes runtime mode switches against TX, discards queued AP packets from an earlier AP session. |
| `overlays/xnu/files/bsd/dev/umi_wifi_ap.inc` | Root-writable runtime sysctl and BSD en3 link/input/output glue; already-off requests are no-ops. Included by hakios.lx.c; no new build-manifest entry. |
| `overlays/xnu/files/bsd/sys/hakios.lx.h` | Small config/callback ABI between the XNU half and LinuxKPI object. |
| `overlays/xnu/files/bsd/sys/hakios.net.h` | Reserve existing generic Ethernet link unit 3; en0/en1/en2 retain their units. No hakios.net.c/NAT/DHCP/DNS edits. |
| `overlays/userland/src/umi_wifi_ap.c` | Explicit config-file-to-sysctl helper; assigns en3 IPv4 and UP/RUNNING, removes the address on --off. No automatic job. |
| `tools/build-wifi-ap-helper.sh` | Standalone iOS arm64 helper build; no userland image/payload build. |
| `tools/test-wifi-ap-helper.c` | Host parser tests (including refusing a third passphrase line) and Darwin IPv4 ioctl ABI checks. |
| `tools/stage-wifi.py` | Seed from the retained research cache and repoint xnu-deps/build/ath11k to the device-local build, so XNU links the newly compiled AP object rather than the old research object. |
| `HOTSPOT-umi.md` | This implementation/test record. |

Downstream contract for the shared tethering layer: **interface `en3`, subnet
`192.168.2.0/24`, phone/gateway `192.168.2.1`**, Ethernet MTU 1500. Point its
downstream-interface setting at `en3` and use that subnet. This milestone does
not install forwarding, NAT, DHCP or DNS, and has no dependency on those services.
en3 is created on first explicit AP start and remains attached with link down
after stop. It is deliberately a normal BSD Ethernet interface without an
Infrastructure IOKit role/Apple80211 control publication, so wifid does not
mistake it for a second STA. Native HostAP publication is later ABI work.

### Runtime switch and helper

Default is `kern.hakios.wifi_ap=off`. No new boot arguments, automatic launch job,
or boot-time file polling were added. An existing file does nothing until the
supervisor explicitly executes the helper. SSID is 1–32 printable ASCII bytes;
channel is 1–11. File `/var/db/hakios-hotspot/wifi-ap`, root-owned, mode 0600:

```text
HakiOS-AP
6
```

Exactly two lines; a final newline is optional. **A third line, including a
passphrase, is rejected** before changing kernel state. WPA2 support must be
implemented before this file format grows a passphrase field. Contents and
passphrases are never logged by the parser. There is no fallback from a failed
protected configuration to open mode. The channel line contains only one or
two decimal digits: leading whitespace, signs and extra blank lines are invalid.

Build the helper from `R`:

```sh
bash devices/xiaomi-mi-10/tools/build-wifi-ap-helper.sh
```

Output: `out/xiaomi-mi-10/hotspot-ap/umi_wifi_ap`. Supervisor includes this
binary using the existing userland builder's `EXTRA` mechanism in the one merged
payload build; it will be `/redwood/bin/umi_wifi_ap`. No payload build was run
for this implementation. After the ordinary `wifi=1` driver is ready, execute
as root using the supervisor's test/job mechanism:

```text
/redwood/bin/umi_wifi_ap --apply
/redwood/bin/umi_wifi_ap --status
/redwood/bin/umi_wifi_ap --off
```

`--apply [path]` accepts an explicit alternative config path. Missing/invalid
files return failure and leave the current mode unchanged. `--apply` switches
to AP synchronously, then assigns `192.168.2.1/24`; an address-ioctl failure
stops the AP and returns failure. `--off` stops AP, restores STA availability,
requests its existing standing network again, and removes the en3 address.
Calling `--off` again leaves the restored STA alone, including its power state.
Deleting the file alone does not stop AP; use `--off`. Reapplying restarts AP
and disconnects its client. If called before driver registration, the sysctl
returns ENXIO; wait for the radio-ready log and invoke again explicitly.

Kernel control, for another helper: write ASCII `6 HakiOS-AP` (NUL optional)
to `kern.hakios.wifi_ap`; write `off` to stop. This only controls radio mode;
the caller must separately manage en3's IPv4 address. Reading returns the last
successfully requested setting, **not** live firmware/client status.

### Radio, concurrency and security decisions

The first version is **AP-only**, using the existing singleton radio/vif/peer.
It withdraws and drains STA callbacks, leaves the STA network, removes its vdev,
creates a WMI AP vdev (including ath11k's AP self-peer), then starts the channel
and uploads/enables the beacon template. Normal STA callbacks are absent while
AP runs. Disabling AP restores the ordinary STA interface and callback table;
supervisor must regression-test rejoin. No change to the default STA supplicant
or key installation code was made.

**Firmware STA+AP concurrency is unproven**, and this shim does not support it.
QCA6390's vdev counts/interface-mode bits are insufficient evidence of an allowed
concurrent combination. Cellular upstream does not need STA concurrency. Do not
enable two Wi-Fi roles in this implementation.

Channel width is fixed at 20 MHz; only legacy non-QoS rates are advertised and
negotiated for this proof (no HT/VHT/HE or WMM IEs). Beacons are every 100 TU,
DTIM 1, empty TIM. SSID/beacon interval/DTIM are passed to ath11k before channel
assignment/vdev start. Startup requires an installed firmware regulatory domain
and a channel without DISABLED, NO_IR or RADAR; refusal is EACCES and does not
override country rules. 5 GHz, DFS/CAC, country selection and runtime regulatory
changes while AP is active are outside this milestone.

Management RX is copied into a bounded queue and processed on a separate worker;
WMI peer creation/association must never block the RX/poll thread needed for
firmware completions. TX, management processing and stop are serialized. Stop
quiesces management producers/work, deauths/removes the peer, drains asynchronous
management TX before freeing the vif, disables beacons, unassigns/removes the
channel, removes the vdev and stops the radio. AP data is accepted only after
the single peer is associated/authorized, with its source MAC checked. AP EAPOL
is dropped rather than entering the unchanged STA supplicant.

**WPA2 decision:** target reuse of the shipped iOS hostapd; no in-kernel
authenticator was added in this milestone. Its existing authenticator/rekeying
code avoids introducing a new AP cryptographic state machine. Its driver ABI
has NOT yet been satisfied: en3 needs HostAP discovery/control, exact versioned
station/RSN/key request payloads (not Boolean request 25), station IE retention,
STA_ARRIVE/LEAVE/AUTH events and userspace EAPOL RX/TX. Authorization must then
wait for hostapd, and PTK/GTK must use ath11k set_key paths before opening the
port. Request 3/73/74/75/76/77/78 and event delivery need implementation and
runtime validation, along with explicitly including the hostapd on-demand job.
Do not enable hostapd and infer WPA2 from this open-only kernel. A minimal
in-kernel AP handshake remains an alternative if native ABI integration proves
impractical; there is no evidence yet that forces that alternative.

Remaining limits: no key/handshake or GTK rekey, multi-client, power-save
buffering/PS-Poll, client inactivity/auth timeout, quick-kickout handling,
aggregation, native Settings integration or automatic recovery. A client that
vanishes without deauth can occupy the only slot until AP is restarted. Use an
awake client for the first ping test. These are proof-stage limitations, not
production hotspot support.

### Exact log signatures

The following are format strings, not captured device results. `%02x` fields are
partly masked client MACs. Error integers are positive BSD errno values except
the imported ath11k driver's own diagnostics.

Successful requested lifecycle (channel/SSID length/vdev id vary):

```text
hotspot-ap: mode AP-only; STA withdrawn
hotspot-ap: vdev created type=AP channel=6 width=20
hotspot-ap: vdev up id=0 channel=6 width=20
hotspot-ap: beacon start interval=100 dtim=1 ssid-length=9 open en3
hotspot-ap: en3 192.168.2.1/24 configured; open AP, one client
hotspot-ap: assoc %02x:%02x:**:**:**:%02x aid=1 open authorized
hotspot-ap: handshake skipped security=open
hotspot-ap: disassoc %02x:%02x:**:**:**:%02x reason=%u
hotspot-ap: teardown complete
hotspot-ap: STA restore error=0
```

`vdev up` checks ath11k's real `arvif->is_up` after the WMI path; template send
and vdev-up failure do not produce a successful start. It still does not prove
RF beacon transmission or management-response delivery. There is intentionally
no handshake-success/failure log for WPA2, which is unimplemented; an open
association emits the explicit `handshake skipped` line instead.

Failure signatures:

```text
hotspot-ap: channel refused channel=%u flags=%#x regulatory=%u
hotspot-ap: start failed error=%d
hotspot-ap: configure failed error=%d
hotspot-ap: auth failed %02x:%02x:**:**:**:%02x error=%d
hotspot-ap: assoc failed %02x:%02x:**:**:**:%02x status=%u
hotspot-ap: peer remove failed state=%d error=%d
hotspot-ap: STA restore error=%d
hotspot-ap: control failed: %s
hotspot-ap: en3 address failed: %s
hotspot-ap: config open failed: %s
hotspot-ap: status failed: %s
hotspot-ap: config must be a root-owned regular file without group/other write
hotspot-ap: invalid config; expected SSID then channel, exactly two lines (open only)
```

Association status 9: authenticate first; 12: wrong/missing SSID or protected
request; 17: peer/firmware resource or state operation failed; 18: missing basic
rates. A second client's auth logs EBUSY (16) and is rejected with status 17.
Also inspect imported ath11k errors such as `failed to get beacon template from
mac80211`, `failed to submit beacon template command`, `failed to bring up vdev`,
`failed to get peer assoc conf event`, `failed to transmit frame`, and firmware
timeouts/crashes. They are not all routed through the new prefix.

### Supervisor on-phone checklist

1. Include the standalone helper in the supervisor's merged payload and boot
   the new `kernel-hotspot-ap` through the ordinary phone-test harness and
   existing full driver boot arguments. No new arguments. All phone operations
   belong to the supervisor; the implementation agent did none.
2. Before creating/applying a config, confirm `--status` reports `off`, en3 is
   absent, and ordinary STA scan, saved-network WPA2 join, DHCP and Internet
   behave as before. Also invoke `--off` with STA already joined, and with STA
   powered off: it must not disconnect/rejoin or power STA on. This is the
   default-path regression gate.
3. Prepare the root-owned mode-0600 two-line `/var/db/hakios-hotspot/wifi-ap`
   shown above. Wait for the normal radio-ready message and execute `--apply`
   using the supervisor's existing command/job mechanism. Save the result/log.
4. Require the vdev-up and beacon-start lines and the helper's en3 IPv4 line.
   For EACCES/channel refusal, record domain/channel flags and stop; do not
   bypass NO_IR or DFS. For another failure, retain ath11k diagnostics and
   confirm STA restoration. Do not treat a helper exit alone as RF proof.
5. On an awake Mac or another phone, scan for **HakiOS-AP**. Confirm channel 6,
   20 MHz, open security and the configured SSID. Capture beacons/probe responses
   if it is absent or fails to connect.
6. Associate that one client. Require its masked `assoc ... aid=1 open
   authorized` line. Configure the client statically as `192.168.2.2`, mask
   `255.255.255.0`; gateway `192.168.2.1` is optional for the local ping. DHCP,
   DNS and Internet are not expected at this stage.
7. Ping **192.168.2.1** repeatedly. Verify ARP resolves the AP BSSID/MAC and
   receive replies; if available, ping `192.168.2.2` from the phone as well.
   Capture en3 Ethernet/ARP/ICMP and over-air frames if association works but
   ping does not. Distinguish firmware TX errors, RX decapsulation and the BSD
   path before changing association logic.
8. Disconnect cleanly; require the masked disassoc line. Reassociate and repeat
   the ping. Try a second client while the first is present and verify refusal
   without disrupting the first. If a client disappears silently, use --off
   and --apply to clear its slot; timeout/kickout handling is not implemented.
9. Execute `--off`; require teardown and `STA restore error=0`, disappearance
   of the SSID, link-down en3 without its IPv4 address, then ordinary STA
   WPA2 rejoin/packet tests. en3 stays attached after first use. Repeat an
   enable/disable cycle and test ordered kernel shutdown with AP active.
10. Negative checks: a file with a third line must fail without starting an
    open AP, including an empty channel line followed by `6` on line three;
    channels 0/12/36 must fail input validation. Record separately
    whether the loaded firmware accepted AP mode and whether STA regression
    passed. Only after this gate should step 5 attach hostapd and the shared
    DHCP/DNS/NAT layer to en3 / 192.168.2.0/24.

### Build and local validation

Kernel and Wi-Fi builds run only under `/Users/xm666/X/umi-ios/build-lock.sh`.
The reviewed build and artifact copy were run inside the same lock. From `R`:

```sh
/Users/xm666/X/umi-ios/build-lock.sh env \
  PATH="$PWD/tools/macos-bin:/opt/homebrew/opt/llvm/bin:$PATH" bash -c '
set -e
bash devices/xiaomi-mi-10/build-wifi.sh > out/xiaomi-mi-10/hotspot-ap-resume-wifi-build.log 2>&1
bash devices/xiaomi-mi-10/build-kernel.sh -j8 BUILD_DSYM=0 > out/xiaomi-mi-10/hotspot-ap-resume-kernel-build.log 2>&1
cp out/xiaomi-mi-10/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837 out/xiaomi-mi-10/flash/kernel-hotspot-ap
shasum -a 256 out/xiaomi-mi-10/flash/kernel-hotspot-ap
'
```

`BUILD_DSYM=0` avoids the existing dSYM recipe's process substitution through
`/dev/fd`, which this execution sandbox denies. C compilation/linking and the
normal stripped kernel target still run. No kernel Makefile was edited.
The local ath11k object has zero generated external stubs; its beacon helper
and AP callback symbols were checked. The standalone iOS helper builds with
`-Wall -Wextra -Werror`. All 20 host parser cases and the Darwin IPv4 ioctl ABI
checks passed with ASan and UBSan:

```sh
clang -Wall -Wextra -Werror -fsanitize=address,undefined -g \
  devices/xiaomi-mi-10/tools/test-wifi-ap-helper.c -o /private/tmp/umi-wifi-ap-helper-test
/private/tmp/umi-wifi-ap-helper-test
```

Build logs: `R/out/xiaomi-mi-10/hotspot-ap-resume-{wifi,kernel}-build.log`.
Artifact: `R/out/xiaomi-mi-10/flash/kernel-hotspot-ap`, SHA-256
`9ecd64e6837c7f46d0689647793dc2f11fe0e7d5ed75d147110b0d93148680f6`.
The build exited 0; the final linked kernel contains the AP sysctl, AP
configure/send/stop callbacks and beacon-template helper. `kernel-sbx-apps`
is retained unchanged with SHA-256
`3c15fd9a1de25d24b42dc009512d97ebd642332fae15ecd01778792f3893914f`.
`kernel-sbx-apps2` was absent and was not created. No phone commands, full
userland payload build, or NAT/DHCP/DNS changes were performed.

## Implementation (USB/NAT)

2026-11-11, resumed/reviewed implementation. Steps 1–2 are implemented in
userland and host-tested; **on-phone acceptance is pending**. No phone commands,
kernel build, or full userland/payload build were run for this USB/NAT work.
The Wi-Fi AP section above, its kernel artifact, and its source files were not
changed by this implementation.

### Files and review of the interrupted work

Paths relative to `R = work/umi-v23`:

- `devices/xiaomi-mi-10/overlays/userland/src/hakios_hotspot.c`: reviewed and
  replaced the partial router with direct PF ioctls, shared downstream config,
  DHCP, upstream discovery/reconnect and reversible session management.
- `userland/com.hakios.hotspot.plist`: retained the dedicated launchd job;
  Background process, console logs, 10-second termination/respawn intervals.
- `devices/xiaomi-mi-10/overlays/userland/build.sh`: small compile/install
  stanzas only. Builds the helper with the matching XNU PF header; installs its
  job only when the binary is present. No mkjobs.py or rc.jobs edits.
- `devices/xiaomi-mi-10/tools/build-hotspot-helper.sh`: standalone arm64 build,
  not a payload build.
- `devices/xiaomi-mi-10/tools/test-hotspot.py`: actual C functions exercised on
  the host with ASan/UBSan and captured PF ioctls; no host network mutations.
- `devices/xiaomi-mi-10/HOTSPOT-umi.md`: this USB/NAT section only.

The saved code's opt-in job and USB addressing were appropriate. Its DHCP parser
started four bytes past the options (the cookie was already a separate field),
its hashed allocation could collide, and REQUEST did not validate requested IP
or server ID. Those were corrected. The unverified PacketFilter framework
function prototypes/rule dictionaries were removed, as were unchecked route
writes, unconditional route deletion and incomplete enable-failure rollback.
The old `kern.hakios.cellular_*` dependency was also removed: those sysctls belong
to the integrated-MPSS driver, not umi's current CommCenter/X55 path.

### PF evidence and runtime decision

The current generated XNU has `options PF` in `config/MASTER:66` and
`BUILD/obj/DEVELOPMENT_ARM64_BCM2837/bsd/DEVELOPMENT/pf.h` defines `PF 1`.
`bsd/net/dlil.c` calls `pfinit()`. `bsd/net/pf_ioctl.c:406` initializes PF;
`:501` registers root-only `/dev/pf` and `/dev/pfm`, then attaches the IP hooks.
The IPv4 hook invokes `pf_test_mbuf()` on both directions, including the raw-IP
cellular interface. The extracted iOS rootfs has **no pfctl**; its bootpd binary
exists, but this service uses a built-in DHCP implementation.

Reuse this compiled PF, with no framework/manager dependency or new NAT engine.
The helper includes this build's `out/xiaomi-mi-10/xnu/bsd/net/pfvar.h` (the
current arm64 ABI is `sizeof(pf_rule)=1040`, `sizeof(pfioc_rule)=3104`). It uses
DIOCGETSTATUS, reference-counted DIOCSTARTREF/DIOCSTOPREF and Apple's
DIOCCHANGERULE (ticket + ADD_HEAD)/DIOCDELETERULE. Rules have owner `com.hakios.hotspot` and are
inserted at the head of the active root rulesets; they need no pre-existing
root anchor call. Their narrow matches take precedence, ensuring translated
flows use our filter/state owner even if a pre-existing broad PASS rule exists. No root ruleset replacement or global state flush occurs. XNU's owner
rule deletion expires states belonging to each owned filter rule, including
its NAT flows (`pf_expire_states_and_src_nodes`). PF references held by other
clients are preserved.

**This establishes compiled availability, not runtime success.** First run
`/redwood/bin/hakios-hotspot --probe` as root via the supervisor's existing
command/job mechanism. It opens `/dev/pf` and queries status without enabling
PF. A successful probe plus rule insertion/readback and actual translated return
traffic are all required before declaring PF usable. If open/status/startref/
insert fails, retain the exact errno and stop this acceptance test; there is no
silent alternative NAT path and no change to CommCenter.

Per downstream, five IPv4 rules are installed:

1. PASS IN on that interface, source its /24, no state, tag
   `com.hakios.hotspot.<interface>`.
2. NAT OUT on pdp_ip0, source that /24 **and matching the ingress tag**, to the
   current cellular IPv4 /32, translated ports 1024–65535.
3. PASS OUT on pdp_ip0 with that tag, normal state, owned by this service.
4. SCRUB TCP IN on the downstream, source its /24, max-mss `min(MTU,1500)-40`.
5. SCRUB TCP OUT on the downstream, destination its /24, the same max-mss.

Thus spoofing the source subnet on an unrelated ingress does not select this
NAT rule. Rules cover `172.16.42.0/24` for USB; NAT does not include the phone's
own traffic. Unrelated system rules remain installed. This does not
provide client isolation or a general firewall policy for other interfaces.

### Enable, configuration and disable

The default downstream is `usb-on en2 172.16.42.1/24`. No switch files means
no network writes, DHCP listener, directory creation or routine hotspot logs.
The idle launchd process only polls the configuration/switches once per second.
No new boot arguments, USB identity changes or USB/NCM toggles are introduced;
the existing working NCM and cellular setup must already be enabled.

Supervisor commands on the phone, using the existing test/job mechanism:

```text
/redwood/bin/hakios-hotspot --check
/redwood/bin/hakios-hotspot --probe
/redwood/bin/hakios-hotspot --status
/redwood/bin/hakios-hotspot --usb-on
/redwood/bin/hakios-hotspot --usb-off
```

`--usb-on` creates root mode-0600 `/var/db/hakios-hotspot/usb-on`; `--usb-off`
removes it. They return after changing the file; the daemon performs the network
transition on its next poll. Preserve switches across boots only intentionally.
`--probe` and `--status` are read-only diagnostics and need no switch.

Native upstream discovery reads the actual UP IPv4 address and MTU of pdp_ip0,
then the configd `State:/Network/Service/<id>/IPv4` entry whose InterfaceName and
Addresses match it, and that service's DNS ServerAddresses. This avoids using
Wi-Fi's global DNS. If the standalone kernel QMI mode publishes no configd DNS,
provide **the carrier DNS from the current WDS runtime-settings log**, e.g.:

```text
/redwood/bin/hakios-hotspot --dns <carrier-IPv4-DNS>
```

This writes `/var/db/hakios-hotspot/upstream-dns` (one IPv4 address, mode 0600).
It is an explicit override, not a public-resolver fallback. `--dns-auto` removes
it and returns to configd discovery. On carrier changes update/remove a manual
override; automatic DNS changes are observed from configd. A missing address,
valid MTU or IPv4 DNS leaves the service waiting with forwarding/rules untouched.
`--status` shows which of address/MTU/DNS is absent. No DNS proxy is needed:
DHCP option 6 advertises this carrier resolver directly, and both UDP and TCP DNS
travel through NAT. The gateway `.1` is **not** a DNS listener.

If no IPv4 default route exists, the service adds an acknowledged, direct
interface default on the raw-IP pdp_ip0 link; no ARP/gateway Ethernet bridge is
involved. An existing pdp_ip0 default is retained. An existing default through
another interface causes activation to fail and roll back: disconnect the
phone's Wi-Fi STA for this cellular-only test. It never replaces that route or
changes route priorities. Only a default created by this session and still
matching its interface/gateway is removed at teardown.

Enable takes a snapshot of IP forwarding, configures missing downstream IPv4
aliases and UP flags, opens DHCP port 67, installs the default/rules and enables
forwarding. Existing correct USB/AP addresses are retained. Errors undo partial
work. Removal of the switch or SIGTERM closes DHCP, removes owned rules/states,
removes only owned addresses/route, restores flags/forwarding and releases only
this PF reference. An atomic root-only `session` journal and process lock allow
same-boot launchd restart recovery. A journal from a previous boot is discarded;
old kernel PF tokens/routes are not applied to the new boot. A cleanup failure
is explicitly logged and retains the journal for retry. Do not delete a live
session journal by hand.

The daemon polls the upstream each second. Address, MTU, DNS changes or loss of
UP/IPv4 tear down the old session and its NAT state, then enable again when ready.
Existing TCP connections are expected to break across a PDN address change;
new connections must work. DHCP leases are 300 seconds, T1 150 seconds, so DNS
changes reach clients on renewal; explicitly renew immediately when testing.

DHCP uses one UDP listener with IP_PKTINFO receive/reply interface selection,
and rejects packets from inactive interfaces. It serves Ethernet clients
without relay/BOOTP option-overload support, reserves unique `.2`–`.65` leases
per /24, handles DISCOVER/REQUEST/RELEASE/DECLINE, ignores another selected server,
and NAKs conflicting/out-of-pool requests. ACK logs mask four MAC octets.
Gateway/server ID are `.1`, netmask /24, DNS is the carrier IPv4, and option 26 is
`min(cellular MTU,1500)`. A port-67 conflict fails activation; it does not stop
bootpd or another DHCP server. No interface MTU is changed; PF clamps both TCP
SYN directions even when a client ignores option 26. UDP/ICMP still rely on PMTU
and normal fragmentation/ICMP behavior.

### Reuse by Wi-Fi AP

The Wi-Fi agent's actual contract is **en3**, `192.168.2.1/24`, not ap1.
Keep the implicit USB row and add one line to the root-owned file
`/var/db/hakios-hotspot/downstreams`:

```text
wifi-on en3 192.168.2.1/24
```

The format is `<switch-basename> <interface> <gateway>/24`, one per line; `#`
comments and blank lines are accepted. IPv4 RFC1918 /24 with gateway `.1` only,
up to four downstreams including USB; duplicate interfaces/switches/subnets
and pdp_ip0 as a downstream are rejected. Config is reloaded while all sharing
switches are off; use `--check` before enabling. Start the AP with its own helper
first, then `/redwood/bin/hakios-hotspot --on wifi-on`. Both USB and Wi-Fi switches
can be present. Stop sharing with `--off wifi-on` **before** stopping the AP.
The AP radio helper/switch remains separate; this daemon never starts or stops
Wi-Fi or makes an open AP secure. Its one-client/open-network limits remain as
documented above. The sharing layer has no hardcoded en3/AP radio dependency.

### Exact log signatures

These are expected format strings/examples, **not captured device output**.
USB with cellular MTU 1410:

```text
hotspot: PF ready running=0 states=0 rule-size=1040
hotspot: NAT rule loaded downstream=en2 subnet=172.16.42.0/24 upstream=pdp_ip0 mss=1370
hotspot: upstream pdp_ip0 address=<IPv4> mtu=1410 dns=<IPv4>
hotspot: enable downstream-mask=1 forwarding=1
hotspot: DHCP lease interface=en2 address=172.16.42.2 mac=02:42:xx:xx:xx:xx seconds=300
hotspot: disable rules=removed states=removed forwarding=0
```

`running`, pre-existing state count, address, DNS, MTU/MSS, lease and previous
forwarding vary. `--status` emits one `hotspot: rule interface=%s action=%u
packets-in=%llu packets-out=%llu states=%u` line per owned rule, then:

```text
hotspot: status owned-rules=5 owned-states=<count>
hotspot: upstream probe interface=pdp_ip0 address=<IPv4> mtu=1410 dns=<IPv4> ready=1
hotspot: forwarding=1
```

Rule actions: PASS=0, SCRUB=2, NAT=4. Owned state count sums filter rules (not
NAT references to the same states). Expect five rules for USB, ten for USB+AP;
after all switches are off expect `owned-rules=0 owned-states=0` and the original
forwarding value. PF may still run if another client holds a reference.

Failure/wait signatures:

```text
hotspot: PF unavailable open=/dev/pf errno=%d
hotspot: PF failed op=%s errno=%d
hotspot: upstream unavailable interface=pdp_ip0
hotspot: enable failed snapshot errno=%d
hotspot: enable failed downstream=%s errno=%d
hotspot: enable failed DHCP-bind errno=%d
hotspot: enable failed default-route interface-index=%u expected=pdp_ip0; disconnect STA first
hotspot: enable failed route-get errno=%d
hotspot: enable failed route-add errno=%d
hotspot: enable failed route-verify errno=%d index=%u
hotspot: enable failed forwarding errno=%d
hotspot: DHCP send failed interface=%s errno=%d
hotspot: cleanup failed route errno=%d
hotspot: cleanup failed route-get errno=%d
hotspot: cleanup failed forwarding errno=%d
hotspot: cleanup failed PF-open errno=%d
hotspot: cleanup failed PF-stopref errno=%d
hotspot: cleanup incomplete; session retained
hotspot: invalid downstream config
hotspot: session lock failed errno=%d
hotspot: invalid session journal; refusing network changes
```

PF operations include `status`, `startref`, `begin-pool`, `add-pool`,
`rule-ticket`, `insert-rule`, `delete-owner`, `get-rules`, `get-rule`. Open ENOENT=2 means the
runtime device is absent; ioctl ENOTTY=25 suggests ABI/config mismatch; EPERM=1
or EACCES=13 means authorization/securelevel; DHCP EADDRINUSE=48 means a port
conflict. Preserve the operation and errno rather than diagnosing from one code
alone. An unavailable upstream is a wait condition; a NAT-loaded log alone is
not proof of traffic translation.

### Supervisor build and on-phone checklist

Host checks already performed:

```sh
cd work/umi-v23
python3 devices/xiaomi-mi-10/tools/test-hotspot.py
bash devices/xiaomi-mi-10/tools/build-hotspot-helper.sh
bash -n devices/xiaomi-mi-10/overlays/userland/build.sh
```

The real C DHCP/config/PF builders passed ASan/UBSan: wire layout and first
option, truncation/duplicates/PAD/relay rejection, ACK/NAK/router/DNS/MTU options,
64 unique leases/exhaustion/conflicts/expiry/renewal, two subnets, ingress tags,
NAT pool/ports, state ownership, head insertion, bidirectional MSS, partial
activation teardown/PF-token release/journal retention on failure, and
100,000 malformed option streams. IP_PKTINFO decoding/truncation and launchd
plist were checked. The **exact** userland compiler stanza also compiled in an
isolated snapshot-style symlink layout, so its private PF header path was checked.
Standalone iOS helper links only libSystem, CoreFoundation and SystemConfiguration,
with no unresolved dynamically-looked-up symbols. Tests do not emulate XNU PF
packet traversal or prove iOS UDP/configd/route behavior.

Supervisor builds **one merged userland payload** with the usual current CA/USB/
cellular settings. The overlay build now compiles and installs
`/redwood/bin/hakios-hotspot` and `com.hakios.hotspot`. No USB/NAT kernel rebuild
is required: use the supervisor's selected merged kernel with PF already enabled.
Do not mistake the AP section's kernel hash/build for a USB/NAT rebuild. If using
PREBUILT, include the standalone helper as well as its job; a job is deliberately
not installed when no binary is available.

1. **Default regression:** boot with no hotspot switches (and no live recovery
   journal), existing working NCM/cellular options only. Verify USB local ping,
   current USB services, cellular and ordinary STA behavior. There should be no
   automatic `hotspot:` logs, port-67 listener, forwarding change or hotspot rules.
   Record `--probe` and `--status` explicitly as the PF/forwarding baseline.
2. **Upstream gate:** disconnect the phone's Wi-Fi STA; leave native CommCenter
   running. Require pdp_ip0 IPv4 and `--status ... ready=1`. If DNS is zero in
   standalone QMI mode, use `--dns` with the current WDS carrier DNS. If configd
   state is missing in native mode, retain it for diagnosis; an explicit override
   may independently test NAT without modifying CommCenter.
3. **Step 1 static Mac:** supervisor executes `--usb-on`, requires successful
   enable/rule logs. On the Mac identify the existing NCM enN from
   `networksetup -listallhardwareports`/`ifconfig -a`. Set `$USB_IF` to it:

   ```sh
   sudo ifconfig "$USB_IF" inet 172.16.42.2 netmask 255.255.255.0 up
   ping -c 3 172.16.42.1
   sudo route -n add -host 1.1.1.1 172.16.42.1
   route -n get 1.1.1.1
   ping -c 3 1.1.1.1
   ```

   Require route gateway `.1` on that USB interface. A targeted host route avoids
   silently testing the Mac's Wi-Fi path. Record ping and `--status`; PASS/NAT
   counters and owned states must increase. If available, capture en2/pdp_ip0:
   outgoing pdp source must be the current cellular IPv4, and reverse packets
   must arrive at `.2`. Keep all diagnostics local. Remove this Mac test route
   with `sudo route -n delete -host 1.1.1.1 172.16.42.1` afterwards.
4. **Step 2 DHCP:** return the Mac USB service to DHCP (`sudo ipconfig set
   "$USB_IF" DHCP`, or its network service's DHCP setting). Inspect
   `ipconfig getpacket "$USB_IF"`; require `.2`–`.65`, /24 mask, router `.1`,
   carrier DNS, 300-second lease and MTU option. Require the masked ACK log.
   Check `route -n get 1.1.1.1` still selects USB; temporarily disable Mac Wi-Fi
   or change service priority for a conclusive full Internet test. Clear any
   manually configured DNS on that Mac service. Verify numeric IPv4 access,
   `dig @<carrier-DNS> example.com`, TCP DNS (`dig +tcp`) and an HTTPS transfer.
5. **MTU:** compare `--status`/upstream MTU and rule MSS. Capture TCP SYN and
   SYN/ACK on USB/cellular; both must advertise MSS no greater than MTU minus
   40. Repeat a large HTTPS download with no stalls. Test DF ICMP near
   `MTU-28` and above it, recording ICMP fragmentation-needed/PMTU behavior;
   DHCP MTU compliance alone is not proof that clamp/PMTU works.
6. **Renew/reconnect:** renew DHCP, release/reacquire it, unplug/replug NCM and
   renew again. en2 normally stays attached; without its switch being removed
   the session/leases may remain during a USB disconnect. Require working new
   traffic after replug. Cause a supervisor-controlled cellular disconnect/
   reconnect and, if possible, an IPv4/MTU/DNS change. Require unavailable/
   disable/new-upstream/NAT-loaded/enable logs, purged old state, then successful
   new DNS/HTTPS traffic. Renew DHCP immediately if DNS changed. Do not expect
   an established TCP connection to survive a new public/PDN address.
7. **Disable/rollback:** execute `--usb-off`; require disable and zero owned
   rules/states, original forwarding, no DHCP listener and no session file.
   Preserve the existing `.1`/USB services; only helper-created addresses/routes
   should disappear. Repeat enable/disable. Test SIGTERM and a daemon restart
   with the switch present; verify same-boot recovery and no duplicate rules or
   PF references. Test a missing downstream, busy DHCP port and non-cellular
   default route: activation must fail and restore the baseline each time.
8. **AP reuse, after radio acceptance:** with sharing off, add the en3 config
   line, `--check`, start the AP via its own helper, then `--on wifi-on`.
   Require en3 rules and `192.168.2.2` DHCP; test gateway/DNS/HTTPS through
   cellular. USB+AP must use separate pools and ten rules. Stop AP sharing before
   the radio helper. Respect the AP milestone's open-network/one-client limits.

Known boundaries: IPv4 and RFC1918 /24 only; 64 MAC leases per downstream,
no relay/option-overload support or persistent lease database; no IPv6 sharing,
DNS proxy/cache, native Settings/misd integration or STA policy-routing override.
A manual DNS override needs operator maintenance. Runtime PF, DHCP broadcast,
configd DNS publication, direct cellular default-route acceptance, PMTU and
reconnect all remain supervisor hardware gates. No cellular identifiers are
read or logged, and no CommCenter process/binary/job was changed.

Final standalone artifact: `R/out/xiaomi-mi-10/hotspot/hakios-hotspot` (Mach-O
arm64), SHA-256
`7b6b8d06cd1f03dd9f487967fcab21a99db00944463d879fd06bc787fcad8556`.
USB/NAT kernel artifact/path/SHA-256: **not applicable; no kernel rebuilt**.

## Implementation (native Settings)

2026-11-11 follow-up, **paused at the open AP data-path repair**, per the
supervisor's v28 evidence. This section supersedes the initial research's
assumption that CommCenter/misd could not launch. The Settings row is visible,
misd is launched by launchd, and CommCenter reports tethering `authStatus=true`.
That establishes provisioning progress, not a working native AP switch. No
CommCenter, carrier bundle, loader, boot arguments, GNSS or statusbar code was
changed in this follow-up. No phone access was performed.

### Native control flow and ABI established locally

The chain is Settings/Netrb → `com.apple.MobileInternetSharing` (misd) →
MobileWiFi/wifid → IO80211/Apple80211 BSD ioctl → the Wi-Fi shim. On successful
HOST_AP start, IO80211 prepares the native hostapd configuration and starts
`com.apple.wifi.hostapd`. These are local binary call sites; a successful native
switch invocation has **not** been captured on this kernel.

Additional iOS 16.5 / 20F66 disassembly findings:

- wifid `0x100042fa8` sets request **25**, value **1**, with a CFDictionary
  (`len=8` at this API boundary). IO80211 `_startAPMode` at `0x1d30deaf0`
  marshals that into a **1500-byte, version-1 raw structure**, then calls
  Apple80211RawSet at `0x1d30df13c`. Disabling at wifid `0x100043000` uses
  request 25, **value 2, length 0**. Do not treat this as a Boolean sysctl.
- Confirmed raw structure offsets: version `0x00`, uint16 mode `0x04`
  (HOSTAP=2), auth lower/upper `0x06/0x08`; channel version/number/flags
  `0x0c/0x10/0x14`; SSID length/bytes `0x18/0x1c` (32 bytes); key
  length/cipher/flags-or-index/bytes `0x40/0x44/0x48/0x4c` (64 bytes).
  Tail fields and all key/security semantics still need validation. Never
  print the raw request, password, PSK or key material.
- wifid `0x1000489d4` requests virtual **ap1**, role **7**. Request **94**
  uses a 32-byte version-1 structure, role at `0x0c`, returned interface name
  at `0x10` (16 bytes); IO80211 handler `0x1d30d70fc`, returned-name read
  `0x1d30d717c`. Delete request **95** uses 20 bytes, name at offset 4.
- IO80211 hostapd configuration code at `0x1d30df428` hardcodes **ap1** and
  uses `/var/run/hostapd.conf`. Returning `en3` from CREATE alone will not
  satisfy it. The current en3 is an Ethernet downstream, without native
  HostAP IOKit publication or an ap1 interface.
- The current `drivers/wifi/hakios.wlan.cpp` publishes Infrastructure only;
  its unsupported user client makes native users fall back to BSD ioctls.
  Correct HostAP publication/events, request 25/94/95 lifecycle and aliasing
  remain necessary in `hakios.apple80211.c` and friends. None is implemented
  by this follow-up; protected requests must never silently enable an open AP.
- Native hostapd still needs the request 3/73–78 driver ABI, association and
  EAPOL delivery, controlled-port authorization and ath11k key installation.
  Existing `lx_wpa.c` is the STA supplicant, not an AP authenticator. WPA2,
  Settings password handling and the iPhone-name SSID are pending.

misd owns IPv4 forwarding/PFUser rules, writes bootpd configuration and starts
`/usr/libexec/bootpd`; its DNS path uses `mrc_dns_proxy_*` with input/output
interfaces (see research section 3). Further PacketFilter disassembly shows
`PFUserCommitRules` (`0x1f1a75b5c`) eventually goes through
**PFManagerSendMessage/XPC**. `/dev/pf` and a compatible `pfioc_rule` alone do
not prove that this native service path works. Identify/validate the PF manager
server and native bootpd/DNS activation before replacing these functions.
Stock bootpd contains BPF transmitter paths (`Transmitter: bpf_setif`,
`bpf_write`, `/dev/bpf%d`), supporting the DHCP repair below.

For this milestone, **hakios-hotspot still supplies all NAT/DHCP and advertises
the cellular DNS directly; it supplies no DNS proxy**. This is the existing
manual-switch service, not an automatic competing service started by Settings.
Native misd NAT/DHCP/DNS is preferred for the later integration, but unverified.
Do not run both DHCP servers on the same downstream.

### v28 root cause and implemented data-path repair

The completed `logs/test-1111-172537-umi-v28.kernel.log`, lines
184971/185070/185364/185764/186182/186823, contains six
`hotspot: DHCP send failed interface=en3 errno=51` messages after association.
Thus valid DHCP traffic reached the helper through the existing radio/en3
input path. The earlier report of no DHCP activity was based on an incomplete
snapshot. PF's subnet-source rules do not match initial DHCP source 0.0.0.0;
zero PF counters/states do not establish a broken wireless receive path.

The helper's previous scoped UDP broadcast sender hits Darwin's routing rules:
`bsd/netinet/udp_usrreq.c` makes nonzero `IP_PKTINFO.ipi_ifindex` override
`ipi_spec_dst`, clearing the selected source; `bsd/netinet/in_pcb.c` rewrites
255.255.255.255 using the primary interface's broadcast **before** scoped route
selection. The resulting send can fail with ENETUNREACH (51) on en3. This is
source-supported diagnosis; no trace of the phone's internal route decision
was added.

Changes in this follow-up (paths relative to `D`):

- `overlays/userland/src/hakios_hotspot.c`: keep one UDP/IP_PKTINFO ingress
  socket; replace DHCP transmission with one write-only BPF descriptor per
  active downstream, bound with BIOCSETIF and complete Ethernet headers.
  Require DLT_EN10MB. Build wire-order IPv4/UDP with checksums, source
  gateway/interface MAC, ports 67→68. Initial OFFER/ACK and NAK use Ethernet
  and IP broadcast; renewal uses ciaddr and the requesting client's MAC,
  without waiting for ARP. Close descriptors on rollback/disable. The same
  fix applies to en2; no alternate NAT engine or routing fallback was added.
- `overlays/wifi/lx/src/lx_80211.c`: validate raw AP data as three-address
  ToDS=1/FromDS=0 with RA=BSSID; count null/invalid/4-address frames instead
  of delivering them as ordinary payload. Decapsulation maps DA=addr3,
  SA=addr2. The common decapsulator now checks length before reading frame
  control; its STA mappings remain intact. AP group Ethernet TX supplies
  no unicast sta to ath11k, using the AP vdev/self-peer metadata; unicast
  keeps the associated station. Firmware performs FromDS encapsulation.
- `overlays/wifi/lx/src/umi_wifi_ap.h`: count receive authorization/source/
  format drops, en3 submissions, TX group/submit/drop/completion; print totals
  at start, every five seconds while active and before peer removal. Stop
  the diagnostic work on AP shutdown. Log actual driver peer authorization,
  DP setup, active TID mask and four-address setting after association.
- `overlays/xnu/files/bsd/dev/hakios.net.c`: value-only en3 counters for
  mbuf allocation/copy/framing, ifnet_input acceptance/rejection and output
  queued/dropped/down. Report ENOBUFS to an en3 output caller when the link
  queue rejects its packet, instead of silently returning success.
- `overlays/xnu/files/bsd/dev/hakios.lx.c`: distinguish AP queue enqueue,
  full/copy failure, stale-session drop, submission and send rejection.
- `tools/test-hotspot.py`: exercise actual BPF frame construction, independently
  verify IP/UDP checksums (including odd length), broadcast/renewal/NAK,
  capacity rejection, BPF setup failures and descriptor cleanup. Existing
  DHCP lease/PF/rollback/malformed-input tests remain enabled.
- `tools/test-wifi-ap-datapath.py`: compile the real decapsulator, AP ingress
  gate and TX peer selector in a host harness; cover DS/QoS/HT/IV mappings,
  short-frame bounds, authorized/source/EAPOL gating, AP group and STA TX.
- `overlays/xnu/files/bsd/dev/hakios.cpufreq.c`: one build-only prerequisite,
  include `sys/vnode_internal.h` for its pre-existing rootvnode use. No
  governor behavior change.

Driver review: `ath11k_mac_station_add` calls `ath11k_dp_peer_setup`, which sets
WMI default RX routing and initializes TIDs 0–16, including non-QoS TID16.
The existing AUTH→ASSOC→AUTHORIZED transitions set the open peer association
`auth_flag` and `peer->is_authorized`; the driver intentionally sends a separate
WMI_PEER_AUTHORIZE only for STA vifs. No duplicate TID setup or speculative AP
WMI authorization was added. A healthy new peer should log `found=1`,
`authorized=1`, `dp-setup=1`, `tid-mask=0x1ffff`, `fouraddr=0`.

Counter interpretation:

- `hotspot-ap: rx`/`rx-drop`/`tx` are per AP session. `rx en3` means submitted
  to the BSD adapter; `bsd stage=input` means ifnet_input accepted ownership,
  not that UDP already consumed it. `hotspot: DHCP rx` is proof of delivery
  through to the service. Driver Ethernet RX totals may include rejected
  sources; authorized deliveries are counted separately.
- `hotspot-ap: bsd` and `queue` are cumulative since boot, logging the first
  eight and powers of two of each stage. All new diagnostics contain only
  counter values, lengths, stages and error codes; peer MAC logs stay masked.
- `hotspot-ap: driver-total` samples existing ath11k error-ring, invalid-RBM,
  REO/RXDMA/HAL and TX descriptor/allocation error counters. They are cumulative
  since driver start, including earlier STA use; compare the AP-start baseline
  with later samples to find drops before the shim's RX callback.
- `tx submit`/queue `submitted` do not establish an over-air transmission.
  `complete`/`ack`/`noack` are driver status callbacks; group traffic normally
  has no ACK. `released` is an unclassified free callback, **not** proof of
  failure (it can include normal group completions).

### Build and supervisor acceptance gate

Host tests pass under ASan/UBSan; standalone arm64 helper and Wi-Fi/kernel
builds pass. Kernel builds used the required build lock and BUILD_DSYM=0;
no full payload build was run. Build logs are
`R/out/xiaomi-mi-10/hotspot-native-{wifi,kernel}-build.log`.

Kernel: `R/out/xiaomi-mi-10/flash/kernel-hotspot-native`, SHA-256:
`608883e85eaf45d354c511a120f706140fa8f88b19f0ff2933884cbe688d72da`.
Helper: `R/out/xiaomi-mi-10/hotspot/hakios-hotspot`, SHA-256:
`927b88dbe32fdef204406d4bb0f435b33f7f5a47f2a31dfc521530e143edf7d6`.
**Both artifacts are needed**: a new kernel with the old helper retains the
known DHCP routing failure. The supervisor must merge the new helper as
`/redwood/bin/hakios-hotspot` in their normal payload workflow.

On-phone checklist (supervisor only):

1. With hotspot off, confirm STA joins and has internet. Enable the existing
   open AP with `umi_wifi_ap --apply`, then `hakios-hotspot --on wifi-on`, using
   `wifi-on en3 192.168.2.1/24`. Confirm en3 link/address and cellular upstream.
   Do not use Settings as the activation gate for this milestone.
2. Join one client. Require the healthy peer line above; increasing `rx en3`,
   BSD input and DHCP rx type=1; DHCP tx type=2 `transport=bpf`; then request
   type=3, ACK type=5 and a lease in 192.168.2.2–65 with gateway 192.168.2.1.
   `DHCP send failed errno=51` must disappear. BPF open/DLT/ioctl errors fail
   activation visibly and roll back rather than enabling another sender.
3. If no address arrives, correlate DHCP tx → BSD tx-queued → AP queue →
   tx submit/completion. Capture client-side DHCP OFFER/ACK and ARP. BPF write
   success alone is insufficient. If driver totals rise without shim RX,
   investigate REO/RXDMA/TID errors; if source/inactive/DS drops rise, retain
   those totals and association timing. A group noack alone is not a failure.
4. With a lease, test ARP/gateway ping, DNS to the advertised resolver, HTTPS
   and a larger download over cellular; require PF counters/states to rise.
   A DNS-only failure needs separate resolution of upstream DNS reachability;
   this helper does not run mrc_dns_proxy.
5. Renew, disconnect/reassociate and repeat DHCP; disable sharing before
   `umi_wifi_ap --off`. Require teardown and `STA restore error=0`, no new AP
   periodic lines, no owned PF rules/states, and working STA internet again.
   Retest en2 DHCP/NAT because its replies now use the same BPF transmitter.
6. Later native milestone: capture Settings enable/disable, SSID/name/password
   changes (never log credentials), request 94/25/95 order, ap1 publication,
   hostapd events/EAPOL and key installs; separately verify misd PF manager,
   bootpd and DNS service. This build does not claim those paths work.

Still untested on hardware: repaired DHCP TX, AP unicast/group data completion,
ARP, cellular forwarding, DNS, reassociation and off→STA restoration with this
build. Open, one-client, legacy 2.4 GHz AP limitations remain, including no
host power-save buffering. Native Settings and WPA2 remain the next milestone,
after the above data-path acceptance test.

## Implementation (native Settings, WPA2)

2026-11-11, continuation for **v29**. Native Settings control, BSD/IOKit `ap1`,
and an AP-side WPA2-PSK/CCMP authenticator are implemented and built. Native misd
owns IP sharing. Host tests and local binary/source ABI checks passed;
**end-to-end phone acceptance is untested**. The historical open-AP/en3/manual
instructions above describe older artifacts and are superseded for this path.
No phone access, CommCenter/carrier change, new boot argument or full payload
build was performed.

### Call flow, discovery and raw ABI

Settings → WirelessModemSettings/Netrb → `com.apple.MobileInternetSharing`
(misd) → MobileWiFi/wifid → IO80211 → Apple80211 BSD ioctl →
`hakios_wifi_ap_configure` → ath11k AP vdev. Settings supplies the device-name
SSID and configured password. The kernel uses those bytes, including UTF-8
SSID bytes up to 32 bytes; it does not read a password/switch file. Invalid or
missing protected credentials fail, never enabling an open AP.

`umi_apple80211_ap.inc` defines these 20F66 raw layouts, with structure version 1:

| Request | Implemented contract |
| --- | --- |
| Set 94, CREATE | 32 bytes; role 7 at +12; return `ap1` at +16, 16-byte field. Publishes interface without starting radio. |
| Set 95, DELETE | 20 bytes; name +4. Stop AP, restore STA, remove IOKit service and asynchronously detach BSD ap1. |
| Set 25, start | Value 1, 1500 bytes; mode low byte 2 at +4; lower/upper auth +6/+8: 1/8 (mixed upper 10 accepted, WPA2 only advertised). Channel version/number/flags +0xc/+0x10/+0x14; SSID length/bytes +0x18/+0x1c; key version/length/cipher/flags/index/bytes +0x3c/+0x40/+0x44/+0x48/+0x4a/+0x4c. |
| Key in start | Cipher 10: 8–63 printable ASCII passphrase bytes; cipher 6: 32-byte PMK. Control copies wiped after configure, never returned/logged. |
| Set 25, stop | Value 2, length 0. Idempotent; restores original STA power request. |
| Get 26 / 15 / 13 | AP active / HOSTAP mode / link-state scalar. Raw Get 25 also reports active; IO80211 does not expose its high-level Get. |
| Get 1 / 9 / 4 / 2 | Actual AP SSID / BSSID / channel / auth type, not STA values on ap1. |
| Get 42 | 2056-byte station list: version/count, 128 entries of 16 bytes; entry version, MAC +4, RSSI +12. Returns the one authorized client, currently RSSI=0. |
| Set/Get 336 | Hidden-SSID scalar, real beacon update/directed-probe handling. |
| Get 254 | Legal AP 2.4 GHz channel list. Channel 0 selects 6; explicit channels 1–11 still require regulatory permission to initiate radiation. |
| Get/Set 347; Get 350 | 28-byte power params: disabled LPM accepted, unimplemented AP sleep scheduler enable returns EOPNOTSUPP. 88-byte LPM stats have zero counters. |

Physical capabilities include HostAP bit 7. Internal generic link slot 3 now
creates **BSD ap1**, not en3, with Ethernet framing, wireless media, BPF and
attach/link/detach events. Its IONetworkInterface child under the existing
IO80211Controller publishes IOInterfaceName/BSD Name `ap1`, prefix `ap`, unit 1
and **`IO80211VirtualInterfaceRole="SoftAP"`**. IO80211 virtual-role discovery
at `0x1d30dbc4c..0x1d30dbcd8` compares this property to SoftAP CFString
`0x1dab575c8`; IO80211InterfaceRole alone is insufficient. en0 retains its
Infrastructure service. These events/registry entries expose ap1 to configd
and misd; actual configd/NWI publication and any native bridge100 remain v29
runtime checks. IOUserClient remains unsupported, using the established ioctl
fallback. AP interface identity uses its actual unique-ID length.

Correct events are **12 STA_ARRIVE, 13 STA_LEAVE, 20 RSN_HANDSHAKE_DONE**,
plus 2/4/8 SSID/link/mode changes. Station event payloads are six raw MAC bytes
at offset zero, not a versioned structure (stock hostapd handlers
`0x1000246dc/708/7d0/7ec`). AUTH_IND 152 belongs to the unused userspace auth
path; kernel auth handles it internally. Status contains no credentials.
Completing startup preserves peer callbacks received before configure returns.

Optional Apple vendor IE requests 352/403 and CSA 349/420 remain unsupported
and unadvertised. Inspected wifid vendor-IE helpers log errors without returning
a fatal activation result; they are not the WPA2 start transaction. There is
no fake-success response. No 5 GHz/DFS/6 GHz, WPA3/SAE or PMF is advertised.
If wifid nevertheless forces an unsupported band, preserve its request trace;
the driver refuses that explicit request rather than silently changing it.

### WPA2 and native hostapd activation

The shipped hostapd driver needs RawGet 73 (station IEs), RawSet 74–78
(authorization/deauth/RSN/RSC), RawSet 3 (148-byte cipher key) and its own EAPOL
transport. Those complete stock payloads/transport are not implemented. This
continuation uses the task's alternative, a minimal in-kernel AP authenticator
sharing tested STA SHA1/PRF/AES arithmetic. No second authenticator runs.

- `lx_wpa.c` includes `umi_wpa_auth.inc`: PBKDF2-HMAC-SHA1/4096 PMK from
  password+SSID or supplied PMK; fresh ANonce, replay counter and GTK for every
  new association; PTK derives from ordered MACs/nonces.
- M2 checks lengths/type/replay/association RSN/MIC. M3 contains RSN + GTK KDE
  index 1, padded and RFC3394 AES-wrapped by KEK, key-info 0x13ca and
  HMAC-SHA1-128 MIC. M1/M3 have five attempts at one-second intervals.
- M4 replay/MIC validation precedes ath11k PTK install and ASSOC→AUTHORIZED.
  GTK uses AP self-peer index 1, PTK client peer index 0. Nonzero key result,
  bad MIC/format or timeout fails closed/removes the peer. Duplicate M4 never
  reinstalls a key. WMI key/peer work runs off the RX thread.
- Before authorization only EAPOL passes; ordinary secure RX then requires
  DECRYPTED and the associated source. Group TX selects GTK/self-peer, unicast
  PTK/client. Shutdown removes keys and wipes PMK/GTK/handshake state. Immutable
  AES tables replace racy lazy initialization; STA crypto regression passes.

After raw start, IO80211 unconditionally writes `/var/run/hostapd.conf` and
connects to **com.apple.wifi.hostapd**. Disassembly establishes an **empty XPC
dictionary** activation and ignored reply contents; stock hostapd reloads config
and replies empty. New demand-only `hakios-hotspot-native` owns that Mach service,
checks root peer plus actual active WPA2 kernel status, acknowledges and removes
the unused transient file. wifid Umask=077 makes that temporary credential file
private. The helper never reads/logs credentials or EAPOL. Inactive/unprivileged
requests are rejected. Stock hostapd must be replaced here because it would
compete with kernel auth and issue unsupported driver requests.

Radio limits remain **one client**, AP-only ownership, 2.4 GHz/20 MHz, legacy
non-QoS rates; no HT/VHT/HE, host power-save/PS-Poll buffering, inactive-client
eviction or periodic GTK rekey. GTK rotates on a new association; periodic
rekey was optional. A silently vanished client may occupy the slot until restart.
Sleep/reassociation are explicit acceptance gates, not claimed proven behavior.

### Native NAT/DHCP/DNS

v28 shows misd/pfd launched and CommCenter tethering authentication allowed.
No carrier or CommCenter change is needed/made here. misd owns ap1/bridge
addressing, configd/NWI, forwarding, PFUser rules, bootpd.plist and mrc_dns_proxy
input/output selection. Cellular upstream remains pdp_ip0.

PFUserCommitRules reaches **pfd**, the PFManager XPC server. Stock pfd constants
match XNU: pf_rule=1040, pfioc_rule=3104; DIOCINSERTRULE 0xcc20441b
(`0x100004a94`), DIOCDELETERULE 0xcc20441c (`0x1000045a0`),
GETRULES/GETRULE 0xcc204406/407 (`0x100006130/63c4`), STARTREF 0x40084408
(`0x100006220`), STOPREF 0xc0104409 (`0x100005efc`). pf_ioctl.c implements
all. Earlier manual v28 PF use establishes /dev/pf runtime availability, not
native PFManager success. pfd and mDNSResponder DNS-proxy Mach services are
already staged by the demand-job pass.

The missing launch piece was **bootps.plist**, omitted because it has no
MachServices. mkjobs explicitly stages it when misd is staged, preserving label
com.apple.bootpd, Disabled=true, inetd Wait=true, port 67 and ReceivePacketInfo.
misd enables it and writes
`/Library/Preferences/SystemConfiguration/bootpd.plist`; stock bootpd uses BPF
transmission, avoiding the previous broadcast routing problem.

No concrete native IP ABI blocker was found, so **no automatic manual-helper
fallback or competing DHCP/NAT owner was added**. Settings needs no switch
files. The legacy helper remains explicit for USB testing and must be idle
for native Wi-Fi. Native PF/bridge/bootpd/DNS transactions remain unverified.

The new helper only observes: read-only PF queries check running/forwarding/NAT
on pdp_ip0; read-only ap1 BPF captures UDP server replies from 67/53 and logs
DHCP ACK/first DNS reply. It changes no addresses, routes, PF or DHCP/DNS
services. Log `owner=` labels identify intended native ownership; observing a
rule/packet alone does not attribute it to misd/mrc_dns_proxy. Actual service
logs, PF counters and client traffic must substantiate native operation.

### Files, tests and artifacts

Paths below are relative to `D = devices/xiaomi-mi-10`:

| Files | Implementation |
| --- | --- |
| `overlays/xnu/files/bsd/dev/{hakios.apple80211.c,umi_apple80211_ap.inc}` | Native requests/status/events. |
| `overlays/xnu/files/bsd/dev/{umi_wifi_ap.inc,hakios.net.c}`, `overlays/xnu/files/bsd/sys/{hakios.lx.h,hakios.net.h}` | ap1 lifecycle/status/control. |
| `overlays/xnu/files/iokit/Kernel/hakios.wlan.cpp` | Device-local virtual SoftAP registry; shared WLAN source unchanged. |
| `overlays/wifi/lx/src/{lx_80211.c,umi_wifi_ap.h,lx_wpa.c,lx_wpa.h,umi_wpa_auth.inc}` | Authenticator, RSN/security, key/data/peer lifecycle. |
| `overlays/userland/src/hakios_hotspot_native.c`, `overlays/userland/com.hakios.hotspot-native.plist` | Activation XPC and passive diagnostics. |
| `overlays/userland/{build.sh,mkjobs.py}` | Small compile/install, replacement job, wifid umask, native bootps staging. |
| `overlays/userland/src/umi_wifi_ap.c` | Explicit legacy open helper now addresses ap1; Settings does not use it. |
| `tools/{build-hotspot-native-helper.sh,test-wpa-auth.py,test-hotspot-native.py,test-wifi-ap-datapath.py}` | Standalone build and host tests. |

Tests performed locally, with no phone/network mutation:

```sh
/Users/xm666/X/umi-ios/tools/uefi-venv/bin/python devices/xiaomi-mi-10/tools/test-wpa-auth.py
python3 devices/xiaomi-mi-10/tools/test-wifi-ap-datapath.py
python3 devices/xiaomi-mi-10/tools/test-hotspot-native.py
python3 devices/xiaomi-mi-10/tools/test-hotspot.py
bash devices/xiaomi-mi-10/tools/build-hotspot-native-helper.sh
bash devices/xiaomi-mi-10/tools/build-wifi-ap-helper.sh
```

All pass. Actual overlay crypto under ASan/UBSan: **195 checks, zero failures**,
including IEEE 802.11-2020 J.4.2 PMK, FIPS SHA1/AES, RFC2202/3394 and STA
handshake tests; additionally 32 independently generated AP PMK/PTK/M1–M4/
wrapped-GTK cases compare with Python hashlib/hmac/cryptography. Negative
MIC/replay/RSN/truncation and retry/no-reinstall pass. Actual secure ingress,
decapsulation/TX selection, native decoder, 100,000 malformed packets, generated
launch jobs, early-association control completion/restart/failure/already-off/
STA-power cases pass. Existing DHCP-BPF/PF/rollback tests and 20 legacy parser/
IPv4-ABI cases pass. The exact new build stanza also compiles in a temporary
snapshot/symlink layout. `nm -u -m` has only libSystem imports, no dynamically
looked up symbols; shell syntax and mkjobs Python compile pass.

Wi-Fi and kernel builds used **only build-lock.sh**. Final kernel build and copy
ran inside the same lock using device build-kernel.sh, `-j8 BUILD_DSYM=0`.
Logs: `R/out/xiaomi-mi-10/hotspot-native2-{prepare,wifi-build,kernel-build}.log`.
Wi-Fi object: 18 driver files, zero external stubs. Copied kernel contains AP
control, SoftAP IOKit and authenticator symbols.

- Kernel: **`R/out/xiaomi-mi-10/flash/kernel-hotspot-native2`**, SHA-256
  **`1a3ebff221bb707194569bd7c2270dda6354b94313fe4826c391dcf21b40d1ae`**.
- Helper: **`R/out/xiaomi-mi-10/hotspot-native2/hakios-hotspot-native`**, SHA-256
  **`21c2e61d4cbf022c5018df46730d9180cc7a42ca88ae4d64a0701e2adafd1a36`**.

Old kernel-* artifacts were preserved. Ship matching helper, replacement
hostapd job, private wifid umask and bootps job with the kernel: a kernel-only
swap retains incompatible stock authentication/launch behavior. The supervisor
performs the merged payload build; none was run here.

### Good/bad logs and exact supervisor v29 checklist

Expected source strings/examples, **not captured device success**:

```text
hotspot-ap: interface ap1 publish role=SoftAP ready=1
hotspot-ap: Settings interface create role=7 interface=ap1 error=0
hotspot-ap: Settings start received security=WPA2-PSK channel=6 ssid-length=<n> error=0
hotspot-ap: mode AP-only; STA withdrawn
hotspot-ap: GTK installed cipher=CCMP index=1
hotspot-ap: vdev up id=<n> channel=6 width=20
hotspot-ap: beacon start interval=100 dtim=1 ssid-length=<n> security=WPA2-PSK ap1
hotspot-ap: native authenticator ready security=WPA2-PSK engine=kernel
hotspot-ap: assoc <masked-MAC> aid=1 security=WPA2-PSK
hotspot-ap: 4-way message=1 attempt=1 submitted=1
hotspot-ap: 4-way message=2 MIC=ok message=3 submitted
hotspot-ap: 4-way handshake ok <masked-MAC> PTK=CCMP GTK=CCMP authorized=1
hotspot: native AP up interface=ap1 IP-owner=misd/bootpd/pfd/mDNSResponder
hotspot: NAT up owner=native-misd upstream=pdp_ip0 forwarding=1 observed=rules
hotspot: DHCP lease interface=ap1 address=<lease> mac=<masked> owner=native-bootpd observed=ACK
hotspot: DNS reply interface=ap1 owner=native-mrc-proxy observed=UDP
hotspot-ap: Settings stop received
hotspot-ap: teardown complete
hotspot-ap: STA restore error=0
hotspot: native AP teardown interface=ap1 IP-owner=misd/bootpd/pfd/mDNSResponder
hotspot-ap: interface ap1 removed
```

Bad gates: nonzero Settings/start/configure errors, channel refused, native
authenticator activation rejected, `4-way handshake fail` with MIC-or-format/
timeout/key-install, nonzero STA restore, or absent native DHCP/PF/DNS state.
Expected wrong-password MIC failure must yield no authorization/lease. Old
`DHCP send failed errno=51` during native testing means a legacy helper is still
active. Do not log passwords/PMKs/raw key requests; keep MACs masked.

1. **Deploy together:** new kernel, `/redwood/bin/hakios-hotspot-native`,
   replacement `/redwood/LaunchDaemons/com.apple.wifi.hostapd.plist`, regenerated
   wifid Umask=077 and bootps.plist; native misd/pfd/configd/mDNSResponder jobs.
   Preserve bootps Disabled/socket keys; misd enables it. Do not separately
   start stock hostapd. Use the supervisor's usual merged build/test harness.
2. **Disable old tests cleanly:** use the old helper's --off/--usb-off controls
   to stop live manual sessions and stop any explicitly applied open AP. Remove
   launch-job commands running `umi_wifi_ap --apply` or creating switch files.
   No wifi-ap/usb-on/wifi-on file is needed. Do not delete a live recovery
   journal or run two DHCP servers. Keep CommCenter untouched.
3. **Off regression:** hotspot off, verify en0 saved-network WPA2, DHCP, DNS,
   HTTPS and Wi-Fi off/on. Before first CREATE ap1 should be absent; no AP
   beacons/authenticator activation or hotspot sharing state. Record PF and
   forwarding baseline. Disconnect STA and verify cellular data independently.
4. **Settings start:** set device name and valid hotspot password in Settings,
   enable Allow Others to Join. Require CREATE94/HOST_AP25, BSD ap1/SoftAP
   registry property, configd/NWI state, vdev/beacon and authenticator readiness.
   Record request order/band/channel. If offered, test Maximize Compatibility
   for this 2.4 GHz-only port and record default-mode channel selection too.
   Do not use helper commands/manual switches to make activation succeed.
5. **Security:** scan from one awake client for exact device-name SSID and
   WPA2-Personal/CCMP privacy+RSN. Try wrong password: handshake failure, no
   authorized station or lease. Forget/rejoin using the Settings password:
   require association, M1→M2/M3→M4 and authorized=1. Healthy driver peer:
   found=1, authorized=1, dp-setup=1, tid-mask=0x1ffff, fouraddr=0. Completed
   key install must not repeat on a duplicate M4.
6. **Native IP:** inspect actual ap1/bridge100 address/subnet/gateway, generated
   bootpd.plist, bootpd socket, native PFUser/pfd rules and mrc_dns_proxy
   input/output. Do not assume the manual 192.168.2.0/24 pool. Require DHCP
   ACK plus client lease/router/mask/DNS. Retain native errors/state for any
   failed stage: the passive observer does not repair it.
7. **Prove cellular Internet:** disable the client's other uplinks and verify
   its route uses the hotspot. Test gateway ARP/ping, numeric IPv4, advertised
   DNS over UDP/TCP, HTTPS and a large download. Require increasing PF NAT
   counters/states and pdp_ip0 translated/return traffic. Observer NAT-up/ACK
   alone is insufficient. Record PMTU/MSS and cellular reconnect behavior.
8. **Renew/reassociate/sleep:** DHCP renew, clean disconnect/reassociate, repeat
   DNS/HTTPS; test phone/client sleep/wake, recording the known missing host
   power-save buffering. A vanished client's occupied slot currently needs
   Settings off/on. An awake-client pass does not establish sleep support.
9. **Settings off/repeat:** require teardown, link down/DELETE as wifid issues,
   SSID gone, keys/client removed, native sharing rules/DHCP/DNS cleaned up and
   forwarding baseline restored as appropriate for other services. Require
   STA restore error=0 and saved-network WPA2/DHCP/DNS/HTTPS rejoin. Repeat
   toggle, change name/password in Settings; require new name/key works and
   old key fails. Repeat with STA originally powered off and verify that
   preference is preserved after teardown.

Acceptance is Settings → WPA2 client → native lease/DNS/NAT → cellular Internet
→ off/STA restoration. Local build/tests establish implementation/ABI evidence,
not completion of those hardware acceptance gates.

## v29 completion (multi-client, HT, 5GHz, PS, kickout, rekey)

本版把 AP peer 从单一状态改为 firmware 能力驱动的 peer 表（至少 8 个）：每个地址独立保存 AUTH/ASSOC/WPA2 PTK、重试计数、AID、BA 和 RSSI。Get 42 返回全部已授权地址及 ath11k `rssi_comb`/RX 管线的 dBm；STA_ARRIVE、STA_LEAVE、RSN_HANDSHAKE_DONE 按客户端发送。GTK 不会因新客户端重装；默认 3600 秒轮换，逐 peer 完成 Group Key Handshake 1/2、2/2 后才切换 firmware group key。

Beacon、probe response、assoc response 现在带 HT Capabilities、HT Operation 和 WMM Parameter IE，assoc 解析对端 HT MCS/VHT MCS、WMM/uAPSD 并走 `WMI_PEER_ASSOC`；2.4 GHz 至少 HT20，5 GHz 在 firmware 支持时为 HT20/VHT20。信道只接受中国非 DFS 1–13、36–48、149–165，并与 ath11k regulatory 交集；Get 254 同样过滤 DFS/passive。TIM/DTIM、`AP_LINK_PS`、`BEACON_OFFLOAD`、`WMI_PEER_STA_PS_STATECHG` 和 uAPSD 由 firmware 处理缓冲，host 设置 listen interval、AID、WME；keepalive 为 30/60/65 秒。`WMI_PEER_STA_KICKOUT_EVENTID` 通过 `ieee80211_report_low_ack` 标记 peer，由 AP worker 删除、释放 slot 并发 STA_LEAVE。

### iOS 16.5 最大兼容性行为（逆向记录）

`WirelessModemSettings` 的开关由 MobileWiFi 的 `kWiFiPHBandPreferenceShowUIKey`（CF 字符串 `showPHBandUI`）控制，当前 Settings 方法为 `-[WirelessModemController _bandPreferenceSpec]`；偏好键是 `kWiFiPHBandPreferenceKey`（`phBandPreference`）。wifid 的 MIS 路径读取 `softapUserPreferredBand`，日志格式为 `MIS WiFi %s 5GHz SoftAP, Cellular Radio %s, User Preferred %s Band, Client Req %s`，并在 `__WiFiDeviceManagerCreateMISStartRequest` 中按频段编码 Set 25 的 `AP_MODE_AUTH_UPPER`/`AP_MODE_CYPHER_TYPE`。兼容性打开会走 `SOFTAP_FORCE_2_4G_CHANNEL`，请求 2.4 GHz；关闭时请求 5 GHz。16.5 的日志明确 WPA2-only（`Allowing WPA2 keying only`）；当 Wi-FiManager 的 `WPA3PersonalHotspot` feature 和 hotspot WPA3 capability 同时开启时，wifid 才生成 WPA2+WPA3 transition upper auth（否则 WPA2 upper=8、cipher=10）。本移植当前实现了 wifid 实际 WPA2/CCMP 请求，未声明 WPA3 capability，因此不会伪装降级；`showPHBandUI` 的出现条件由 Wi-FiManager 多频段/5 GHz capability 与 `Get 254` 返回 5 GHz 活跃信道共同满足。

客户端上限不再使用固定小数：AP 启动读取 `ar->max_num_stations`，以该值和编译表容量的较小者作为 live capacity，低于 8 直接拒绝启动；TIM bitmap、Get 42 和 peer 回收均使用 live capacity。

Host tests:

```sh
/Users/xm666/X/umi-ios/tools/uefi-venv/bin/python devices/xiaomi-mi-10/tools/test-wpa-auth.py
python3 devices/xiaomi-mi-10/tools/test-wifi-ap-datapath.py
python3 devices/xiaomi-mi-10/tools/test-hotspot-native.py
python3 devices/xiaomi-mi-10/tools/test-hotspot.py
```

以上均通过；覆盖 32 组 WPA2、peer 数据路径、HT/WMM/TIM/5 GHz 信道、kickout/PS/rekey 合同、native decoder/DHCP/DNS/PF 回归。真机仍需验收：兼容性开关是否显示；开关两态 Set 25 的 band/channel/auth/cipher 日志；8 台客户端并发、Get 42 RSSI、5 GHz 36/149/165 与 DFS 拒绝、HT/WMM 抓包、睡眠唤醒的 PS-Poll/uAPSD/DTIM 广播、65 秒 kickout 回收，以及 3600 秒 GTK 轮换不中断。

构建产物（均通过 `build-lock.sh`）：

- `out/xiaomi-mi-10/flash/kernel-hotspot-native3`，SHA-256 `649d0aa7088365c16e7c807b86973d8874b461da10551950126a7d6be68d570f`。
- `out/xiaomi-mi-10/hotspot-native3/hakios-hotspot-native`，SHA-256 `cf9a4b1a50a4044f70f83ec93cf872d6444de81f3e3bdd4a97d37423596f29a4`。


## v29d Settings hang audit (2026-11-12)

**Settings 页面硬挂死的根因尚未证实，不能把本版称为该挂死已修复。**
已用实际代码的 ASan/UBSan harness 复现并修复另一个 native-3 缺陷：
XNU peer 表 128 个槽均已关联时，新地址会以哨兵 `128` 索引表外；
现在在满表时释放锁并结束本次发布，原客户端不受影响。该触发条件需要
AP 客户端，不能解释尚未创建 AP 时的页面挂死。

v29c 镜像的回调表已核对：40 字节，stations 在 +24、shutdown 在 +32，
与 XNU ABI 一致。实际 lx `ap_stations()` 在 STA 状态下于 AP mutex 之前返回；
Get 254 只读 STA 缓存信道。两份失败 USB 日志没有任何 Apple80211 行，
包括启动事件；原普通 printf 未直接进入 USB RAM 日志，因此无 CREATE 行
也不能证明没有进入 CREATE。

新增 `ap-trace:` 直写 USB RAM 日志：所有 Apple80211 Get/Set 请求号（含未知
MIS/band 请求）、en0/ap1、入口/出口、copyin/copyout、Apple80211/AP/peer 锁
等待/持有/释放、CREATE/DELETE 的 ifnet 与 IOKit 发布、驱动启动/停止、
worker、100 ms handshake timer、kickout/rekey。按线程、阶段、subject、value
和一秒窗口去重，无累计条数预算；不记录用户缓冲区、req_val、SSID 或密钥。
`subject` 在 ioctl 行为请求号，在 lock 行为固定探针站点，在 timer 行配合
`value` 表示毫秒；日志以 `tid` 关联同线程的最后 wait/enter 与 held/exit。

主机测试通过：test-hotspot-queries.py（实际 dispatcher/Get42/Get254/lx RSSI/
锁、29 万次混合调用、四线程 8000 次 Get42、满表越界/回收、探针去重）、
test-hotspot-native.py、test-wpa-auth.py（含 STA 195 项）、
test-wifi-ap-datapath.py、test-hotspot.py、test-upstream-sync.py。
STA 通用 Get/Set 和 IOKit/firmware 操作在新 harness 中使用替身，未模拟
全系统调度与硬件；硬挂死未在主机复现。

Wi-Fi lx 对象已重编，内核通过 build-lock.sh 和指定 build-kernel 命令构建。
产物 `out/xiaomi-mi-10/flash/kernel-v29d`，SHA256
`68a04db1b6305e3b88ce8a20d17d9d8fe10fac369c0d1ddffc773bc169779a44`，
`__TEXT_EXEC=0x950000`；五个对象的持久化 `-Os` 保留。完整证据、日志和布局：
`/Users/xm666/X/umi-ios/work/v29d/REPORT.md`。未操作手机。


## v29f 挂死诊断探针（2026-11-12）

依据 `/Users/xm666/X/umi-ios/work/v29e/INVESTIGATION-hotspot-hang.md` §5–§7：
v29e 的硬挂死发生在 SCAN_REQ 成功（固件已确认 `WMI_SCAN_EVENT_STARTED`）之后约 55 ms，
整核级停顿；而内核 `printf`、panic 文本、`ath11k_warn` 都不进 USB 实时日志，所以死点不可见。
本版**不改扫描/AP 的业务逻辑**，只做两件事：(1) 把"黑箱"变成可读的日志（P0/P1/P2/P3）；
(2) 消掉日志锁这个放大器（F3）。另加一条独立的小修正（F1：主动扫描跳过不允许发送的信道）。

不加任何 boot-arg（boot 行仍接近 607 字符上限），不记录 SSID 字节、口令、其它设备 MAC。

### 改动清单

| 文件 | 位置 | 内容 |
|---|---|---|
| `overlays/wifi/lx/src/lx_80211.c` | `station_scan()` 信道收集 | **F1**：主动扫描跳过监管域里 `IEEE80211_CHAN_NO_IR` / `RADAR` 的信道；若过滤后一条不剩，再按原列表扫一次（请求只点名这类信道时仍然扫） |
| 同上 | `station_scan()` 请求 `hw_scan` 之前 | **P1**：每次扫描一行解码后的参数（信道 MHz、是否只监听、dwell、SSID 长度、点名数、被跳过的信道数） |
| 同上 | 新增 `scans_asked/scans_ended/scans_given_up/scans_cancelled`；`linuxkpi_ieee80211_scan_completed()`、cancel 路径 | **P2**：异步路径计数；新增 `lx_station_counts()` |
| `overlays/wifi/lx/src/lx_bus.c` | `driver_complains()` | **P0**：`ath11k_warn/ath11k_err` 按格式串去重限流（同一格式串 ≤1 行/秒，下一条带上被压缩的条数） |
| 同上 | `bus_looks()` | **P2**：每 5 s 一行 `bus 5s:`，含各 CE 接收环 `sw_index` 增量、DP 服务条数、扫描状态等 |
| 同上 | 新增 `lx_counts_get()` | 汇总 wifi 侧计数（不加锁、不睡眠） |
| `overlays/wifi/lx/src/lx_base.c` | `ticker()` | **P3**：每秒一行 `alive:`，仅当数值变化时打印 |
| `overlays/wifi/lx/src/lx.h` | 新增设备覆盖文件 | `struct lx_counts` 与两个取数函数（`wifi/lx/src/lx.h` 是上游模板，按 `prepare.py verify` 的要求只能覆盖、不能改） |
| `overlays/xnu/files/bsd/dev/hakios.lx.c` | `lx_os_panic()` | **P0**：`panic()` 之前先 `hakios_tl("lx: PANIC: …")` |
| `overlays/xnu/files/bsd/dev/hakios.wifi.c` | `say_line()` | **F3**：整行一次 `loader_log_line()`，不再逐字符抢日志锁（保留 `say_lock` 对 `printf`/保留环的保护） |
| `xnu-device.patch` | `osfmk/arm64/loader_return.c` 段 | **P0/F3**：`psci_smp_stop_others()` 前后各一行、quiesce 之后一行；日志锁自旋上限 1000 万 → 10 万，超时后在下一次拿到锁时写一行说明（只写一次）；`loader_return.c` 属模板文件，改动只能走设备 patch |

### 新增日志行（全部经 `loader_log_line()` 进 USB 实时日志）

| 行 | 何时 | 读法 |
|---|---|---|
| `lx: PANIC: <文本>` | `lx_os_panic()`：`word_lock` 同线程重入等 lx 侧 panic | 出现即 C1 成立：panic 真的发生了，文本就是原因 |
| `loader_return: the way back to the loader is being taken` | `loader_return()` 第一行 | 出现表示 panic/退出路径已经开始走 |
| `loader_return: the other processors are stopped (or this one is waiting to be)` | `psci_smp_stop_others()` 返回之后 | 有它 = 其它核打掉了；**只有前者没有它** = 卡在 `psci_smp_stop_others()` |
| `loader_return: the drivers are quiet; the log is collected` | 全部 `hakios_*_quiesce()` 之后 | 有它但没有 `--- panic buffer ---` = 卡在 quiesce（UFS/GPU/wifi/adsp/mpss/DDR…）或日志整理 |
| `lx: scan args: n=3 ch= 2412 2437 2462, any network, dwell=120, ssid_len=0, 0 named, 0 left where it may not send` | 每次扫描，`hw_scan` 之前 | 热点页那 3 个信道到底是什么、是否含 DFS/5 GHz、dwell 是否异常；`left` >0 表示主动扫描时被监管域挡掉的信道 |
| `lx: bus 5s: ce-rx=[ 0 3 0 …] polls=+5000 served=+1234 scan=1 scans=172 ended=171 gaveup=0 cancelled=0 rx=42 heard=9 complaints=0` | 每 5 s（总线起来之后） | `ce-rx` = 各 CE 接收环这 5 s 被固件推进了多少格（WMI/数据都从这里上来）；`polls` = `wifi-rings` 线程这 5 s 转了多少圈（轮询模式约 5000）；`scan` 0 无/1 进行/2 结束/3 被驱动放弃 |
| `lx: alive: polls=… served=… rx=… heard=… scan=… scans=… ended=… gaveup=… cancelled=… complaints=…` | 每秒，**仅当任一数值变化** | 这一行来自每 2 ms 睡一次、跑定时的线程：它还在走 = 内核还在切线程、定时器还在 |
| `ath11k WARNS (+12 more since the last one): …` / `ath11k ERROR …` | 驱动 warn/err | 同一格式串 1 行/秒；括号里的数是被压掉的条数。注意：这两条本来就走 `lx_tell`（可见），过去只是没有限流 |
| `--- the log was written without its lock: whoever held it was gone ---` | 日志锁自旋超时后、下一次成功拿锁时（仅一次） | 出现即说明有核在日志锁上空转过 10 万次；本行之后的行可能与被弃锁时的写入交错 |

### 挂死后怎么读（v29f 的判定树）

1. 在日志末尾（`logs/test-*.kernel.log` / 实时 `-run.log.live*`）找最后 20 行：
   - 有 `lx: PANIC:` → C1；文本直接给出 panic 原因。
   - 有 `loader_return:` 三行中的前一两行 → 死点在回 loader 的路上（`stop_others` 或 quiesce），
     说明"panic 发生了但没走完"，不是"没有 panic"。
2. 比较最后一条 `lx: alive:` / `lx: bus 5s:` 与显示帧行、`ap-trace:` 行在日志中的**先后顺序**：
   - 帧行和 `ap-trace:` 还在出、`alive` 停了 → 卡的不是调度器（更可能在别的核/别的锁）；
   - `alive` 还在出、`bus 5s` 的 `polls` 不涨 → `wifi-rings` 线程被卡住（就是异步路径）；
   - `ce-rx` 全 0 而 `polls` 继续涨 → 固件/PCIe 不再交付（C3），此时 `scan` 多半停在 1；
   - `alive` 也停、`bus 5s` 也停 → 整核级停顿（与 v29e 现象一致），再看是否有 `loader_return:` 前行。
3. 最后一条 `lx: scan args:` 给出出事那次扫描的实际参数（信道/dwell/passive），
   与之前成功的若干次对比即可判断是"同样的请求"还是"不同的请求"。
4. 若日志里出现 `the log was written without its lock`，说明 F3 的放大器已经生效于该轮，
   该行附近的行序不可靠，但"某一段之后再无任何行"这个结论仍然成立。

### 构建

1. **必须先重编 Wi-Fi 对象**（改了 `wifi/lx/src` 的三个 .c 和 `lx.h`）：
   `bash devices/xiaomi-mi-10/build-wifi.sh`
   （ath11k 源码来自 `xnu-deps/build/ath11k/src`（固定 Linux 7.0.14 解包），
   `xnu-deps/build/ath11k` → `out/xiaomi-mi-10/wifi-build/xnu-deps/build/ath11k`；
   **不需要**动 `ios-port-research` 里的任何副本，本树已无该软链。）
2. 再编内核（会通过 `xnu-device.patch` 带上 `loader_return.c` 的改动）：
   `bash devices/xiaomi-mi-10/build-kernel.sh`（`build-lock.sh` 由 supervisor 执行）。
3. 无需新 boot-arg；boot 行长度不变。

### 未做 / 已知风险

- **P3 的"显示帧号 / ioctl 次数"没有并入 `alive:` 行**：帧计数器在 display 驱动里未导出，
  且本轮不允许改 display 文件；ioctl 次数在 `hakios.apple80211.c`（内核侧），
  Wi-Fi 对象看不到。这两轴在**同一份日志里**由既有的逐帧行和每秒去重的 `ap-trace:` 行承担，
  顺序比较即可定位"谁先死"。若下轮仍需单行汇总，需要在 `hakios.lx.h` 上加一个取数调用（另一次 build-wifi）。
- `ath11k_warn/err` 去重表 16 槽、无锁：并发下可能偶发重复行或顶掉一个槽（仅影响诊断，不影响驱动）。
- F1 会让**主动**扫描少扫几条 DFS/静默信道（功能影响小）；被动扫描（`scan_type == 2`）不变。
  这条不能修复本次挂死，只是消除"在 DFS 信道上主动发 probe"这一类隐患。
- 全部探针都是"行级、去重、有上限"：最坏情况新增日志量约 1 行/秒（`alive:`）+1 行/5 s（`bus 5s:`）
  +1 行/扫描 + ≤1 行/秒/格式串（ath11k）+3 行/退出，不会把日志环写爆。

## v29g 挂死探针与防御性修复（2026-11-12）

依据 `work/v29f/INVESTIGATION-hotspot-hang2.md` §5–§7。本版只做三件事：
(1) **P-A / FIX-A4**：让 SError/panic 路径在死点之前就能在**实时日志**里留下一行；
(2) **FIX-A1**：设备"没应"（读到 `0xffffffff`）时退避 + 计数 + 报一次；
(3) **FIX-A3**：默认**禁止 IMPS**（`idle_ps=false`），并留一个开关文件恢复旧行为。

**有意不做 FIX-A2（扫描裁剪）**：要证据，不要掩盖触发窗口。
不加任何 boot-arg（boot 行长度不变）；ath11k 源码一行未改（仍为 Linux 7.0.14 原样解包）。

### 改动清单

| 文件 | 位置 | 内容 |
|---|---|---|
| `xnu-device.patch` | `osfmk/arm64/sleh.c` `arm64_platform_error()` 首行 | **P-A**：`lx: PLATFORM ERROR esr=… far=… elr=… source=…` 一行，直接走 `loader_log_line()`（loader 从另一个核读的日志），自旋有上界、不分配、不拿可能已被本核持有的锁 |
| 同上 | `osfmk/kern/debug.c` `panic_trap_to_debugger()` 首行 | **P-A / FIX-A4**：`panic: the panic path is entered (called from 0x…)`；`panic()`/`panic_plain()`/`panic_with_options()` **全部**汇到这里 |
| 同上 | `osfmk/kern/debug.c` `panic_spin_forever()` | `panic: panic_stop() reached; the way back to the loader begins`（在既有的 `loader_return()` 之前） |
| 同上 | `osfmk/arm64/machine_routines.c` `halt_all_cpus()` | `halt: halt_all_cpus is taking the machine down[ for a restart]` |
| `overlays/wifi/lx/src/lx_bus.c` | `struct bus`（新字段）、`bus_answered_nothing()`、`bus_read32()`、`bus_write32()` | **FIX-A1**：读到 `0xffffffff` ⇒ `dead++`、置 **100 ms 静默窗**（窗内读返回 0、写丢弃，都计入 `held`）、**首次**与**空闲>1 s 后首次**各打一行；其余全部只计数 |
| 同上 | `window()`（umi 翻窗分支） | 写 `0x310c` 的**读回值现在被比较**（原来 `ioread32` 的返回值被丢掉）；不一致计入 `winsel`（H1c） |
| 同上 | `bus_looks()` 的 `bus 5s` 行 | 行尾追加 `dead=%llu held=%llu winsel=%u`（周期聚合；不按次打印） |
| 同上 | `bus_get_user_msi_vector()` | **MSI**：`!bus.irq` 时返回 `-EINVAL`；`ce.c`/`dp.c` 的 ring setup 因此**不**置 `HAL_SRNG_FLAGS_MSI_INTR`，固件收到的 MSI 地址为 0（与 WCN6750 侧目前的行为一致，见下"MSI"） |
| 同上 | `bus_start()` | 每次 start 一行：本轮 rings **有没有**被写入 MSI 地址 |
| 同上 | `lx_start()`（`ath11k_core_pre_init()` 之后、`ath11k_hal_srng_init()` 之前） | **FIX-A3**：`ab->hw_params.idle_ps = lx_os_option("wifi_imps")`，默认 0（禁止 IMPS）；并打一行 `idle_ps=N` |
| `overlays/xnu/files/bsd/dev/hakios.lx.c` | 新增 `lx_os_dbfile()`；`lx_os_option("wifi_imps")` | 开关文件读取，写法照 `hakios.cpufreq.c: cpugov_switch()`；返回值区分 **文件在 / 文件不在 / 还没有文件系统**，并各打一行 |
| 同上 | 同文件顶端的 include | 只加了 `#include <sys/vnode.h>`、`<sys/vnode_internal.h>`；**没有**改 `bsd/sys/hakios.lx.h`（所以 wifi 对象不必等内核先 stage 头文件） |

### 两处"看起来该做但没做"的

- **每次访问前写 `PCIE_SOC_WAKE`：不做。** ath11k 里确实有 `ath11k_pci_force_wake()`
  （`pci.c:370-373`：写 `PCIE_SOC_WAKE_PCIE_LOCAL_REG(0x3004)=1`，然后 `mdelay(5)`），
  但它只在 `ath11k_pci_power_up()/power_down()` 调用（`pci.c:849`）。每次寄存器访问前做
  5 ms 延时会把轮询（≈865 次/s）毁掉，报告里"每次访问都唤醒"没有任何上游依据，所以不加。
- 除 IMPS 之外，本版只有两处**行为**改动：设备"没应"时退避 100 ms（FIX-A1），以及不给环编 MSI（见下）。

### MSI（报告 §7 第 4 条）的判定与处理

证据（已核实）：umi 路径在 `lx_start()` 里**无条件** `set_bit(ATH11K_FLAG_MULTI_MSI_VECTORS)`、
装 `bus_ops_irq`、并把 `lx_os_irq_msi()` 得到的 GIC ITS doorbell 地址填进 `ab->pci.msi`
（`lx_bus.c:1022-1036`）；而 `bus.irq` 在 umi 上恒为 false（`hakios.lx.c` 的
`lx_os_option("wifi_irq")` 返回 0），于是 `bus_start()` 里的 `irq_attach()`（也就是
`lx_os_irq_attach()`）**从不执行**。ce.c:530-552 / dp.c:183-221 却照样把
`HAL_SRNG_FLAGS_MSI_INTR` 置上，`hal.c:278-292/357-372` 把 ITS 地址写进每个环，
`dp_tx.c:959-961` 再把它告诉固件 ⇒ **固件整轮都在向一个没人 attach、没人 mask 的 SPI 写 MSI**。

处理（small & clearly safe）：`bus_get_user_msi_vector()` 在 `!bus.irq` 时返回 `-EINVAL`。
`ath11k_get_user_msi_vector()`（`hif.h:126-135`）把这个当"没有"，ce.c/dp.c 因此提前 return，
`HAL_SRNG_FLAGS_MSI_INTR` 不置，`hal.c` 不写 MSI1 寄存器，`dp_tx.c` 发给固件的
`ring_msi_addr_lo/hi` 与 `msi_data` 全为 0。
**安全依据**：WCN6750 侧（`bus_ops`，`hif.ops->get_user_msi_vector == NULL`）本来就是这条路，
一直工作；MSI 关掉后环的 HP/TP 与 shadow register 逻辑不变，轮询照旧。
`wifi_irq=1` 的调试路径（`bus.irq=true`）行为完全不变。

### 新增/变更的日志行

| 行 | 何时 | 读法 |
|---|---|---|
| `lx: PLATFORM ERROR esr=… far=… elr=… source=…` | 每次 `arm64_platform_error()`（SError / 同步平台错误） | **H1 坐实**。`far` 落在 `0x6030xxxx`（PCIe0 窗口）/`0x61exxxxx` = 我们对 radio 的一次访问；`elr` 是出错指令（`ioread32/iowrite32` 的调用点）；`source` 0=同步 1=异步 |
| `panic: the panic path is entered (called from 0x…)` | 每次 panic 进入 | 有它 = 确有 panic；`called from` 是 `panic()` 的直接调用者（用 `keepsyms`/`nm` 反查） |
| `panic: panic_stop() reached; the way back to the loader begins` | 走到 `panic_stop()` | 有它但没有 `loader_return:` ⇒ 卡在 `DebuggerTrapWithState`/停核之前 |
| `halt: halt_all_cpus is taking the machine down…` | `halt_all_cpus()`（重启/关机/panic 出口） | 与 `loader_return:` 对照即可定位 |
| `lx: the chip answered 0xffffffff at its 0x…: … left alone for 100 ms (N such reads in all)` | 读到全 1，且是**首次**或**空闲 1 s 后首次** | 设备在一次访问上"没应"（H1a/H1b 直接证据）；同刻 `bus 5s` 的 `dead=`/`held=` 看规模 |
| `lx: bus 5s: … dead=N held=N winsel=N` | 每 5 s（既有行，行尾新增三个累计数） | `dead` 全 1 读次数；`held` 被退避挡掉的读写次数；`winsel` 翻窗读回不一致次数（H1c） |
| `lx: where the radio would raise an interrupt: NOWHERE -- …` / `… the GIC's MSI address is given …` | 每次 bus start | 确认本轮 MSI 是否被编程；正常应为 **NOWHERE** |
| `lx: IMPS: the firmware may not enter it (no /private/var/db/umi-wifi-imps-on)` | 驱动 start，开关读取 | 三种后缀：`… is there` / `no …` / `NO FILE SYSTEM YET to look for …` |
| `lx: the firmware may NOT take the radio into IMPS while it is idle (idle_ps=0: mac_start() will not ask the firmware for it)` | 驱动 start，生效值 | 与上一行成对；`idle_ps=1` = 开关文件生效 |

### 下一个挂死怎么读（v29g 判定树）

1. 末尾 20 行，按**从下往上**找第一行命中：
   - `lx: PLATFORM ERROR …` → **H1**（硬件/总线中止）。看 `far`：`0x6030…`/`0x61e0…` = 我们的 window 访问；
     `elr` 就是那条访问指令；`esr` 的 DFSC（bit 0-5）与 EA（bit 9）说明是"有应答的错误"还是"根本没应答"。
   - `panic: the panic path is entered` 但**没有** `PLATFORM ERROR` → panic 走的是软件路径，`called from` 就是线索。
   - `panic: panic_stop() reached` 但**没有** `loader_return:` → 死在停核/取日志锁之前（下一步查 `psci_smp_stop_others()`）。
   - `halt: …` → 走到 `halt_all_cpus()`（重启/关机，或 panic 的出口）。
   - **一条都没有**（连 `loader_return:` 也没有）→ **H3**（关中断自旋 / 内核锁被偷）或纯总线 stall：
     下一轮做 P-B（250 ms 心跳线程 + 最后一次 MMIO 记录）。
2. `lx: the chip answered 0xffffffff …` 出现 → 设备确实"没应"。
   - 只有 1~2 次、随后恢复 → 一次坏访问被接住了（H1a 的**直接**证据）。
   - 周期出现、`dead` 一直涨、`held` 远大于 `dead` → 设备/链路已经不再应答，退避在起作用（正是要的证据）。
3. `winsel=` 在涨 → **H1c**：翻窗写没生效，后续访问可能落到别的窗口（本移植特有的写法）。
4. `IMPS` 两行确认本轮是 `idle_ps=0`（默认）。若这两行显示 `may enter`，说明 `/private/var/db/umi-wifi-imps-on` 在场 —— 别忘了它是本轮的实验变量。
5. 与 `lx: alive:` / `tl:` 帧行 / `ap-trace:` 的**先后顺序**照 v29f 判定树读（那三条判据的含义没变）。

### IMPS 开关文件与它的局限

`/private/var/db/umi-wifi-imps-on` 存在 ⇒ 恢复旧行为（允许 IMPS）；不存在 ⇒ 本轮默认（禁止）。
它**只在驱动 start 时读一次**（`lx_start()`），而 umi 的 start 发生在 `umi-wifi` 后台线程里、MHI/QRTR 之后，
通常已经在 root 挂载之后，但**不保证**：若日志里是 `NO FILE SYSTEM YET`，说明那次 start 时 `/var` 还看不到，
文件**没有**生效（此时行为等于"文件不在"）。当前没有 Wi-Fi 热重启路径，所以改文件后要**重启一次**。

### 构建

需要**两次**，顺序不变（先 Wi-Fi 对象，后内核）：

1. `bash devices/xiaomi-mi-10/build-wifi.sh` —— `lx_bus.c` 改了（本版**没有**改 `bsd/sys/hakios.lx.h`，
   所以不必先编内核来 stage 头文件）。
2. `bash devices/xiaomi-mi-10/build-kernel.sh` —— `hakios.lx.c` 与 `xnu-device.patch`
   （`sleh.c`、`debug.c`、`machine_routines.c`）改了；`prepare.py kernel` 会重新应用设备 patch。
3. 无需新 boot-arg；boot 行长度不变；ath11k 源码与 Linux tarball 不用重取。

两次都必须做：只编内核，`lx_bus.c` 的 A1/A3/MSI 改动不生效；只编 Wi-Fi，panic 探针与开关不生效。

### 风险

- **`0xffffffff` 退避 100 ms**：若某个寄存器正常就读出全 1，会周期性盲 100 ms。会以 `dead=`/`held=` 与那条一次性日志显形。
- **禁止 IMPS**：空闲功耗/发热略升（报告称之为"权衡"，本轮接受）。
  `idle_ps=false` 还改变了 `mac.c:8287/8300` 的 6 GHz 功率表口径（不再与固件省电时上报的
  `max_allowed_tx_power` 取 min）；本机只有 2.4/5 GHz，实际影响很小。
- **环不打 MSI**：与 WCN6750 侧一致；唯一变化是设备不再向 ITS doorbell 写（本轮认为这更安全）。
- **三行 panic 探针**都在异常/panic 上下文里调用 `loader_log_line()`：有界自旋（10 万次）、
  不改 DAIF 的持久状态、不分配、不睡眠。
- `lx_os_dbfile()` 在驱动 start 线程（可睡眠）里调用一次；`rootvnode == NULLVP` 时直接返回 -1，不碰 VFS。

## v29h 最后一次 MMIO 记录（P-C：挂死时把访问环打到屏幕）（2026-11-12）

依据 `work/v29g/INVESTIGATION-hotspot3.md` §6 P-C/P-E/P-F。本版只做一件事：
**让"最后一次 MMIO"活过整机停顿，并在挂死后由 loader 打到手机屏幕上**（另存进 loader 的日志，
供仍能连上的宿主抓取）。不加 boot-arg，不改 `SILENCE_SECONDS`，不碰 ath11k 源码。

### 先纠正 v29g 报告里的一条推断（重要）

v29g §3.2 认为 "THE KERNEL HUNG …" 那段**已经打到手机屏幕上**、只是宿主没抓到。**不对**：

- 当天的运行是 `xl` 的 USB 模式（`usb-payload.bin`，loader 的 `usb_command_loop()`），
  该函数第一行 `screen_quiet = 1`（`loader.c:4702`），此后 `put_char()` **不写屏幕**，只写 `log_buf`。
- 而且 `usb=1` 的运行**根本走不到** `run_kernel()` 里那段 "THE KERNEL HUNG"：
  `run_on_other_cpu()` 在把 status 返回给 `run_kernel()` **之前**就调用了
  `usb_kernel_reset()`（`loader.c:2718` → `:4257`），直接冷复位。
  所以 v29g 抓到的是 `usb: autonomous Android reset (kernel return/deadline)` 之后立即复位，
  `THE KERNEL HUNG` 与 `write_logs()` 都没执行。

因此 P-C 的打印点放在 **`run_on_other_cpu()` 返回之前**（`report_hung_kernel()`，`loader.c:1154` 附近），
并且**强制写屏**（新标志 `screen_forced`）。v29f/v29g 的死亡现场只有这条路能取证。

### 改动清单

| 文件 | 位置 | 内容 |
|---|---|---|
| `overlays/xnu/files/bsd/sys/hakios.lx.h` | 新增一节（`struct lx_mmio_access` / `struct lx_mmio_ring`、`LX_MMIO_*`、`lx_os_mmio_ring()`） | 环的**唯一定义**：内核侧与 Wi-Fi 对象都编这个头；loader 里有同构副本（见下）。16 项，每项 64 B，环 1064 B，`magic="LXMMIORG"`，`version=1`，自带 `size` 供 loader 拒绝异构 |
| `overlays/wifi/lx/src/lx_bus.c` | 文件作用域 `mmio`；`mmio_cpu()/mmio_window()/mmio_begin()/mmio_end()`；`window()`；`bus_read32()`；`bus_write32()`；`lx_start()` | **P-C**：每次真实访问前写环（`pending=1`）、返回后写值并清 `pending`。无锁：`__atomic_add_fetch(&seq)` 取号、`seq % 16` 定槽；槽仍 pending 时**不覆盖**（`lost++`），停住的那次永久保留。`window()`（umi 翻窗）记为 `WINSEL` 项，`value`=写入的 select、`answer`=读回值 |
| 同上 | `lx_start()` 开头 | `lx_os_mmio_ring(&mmio, sizeof mmio)`：把环交给内核（一次） |
| `overlays/xnu/files/bsd/dev/hakios.lx.c` | 新增 `lx_os_mmio_ring()` | 只做校验与转发：`loader_live_mmio(ring)`（`osfmk/arm64/loader_return.c`） |
| `xnu-device.patch` | `osfmk/arm64/loader_return.c`：`struct loader_live` 末尾、初始化器、新增 `loader_live_mmio()` | **ABI 只追加**：`magic3="XNULIVE3"` + `volatile uint64_t mmio`；老内核没有这两项 ⇒ loader 读不到 magic3 ⇒ 不碰 `mmio` |
| `loader/loader.c` | `struct kernel_live`（加 `magic3`/`mmio`）、`struct lx_mmio_ring` 同构副本、`log_of_hung_mmio()`、`report_hung_kernel()`、`put_char()` 的 `screen_forced`、`run_on_other_cpu()` 尾部、`run_kernel()` 的 `OTHER_CPU_HUNG` 分支 | 挂死路径：打印 `THE KERNEL HUNG …` + 16 项（旧→新，`*`=没回来）+ "机器 N 秒后复位"；**强制写屏**；kernel log 只进 `log_buf`/`out_buf`（不上屏，免得把上面几行顶掉）；然后**在屏上停留 30 s**再复位 |

### 挂死后怎么读（v29h 取证流程）

1. **什么时候看**：机器冻住后 **约 50 s**（`SILENCE_SECONDS=50`，心跳 10 s 粒度 ⇒ 冻结后 40–55 s）
   屏幕会出现 `THE KERNEL HUNG on core 7: no sign of life for 50 seconds; its log follows`。
2. **拍下来**：随后是 `MMIO:` 两行表头 + **最多 16 行**，例如
   `MMIO   #1a2b3 cpu 700 t 3d2f1c: W 1e04000 -> 80000 win 57 val 1 ans 0 from fffffe0000abc123`
   和 `MMIO * #1a2b4 ...`（`*` = **这次访问没有返回**）。
   屏幕会保持 **30 s**（`HUNG_SCREEN_SECONDS`），然后自动复位进 Android。
3. **怎么判**：
   - `*` 行是 **R** ⇒ 读到一半没回来（H1 的"读"分支；v29g 的 `dead=0` 说明这种读此前没出现过）。
   - `*` 行是 **W** ⇒ 写没完成（v29g 报告排第一的假设）。
   - `*` 行是 **S** ⇒ 死在**翻窗**（写 `0x310c` 或读回）——这是本移植特有的路径，直接坐实 H1c。
   - `cpu` 是 MPIDR 低 24 位（Aff0–2；本机大核 4–7、小核 0–3，启动核 `bootcpu=7`）。
   - `from` 是**内核虚拟地址**（链接地址，等于内核二进制里 `nm` 的地址）：`from` 落在
     `ath11k_*`/`lx_*` 哪个符号上，就是谁发起的（`keepsyms=1` 时也可用内核符号表）。
   - `win` 是芯片窗口号（0 = 前 512K 直通；umi 下为 `(offset>>19)&0x3f`）。
   - `val`：读的返回值 / 写的值 / 翻窗的 select；`ans`：翻窗读回值（≠`val` 说明 `winsel`）。
   - 一行都没有 ⇒ 环没被发布（驱动没 start）或挂在与 radio 无关的地方（H2/H3 回到主线）。
4. **宿主侧**：环也写进了 loader 的 `log_buf`/`out_buf`（`oem log` 可抓）。但 v29f/v29g 的事实是
   **内核一停，宿主的 bulk 读就 `rc=-7`**，复位后设备离总线，所以**屏幕照片是主路径**；
   宿主能连上时 `usb-log.py` 的日志里也能看到同样的 `MMIO` 行。

### 构建（顺序有变：先 stage 头文件）

本版改了 `bsd/sys/hakios.lx.h`，而 Wi-Fi 对象编译时读的是**已 stage 的**内核树里的头文件，
所以顺序是"先 stage，再 Wi-Fi，再内核"：

1. `python3 devices/xiaomi-mi-10/prepare.py kernel` —— 只 stage（把 overlay 头文件与
   `xnu-device.patch` 应用到 `out/xiaomi-mi-10/xnu`），**不编译**。
2. `bash devices/xiaomi-mi-10/build-wifi.sh` —— 编出 `out/xiaomi-mi-10/wifi-build/xnu-deps/build/ath11k/out/ath11k.o`
   （经 `xnu-deps/build/ath11k` 符号链接给内核用）。`lx_os_mmio_ring` 属于 `lx_os_*`，stub 名单不受影响。
3. `bash devices/xiaomi-mi-10/build-kernel.sh` —— 编 `hakios.lx.c` + 应用 `xnu-device.patch`
   （`loader_return.c` 的新字段与 `loader_live_mmio()`），并链接新的 `ath11k.o`。
4. `bash devices/xiaomi-mi-10/flash/build.sh loader` —— 输出 **`out/xiaomi-mi-10/flash/loader.efi`**
   （不是 `loader-v12.efi`）。boot helper 里把 `XL_LOADER` 指到新文件，按现有惯例复制成
   `flash/loader-v13.efi` 再 `export XL_LOADER=$F/loader-v13.efi`；`loader.efi` 本身也会被覆盖为同一份。

boot-arg 白名单**未改**（本版不加参数，`umi_bootargs_ok`/`allowed[]` 不动），boot 行长度不变。
`flash/build.sh loader` 用 `-DLOADER_UMI`；`loader-prod` 不需要（本版的打印只在非 PROD 的
`run_on_other_cpu` 监控路径里，PROD 构建里那段本来就不存在——`#ifdef HAKIOS_LOADER_PROD` 的
紧循环会先命中；如需 PROD 取证，先不要用 PROD 构建）。

### 风险

- **屏幕能不能看见**：loader 的 `con_out` 写的是 UEFI 的 GOP 帧缓冲；内核的显示驱动起来后，
  面板扫的是内核自己的缓冲。v29g 报告称"console 是内核拥有的 GOP 缓冲、文字可能被内核内容覆盖"——
  也就是说**有可能看不见**。这是本版最大的不确定性；看不见时回落到宿主日志（若还能连）或
  串口/其它通道。P-C 的成本很低，值得先试。
- **每个访问多几个 store + 一条 `dmb ishst`**：轮询约 865 次/s，扫描时更多；实测代价应在
  0.1% CPU 量级。没有日志、没有锁、没有分配。
- **环只有 16 项**：并发访问超过 16 个在飞时，旧的待完成项会被保留、新项计入 `lost`
  （`lost` 非 0 说明环在"满载"，但停住的那项永远在）。实际 umi 路径由 `window_lock` 串行化。
- **翻窗多一次记录**：`window()` 里的 select 现在记录 `WINSEL`；`bus_write32` 的调试 printf
  在 `said_write()` 为真时仍会再翻一次窗（原有行为），会多一条 `S` 项——读环时以**最后**一条为准。
- **30 s 停留**：`WDT_BITE_MS` 也是 30 s，所以等待期间每秒 `wdt_pet()` 一次；umi 上 `wdt_armed`
  通常为 0，pet 是空操作。若将来 arm 了看门狗，这里的 pet 让它不会在取证中途咬。
- **`write_logs()` 在 `hc_take_back()` 之前**（USB 路径）：UMI 构建 `bio=NULL`、不碰 UFS；
  非 UMI 构建（QEMU 的 `xnuloader/loader.c`，不是本文件）不受影响。
- **ABI**：`loader_live` 只追加字段；loader 用 `magic3` 判定，老内核/老 loader 互不误读。
  loader 里 `struct lx_mmio_ring` 是同构副本，靠 `magic/size/version` 三道校验。
