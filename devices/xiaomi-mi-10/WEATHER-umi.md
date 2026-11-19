# Weather: city resolves, data never loads (v28) — diagnosis

Status 2026-11-11. Log used: logs/v28-snap/v28-1735.log (snapshot during run v28).

## Verdict

**Blocked by Apple request signing (FairPlay "Mescal"). Not fixable in the port with a small change.**
Networking is not the cause: weatherd is launched, cellular works, and the app does contact the network.
The weather data request needs a JWT, and the JWT request must be signed through FairPlay SAP. On this
hardware that signature can't be produced.

## How iOS 16.5 Weather authenticates

- Weather.app (staged_system_apps) links WeatherDaemon.framework and runs its auth code inside the app process.
- `WDSAuthenticatorFactory.makeAuthenticator(for:)` uses API version 2 and reads the TeaSettings flag
  `weatherdaemon.enableJWTTokenAuth` (default **true**, access `.protected`, so it can't be changed on a customer build).
- With JWT on, the factory builds `WDSJWTAuthenticatorService` with `WDSJWTMescalSigner`
  (or the `com.apple.weatherkit.authservice` proxy, which ends in the same signer).
- `WDSJWTMescalSigner.sign(data:)` calls `+[AMSMescal signatureFromData:type:bag:error:]` (AppleMediaServices).
  Mescal is FairPlay SAP signing, and AMS performs it by talking to `com.apple.fairplayd[.versioned]`.
- With no signature there is no JWT. Without a JWT, weather-data.apple.com returns no data, so the UI never fills.

Checked with ipsw disassembly of the 20F66 dyld shared cache:
WeatherDaemon `makeAuthenticator(for:version:isJWTEnabled:)` @0x1a4acb104, signer selection @0x1a4acb51c,
`WDSJWTMescalSigner.sign` @0x1a4b7fbd0 (calls AMSMescal), setting initialiser @0x1a4b7df78.

## Evidence in the v28 log

| What | Lines / count |
|---|---|
| Weather[294] failed lookups of `com.apple.fairplayd` / `.versioned` | 127 / 128 between 17:28 and 17:29 (repeated signing retries; other AMS clients try only about 2 times) |
| weatherd launched (on mach IPC from the app) | 17:28:06, pid 334, "sandboxed, profile temporary-sandbox". So the "weatherd never launched" lead from v26 no longer applies |
| `com.apple.weatherkit.authservice` | stub created only, never spawned (the app signs in-process) |
| CommCenter `[dafEvent] err_code=93 (kDADMissing) ... frontmost app=com.apple.weather; uses network=0` | 17:28:12. A data-activity heuristic: the app sent almost nothing because it is stuck before the data request. A symptom, not a cause |
| `Adding trustd/com.apple.weather to the cache` | 17:28:08. TLS did happen (AMS bag or cert checks), so the network path works |
| runningboardd `Failed to get LSApplicationRecord` for the weather.widget/poster extensions | Cosmetic. The extensions still spawn and check in |
| `failed lookup com.apple.kvsd`, `com.apple.bird` | iCloud KVS and iCloud Drive. Not needed to fetch weather |
| HakiosMetal `147 compiled (147 failed)` in Weather | A rendering issue, noted only. Unrelated to data |

## Why re-enabling fairplayd doesn't help (and is harmful)

- fairplayd is DENY-listed (`'com.apple.fairplayd.'` in overlays/userland/mkjobs.py and userland/mkjobs.py). Reason (NIGHT-LOG 11-10 19:00, USB-umi.md):
  fairplayd.H2 exits at once because this kernel has no `com_apple_driver_FairPlayIOKitUserClient`. Its registered Mach service becomes a
  black hole: lockdownd hangs for ever in `create_cesm_context`, and AMS clients block in `_AMSFairPlayGetDeviceType`.
- Even a kernel stub that keeps fairplayd alive (APPSTORE-umi.md, "Problem 2") can't produce **valid** SAP/Mescal signatures.
  Those need Apple's per-device FairPlay secrets bound to Apple hardware. Apple's server would reject the token request.
- So: keep the DENY. Re-enabling would bring back the lockdownd hang and still give no weather data.

## Options

1. Accept it as a known limitation, like the other Apple-account and FairPlay services (README: "Apple services are not a goal").
2. Weather from a third-party app that uses an open API (e.g. Open-Meteo) works over our network stack with no Apple signing.
   This is the practical route if the user wants weather.
3. Not pursued: changing Apple's signing or authentication path. Out of scope.

## What to check on the phone (optional, to confirm)

- Open Weather with Wi-Fi off and then on: the number of `failed lookup: name = com.apple.fairplayd` lines from `Weather[...]` keeps growing while the city shows with no data.
- If os_log for Weather/weatherd is captured (`log stream --predicate 'subsystem BEGINSWITH "com.apple.weather"'`), expect
  "Failed to generate jwt token" / "Error while getting new JWT token" / AMS "Failed to mescal sign".
- Safari still loads pages, which confirms the network itself is fine.

No code changed for this issue.
