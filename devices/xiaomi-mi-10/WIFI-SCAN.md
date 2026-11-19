# Unattended UMI scan

Append `wifi_scan=1` to the normal device boot args (`wifi=1` and the private
`wifi_mac=` are still required). Use the rebuilt kernel **and loader.efi**.
For the workspace supervisor's existing wrapper:

```sh
SECS=240 EXTRA="wifi_scan=1" /Users/xm666/X/umi-ios/boot-ios.sh
```

This runs one passive scan of the regulatory domain's enabled channels after
ath11k is ready and the station is up, before publishing the station callbacks
to iOS. It uses the existing bounded scan/completion/cancellation path (40 s
timeout), logs the BSS list even in quiet mode, and then hands the station to
iOS. It does not require tapping the Wi-Fi toggle. Without this argument the
normal startup remains the same. No preference or default boot-args change.

Expected log sequence:

```text
UMI Wi-Fi: boot passive scan begins (wifi_scan=1)
lx: THE DRIVER IS ASKED TO SCAN N channels, listening only: 0
lx: ... ms later the scan is OVER
lx: heard: channel ... (... MHz) ... dBm  ... "SSID"
lx: THE SCAN HEARD N NETWORKS (access points): ...
UMI Wi-Fi: boot passive scan returned 0; station handed to iOS
UMI Wi-Fi: station ready for iOS Wi-Fi requests
```

Zero BSS entries with `scan is OVER` verifies command/completion only; a nonzero
count verifies reception. A nonzero return or `NOT OVER`/`GIVEN UP` identifies
a scan failure. The scan does not test association, DHCP, or connectivity.

The generic `wifi_scan`/`wifi_join` diagnostics already existed, but UMI forced
`wifi_scan=0` and `wifi_station=1` during a normal session, and the station branch
bypassed the diagnostic scan/join branch. Generic `wifi_join=1` also needs the
private two-line `fw/network` file; UMI's QCA6390 packaging currently does not
install it (the generic copy rule is inside the WPSS firmware condition).
No credentials were supplied and this change does not enable auto-association.

`apple80211: wifid gets 19 (POWER), value 0, 24 bytes: answered` prints
`request->req_val`, not the returned power state. `drivers/wifi/hakios.apple80211.c`
returns POWER in a separate `{version, radios, state[4]}` structure. Therefore
that log line alone does not establish an OFF state; it does not justify
changing wifid's preferences.

Do not guess `AllowEnable`/`PowerEnabled` preference keys: this image has no
established wifid preference-seeding recipe. The diagnostic runs independently
of wifid's power requests. Hardware evidence before this change is
`logs/test-1108-230502-wifi-ring-charge.kernel.log`: PCI ID at L986, unified
ready at L1705, then radio/station registration. RPMh BOB/microphone changes
are preserved.
