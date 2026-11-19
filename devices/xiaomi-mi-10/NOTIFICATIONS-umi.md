# Notifications on umi (SMS banner / lock screen / sound)

Status 2026-11-12: v29c proves that the SMS request is generated with sound and passes the client Screen Time
gate. Submission to SpringBoard is not observed. The post-generation submission decision was not instrumented;
the complete-chain diagnostic overlay now covers receipt, submission, policy, destinations, presentation and audio/haptics. No functional fix is established. No phone access.

## Symptom

In v28 an incoming SMS was stored, and the user saw it on opening Messages. There was no banner, no lock-screen
notification and no sound.

## Where notifications live on iOS 16 (correcting the first guesses)

- iOS has **no `usernotificationsd`**. That daemon exists only on macOS. On iOS the `UNUserNotificationCenter` server
  (UserNotificationsServer.framework) runs **inside SpringBoard**. Stock `com.apple.SpringBoard.plist` and our generated
  one (`out/.../build-20261111-172421/userland/root/LaunchDaemons/`) both carry all six
  `com.apple.usernotifications.*` Mach services (`usernotificationservice`, `usernotificationsettingsservice`,
  `remotenotificationservice`, `aps`, `aps.dev`, `delegate.com.apple.donotdisturb`). "Zero usernotificationsd lines"
  is therefore expected.
- The server in SpringBoard comes up and answers clients. At boot it prints `[DataStore] No data found at
  /var/mobile/Library/UserNotifications/<uuid>/...` and `[Migration]` lines. In v28 it read a bundle's store when
  Clock was opened (17:41:15) and when Maps was opened (17:44:52).
- **Who posts an SMS notification:** `IMDNotificationsController` (IMDPersistence.framework). It runs in
  `IMDPersistenceAgent`, an XPC service of IMDPersistence, which `imagent` starts. It holds
  `com.apple.private.usernotifications.bundle-identifiers = [com.apple.MobileSMS]`. `imagent` also owns
  `com.apple.usernotifications.delegate.com.apple.MobileSMS`, which is present in our imagent job.
- These launchd lines are **red herrings**. They show up on every boot and no stock job owns them either:
  - `failed activation: com.apple.usernotifications.delegate.com.apple.Bridge (CommCenter)`. This is the Watch app's
    delegate, and no iOS plist has it.
  - `failed lookup: com.apple.aps.usernotifications (apsd)`.
  - every `com.apple.private.alloy.*-idswake` failure, including `bulletinboard-idswake`, which is Watch bulletin mirroring.
  - `[Migration] [<bundle>] Error reading notification repository. Deleting file.` This prints for all ~70 bundles
    on every boot and is an error-level line from the legacy-store migration.
  - `failed to open fd at tempPath=/var/tmp/sectionInfoIcon...`. This is SpringBoard's icon cache and is cosmetic.

## Evidence

1. **No request ever reached SpringBoard.** SpringBoard runs with `OS_ACTIVITY_DT_MODE` (OSLOG list in
   `flash/prepare.py`). The UserNotificationsServer line `[%{public}@] Adding notification request %{public}@ to
   destinations: %{public}@` is logged at **default** level: `mov w2,#0` before `_os_log_impl` at 0x1f12a4b78 in the
   20F66 dylib. It appears **0 times** in every `logs/test-1110-*`/`test-1111-*` kernel log and in `logs/v28-snap/*`.
   So the client side never handed a notification to SpringBoard. This is not a presentation problem.
2. **The v28 SMS was not a clean test.** Times are from `logs/test-1111-172537-umi-v28.kernel.log`:
   - `17:37:19.712`/`.757`: touch down/up (backboardd).
   - `17:37:19.771`: `MobileSMS` spawned "because launch job demand". The user tapped the Messages icon.
   - `17:37:19.732`: CommCenter decodes the MT SMS PDU. The SMSC timestamp is 14 s earlier.
   - `17:37:19.789`: CommCenter `BundleID: com.apple.MobileSMS is a foreground app`.
   - `17:37:19.844`: imagent `kSmsRetrieveMessage`.
   - `17:37:19.884`: MessagesBlastDoorService spawned and sandboxed. It works now.
   - `17:37:20.93`: `failed lookup: com.apple.dmd.policy` **and** `com.apple.dmd.emergency-mode`, requestor
     **IMDPersistenceAgent[137]**. MobileSMS asks for the same two at `17:37:20.39`.
   - After that there is no SpringBoard UserNotifications line at all.

   Messages was the foreground app when the SMS was processed, and on a real iPhone that alone shows no banner.
   The earlier SMS tests (v25 13:06, v26 14:05) ran while BlastDoor still crashed (SIGTRAP). **No run so far has
   had a working BlastDoor and Messages in the background at the same time.**
3. **The v28 Screen Time suppression hypothesis (not directly observed):**
   - IMDPersistence (20F66) runs `[[STConversation alloc] initSynchronouslyWithBundleIdentifier:]`, then
     `allowableByContactsHandles:`, then `screenTimeNotificationOptionsForContext:`. The selectors are resolved through
     the stub selrefs in libobjc's `__OBJC_RO`.
   - The options function (sub at 0x1a8a71fb8) returns **4 when `allowedByScreenTime` is NO**, and a nil context also
     gives NO. Option bit 2 leads to `Not notifying for %@ because it is not allowed by Screen Time`
     (0x1a8a73cd8). That line is info level, which is why nothing is visible.
   - ScreenTimeCore computes `allowedByScreenTime` in
     `updateShouldBeAllowedDuringGeneralScreenTime:...currentApplicationState:emergencyModeEnabled:` from DMF:
     `communicationPoliciesWithError:` / `requestPoliciesByBundleIdentifier...` (dmd `com.apple.dmd.policy`) and
     `emergencyModeStatusWithError:` (`com.apple.dmd.emergency-mode`). It has the strings "Could not fetch
     communication policies" and "Could not fetch emergency mode status".
   - **dmd has been in mkjobs.py DENY since the first commit** ("these start, crash and are asked for again"). That
     note was never measured on umi. So both lookups fail, the context stays "not allowed", and every Messages
     notification is dropped silently. SpringBoard pays for the same missing daemon at boot (`[Policy] Failed to
     register application policy monitor`).
4. **Secondary issue:** ScreenTimeCore cannot open its RemoteManagement store. ScreenTimeAgent logs
   `CoreData: XPC: Can't create coordinator ... Failed to create file; code = 2` 8 times, because
   `/var/mobile/Library/Application Support/com.apple.remotemanagementd/` does not exist. On an iPhone,
   remotemanagementd creates it from its `on-reboot` xpc activity, but `--rest` strips LaunchEvents, so it never
   runs here. dmd soft-links ScreenTimeCore and would hit the same wall.

Other v28 observations:
- keybag first unlock: CommCenter reports `DeviceFirstUnlock[1]`.
- a clock in the future: no log year or date ahead of real time.
- Focus/DND: donotdisturbd is shipped and quiet; this does **not** establish that Focus is off.
- the UN server's Mach services or entitlements: present.

## Previous fix (built into v29)

| File | Change |
|---|---|
| `overlays/userland/mkjobs.py` | `com.apple.dmd` is removed from DENY. It ships **on demand** like every `--rest` job: no RunAtLoad, KeepAlive or LaunchEvents, and the stock `ThrottleInterval 10` is kept. **Kill switch:** `DMD=0` in the build environment puts it back into DENY, the same as `LOCATIOND=0`. No device-tree `KILL_SWITCH` is used, because that needs a loader boot-arg. |
| `overlays/userland/build.sh` | A var.skel line `d 755 501 501 /private/var/mobile/Library/Application Support/com.apple.remotemanagementd`. init mkdirs var.skel directories on every boot, kept `/var` included, so no data reset is needed. |

Host check: `mkjobs.py --rest --root-volume sysimage/rootfs.slt sysimage/rootfs /tmp/x com.apple.SpringBoard com.apple.imagent`
now emits `com.apple.dmd.plist`:
- Label `com.apple.dmd`.
- `/usr/libexec/dmd`, which is on the iOS volume.
- MachServices `com.apple.dmd`, `.policy`, `.emergency-mode`.
- UserName mobile, ThrottleInterval 10, stderr to the console.
- `plutil -lint` OK.

With `DMD=0` no dmd job is emitted. `bash -n build.sh` OK. CommCenter, the loader and the statusbar/location/Wi-Fi
files are untouched.

Risks:
- dmd crash loop. Being on demand bounds it to one spawn per 10 s while something asks. Watch `com.apple.dmd ...
  service has crashed N times`.
- Wrong policies from dmd. SpringBoard could grey out apps as "blocked for Downtime", which would show as
  `app ... is now blocked for Downtime: 1`.
- A slow dmd behind the synchronous `initSynchronouslyWithBundleIdentifier:` would delay Messages delivery.

Undo any of these with `DMD=0` (rebuild), no data reset needed. The new directory is harmless. To remove it, drop
the line and delete the directory.

Build with `OSLOG="<the prepare.py default list> com.apple.dmd com.apple.ScreenTimeAgent"` to see the two daemons'
own logs. IMDPersistenceAgent cannot get DT mode: it is an XPC service on Apple's sealed volume.

## What to check on the phone

Good log lines:
- `Successfully spawned dmd[N] because ipc (mach)`. This should come at boot, from SpringBoard's policy monitor, or
  at the first SMS.
- No more `failed lookup: name = com.apple.dmd.policy` / `com.apple.dmd.emergency-mode`.
- No `CoreData ... RMAdminStore-Local.sqlite` errors.
- **`SpringBoard ... [com.apple.MobileSMS] Adding notification request <id> to destinations: ...`** and
  `Adding notification ... [ hasAlertContent: 1, shouldPresentAlert: 1 hasSound: 1 shouldPlaySound: 1 ]`.

Bad log lines:
- dmd `exited due to SIG...` / `crashed N times in a row`.
- `is now blocked for Downtime: 1`.
- SMS present in Messages but still no `Adding notification request` line. In that case the STConversation gate is
  still closed: look at the dmd/ScreenTimeAgent OSLOG output.

Tests (keep Messages **out of the foreground**):
1. Screen locked and off: send an SMS from another phone. Expect wake, a lock-screen notification, and sound/vibration.
2. Unlocked, on the home screen or in another app (not Messages): send an SMS. Expect a banner and sound.
3. App notification: Clock, set a 10 s timer, go to the home screen. Expect the timer banner and sound, and a
   SpringBoard `[com.apple.mobiletimer] Adding notification ...` line. This exercises SpringBoard's pipeline without
   Screen Time. Note that SpringBoard already logged `MTTimerServer Code=1` / `MTAlarmServer Code=1` at boot. If the
   SMS works and the timer does not, the problem is in mobiletimerd and is separate.
4. Settings: Notifications > Messages should be Allow on with Lock Screen, Notification Center, Banners and Sounds on.
   Screen Time should be off, and Focus off in Control Center.

## v29 — 11-12, SMS around 09:05

**Cause status:** not established for either banner or sound. In
`out/xiaomi-mi-10/runtime/logs/085506-run.log`, dmd starts at `08:55:31.626470` with
`Successfully spawned dmd[93] because ipc (mach)`; its policy/emergency-mode lookups no longer fail.
The prior fix is present: init creates 113 directories successfully, including the RemoteManagement directory.
Nevertheless line 20833 reports `RMAdminStore-Cloud.sqlite ... SQLite error code:6922`, and line 20907 reports
the same for `RMAdminStore-Local.sqlite`. 6922 is **SQLITE_IOERR_VNODE** (Apple SQLite invalidated an open
descriptor), not an ordinary missing-directory error. At `08:55:57.996`, ScreenTimeAgent reports
`Can't create coordinator ... Failed to create file; code = 2`; the preceding stat failure names the
RemoteManagement directory. The log does not identify who removed/replaced it or the vnode event.
Do not disable SQLite vnode guards based on this evidence.

In the requested `09:04:30–09:06:30` window there is no logged UN request insertion or SMS sound playback.
Messages first launches at `09:07:46`, so the SMS was received in the background.
The `09:06:25.373879` `NotificationDeviceWake` contains **`changeSource: LockButton`**.
It does not prove an SMS alert. Current BBSectionInfo, Focus and ringer state are absent from this log.
The stock Messages defaults allow alerts/sound; they do not reveal persisted user settings.
SpringBoard has the UN/BB services, and mediaserverd has `com.apple.audio.SystemSoundServer-iOS`.
The HAL accepting media samples does not test the system-alert route.

**Change built:** `src/inject_notifications.m` records the original results at the IMDPersistenceAgent,
SpringBoard and ScreenTimeAgent gates. `build.sh` stages it and inserts it into IMDPersistenceAgent,
ScreenTimeAgent and dmd; SpringBoard opens it from its existing injection constructor.
It also traces NSFileManager removal of this one store tree (PCs only). It does not change notification
policy, stored settings, sound routing, or SQLite behavior. **This is a diagnostic build, not a verified fix.**
Only fixed labels, booleans, enum/error values and stack addresses are logged; never request descriptions,
identifiers, sender/recipient handles or message text. No additional Apple IM logs are enabled.

Next boot: look for `hakios: notify:` with these exact substrings (PID/uptime prefix omitted):

| Line substring | Meaning |
|---|---|
| `init process=SpringBoard`, `init process=IMDPersistenceAgent`, `init process=ScreenTimeAgent` | Diagnostics loaded; any `hook-missing` or `diagnostics load failed` must be reported. |
| `store stage=after-vault exists=1 errno=0`, `st-store-loaded error=0` | ScreenTimeAgent created/opened its store. `store-remove` plus `frame=... pc=...` identifies a remover in an instrumented process. |
| `imd-post`, `settings present=1 auth=2 alert=2 style=1 lock=2 sound=2 scheduled=1` | SMS posting started and normal immediate banner/sound settings were returned. Style 2 is also visible; scheduled=2 means scheduled delivery. |
| `imd-eligible=1`, `st enabled=... context=1 allowed=1 limited=0 options=0` | Native client gates allow posting. `options=4` directly proves Screen Time suppression; a missing `st` line means it has not reached that gate. |
| `imd-generated=1 active=1 sound=1`, `server-add sound=1`, `server-add-complete error=0` | Request generated and accepted by the UN server. |
| `bb-section effective=1 present=1 allows=1` | Effective Messages BBSectionInfo; capture `alertType`, `lock`, `push` on this same line. |
| `focus present=1 active=0 suppress=0 error=0`, `ringer status=0 on=1 volumeValid=1 volume=...` | State queried 20 s after SpringBoard starts and at server insertion. Zero alert volume, on=0, or suppress=1 explains sound/presentation policy; a failed query is logged explicitly. |
| `banner-canReceive=1`, `sound-canPlay=1 suppress=0 hasSound=1`, `sound-play` | Native banner and sound decisions. Focus/suppression remains relevant if `suppress=1`. |
| `tone-systemsound type=...` then `hakios: audio: playing` / `hakios: audio: stopped after` | Native ToneLibrary system-sound path was invoked; compare HAL activity at this moment. Media playback alone is insufficient. |

One-time supervisor step: record Messages notification settings, Focus state, Screen Time state and the
Sounds & Haptics alert volume/mute state before changing anything. Choose a non-None text tone and preview
it in Settings to test the native alert audio path. Then receive one SMS on the unlocked home screen with
Messages in the background, and one while locked. No database deletion or data reset is justified yet.

Host validation: `bash work/umi-v23/devices/xiaomi-mi-10/flash/build.sh userland` succeeded;
snapshot `out/xiaomi-mi-10/flash/build-20261112-094015`. The new dylib builds with `-Wall -Wextra -Werror`.
`nm -u -m` has no dynamic-lookup symbols in `libhakios.notif.dylib`; SpringBoard gains no non-weak
dynamic-lookup symbols. Default `inject.conf` is 482 bytes (kernel limit: 512).
## v29c — 11-12, SMS at 12:33:07

Read the complete `logs/v29-snap/v29c-1235.log` notification diagnostics (41 lines), including the
12:32:30–12:34:00 window. Last proven SMS stage: **IMDPersistenceAgent generated a non-nil request with
sound; there is no observed call to the instrumented SpringBoard server insertion method.** The log
cannot identify a unique dropping gate after generation. Do not describe this as a confirmed BB,
Focus, Screen Time, missing destination or audio failure.

| Evidence (log line numbers) | Conclusion |
|---|---|
| 95041 `imd-post`; 95043 `settings present=1 auth=2 alert=2 style=1 lock=2 sound=2 scheduled=1` | Messages is authorized for immediate banner, lock-screen and sound delivery. |
| 95042/95044 `bb-section effective=1 present=1 allows=1 alertType=1 lock=2 push=63` | Effective BB section settings permit notifications. |
| 95124 `st enabled=0 context=1 allowed=1 limited=0 options=1`; 95134 `imd-eligible=1` | Native Screen Time and chat eligibility gates pass. `options=1` has no rejection bit (`4`). |
| 95156 `imd-generated=1 active=1 sound=1` | Request generation succeeds. Here `active` is the method's `isMostActive` device argument, not Messages foreground state. |
| No `server-add`, `server-add-complete`, `banner-canReceive`, `sound-canPlay`, `sound-play` or `tone-systemsound` | Those downstream probes do not establish a presentation/sound rejection. There is no server insertion evidence. |
| 37502 `focus present=0 active=0 suppress=0 error=3` | Focus query failed; the zero fields do **not** prove Focus is off. |
| 37601 `ringer status=0 on=1 volumeValid=1 volume=0.500` | Ringer was on at the startup sample; this does not exercise SMS alert playback. |
| 19546 `st-store-loaded error=0`, then 19931 `error=6922`, 19932 directory `exists=0 errno=2`; 95125 still absent | RemoteManagement store failure persists, but this SMS's Screen Time result is allowed. No recorded `store-remove` identifies its remover. |

**Uncovered branch, verified in the stock 20F66 cache:**

- `_registerUserNotificationsForMessageRecords:...` generates the request, then calls
  `_shouldPostNotificationRequest:withCompletionHandler:` at `0x1a8a65d48`.
- That method checks `_haveMigrated` at `0x1a8a71444`. If false, it calls its completion with NO
  (`0x1a8a714bc`) and never submits the request. If true, it asynchronously calls
  `getDeliveredNotificationsWithCompletionHandler:` and checks whether to repost before returning its decision.
- `_haveMigrated` at `0x1a8a71410` calls `IMDSystemMigrationOccurred`. That function checks
  `DMIsMigrationNeeded` and compares `kIMDPersistenceLastBuild` in `com.apple.imdsmsrecordstore` with
  the current OS build. A missing or stale build record is a possible rejection, **not measured by v29c**.
- datamigrator starts again at 12:33:07.225 and exits normally at 12:33:18.438. Its start alone does not
  give the migration-check result. At 12:33:07.231 SpringBoard reads a missing `DeliveredNotifications.plist`;
  this does not record completion of the client's delivered-notifications query or its final submission decision.

**v29c complete-chain diagnostic overlay (one next boot):**

`overlays/userland/src/inject_notifications.m` now covers the complete notification chain below.
All hooks call the native implementation and preserve its return value. No migration result, Screen Time,
Focus, BB setting, destination registration, ringer setting or sound policy is forced. The native
`NSError` completion for client/server insertion and BOOL completion for submission are forwarded;
opaque private completion blocks are forwarded unchanged. Tone completion-context hooks report their
native completion type/error without guessing those blocks' ABI.

| Boundary | Diagnostics / values |
|---|---|
| imagent receipt and persistence | `imagent-received`, `im-message-store stored=...`; neither message nor chat is inspected or described. |
| IMDPersistenceAgent batching and generation | `imd-post`, `imd-register-batch`, `imd-eligible`, `imd-generated present=... mostActive=... sound=...`; DND-chat, unknown-sender filter, category mute, silent business chat, retraction, FaceTime highlight and first-unlock native decisions. |
| Migration and repost | `imd-submit-check`, `imd-migration ready=... markerPresent=... markerMatches20F66=...`, `imd-delivered-query/complete`, `_shouldRepostNotification:withAlreadyPostedNotification:`, `imd-submit-decision post=...`. The build marker is read only. |
| UN client and SpringBoard server | `imd-client-add` includes the numeric UN destination bitmask; client/server completion error codes; `server-add`, `server-resolved`, `server-save-request`, `server-save-record`, `server-repository-save/observers`, `server-delivered-query`. Record policy logs `hasAlert`, `presentAlert`, `hasSound`, `playSound`. iOS has no separate usernotificationsd job. |
| BB publication/settings | `publishBulletin:destinations:`, `publishBulletinRequest:destinations:`, `_publishBulletinRequest:forSectionID:forDestinations:` and `bb-section-effective/raw`; numeric destinations, notification permissions, alert/lock settings, sound presence, interruption level and active-app suppression. |
| Screen Time / RemoteManagement | `st`, `st-initial-state`, `st-contact-context`, `st-context-update`; initial policy/emergency/application fetch flags, current application state, contact-context allowance, downtime/general/limited allowances; DMF policy/emergency fetch presence and errors. ScreenTimeAgent store load/destroy decisions, SQLite error codes, store removal stack PCs and before/after state. |
| Store disappearance | `store-vnode` watches the fixed parent directory, reporting root existence, errno, inode, uid/gid/mode only on changes. This observes deletion/replacement even if it bypasses NSFileManager. NSFileManager removal hooks in the injected processes identify native callers; the vnode event itself does not identify a process. No directory is recreated by these probes. |
| Focus | `focus-native-query/update` records the native DND state and update reason; `focus-event-decision` records the actual per-event interruption suppression, resolution reason and error. `focus-sample` is a separate startup/insertion/playback sample; `present=0 error=3` remains an unsuccessful query. |
| SpringBoard destination setup | `sb-destinations` shows banner, lock-screen and notification-center destination object classes/presence. Dispatcher and alert-queue registration/unregistration, enabled/ready changes, `destination-ready`, registry registration, active/ready membership and resolution counts/classes are logged. This distinguishes a nil destination from an unregistered, disabled or unready destination. |
| Dispatch and actual presentation | `postNotificationWithRequest:`, `_shouldPostNotificationRequest:`, alert-queue post/readiness decisions, `nc-options` suppression/sound and requested destination counts; banner locked/setup/authentication/content/Screen Time gates; `postNotificationRequest:`, `destination:willPresentNotificationRequest:suppressAlerts:`, `destination:didPresentNotificationRequest:`, `_publishDidPresentBannerForNotificationRequest:`, master/structured list insertion, `banner-refused`. Refusal reason is presence/class only, never its description. List insertion alone is not proof that a row was visible. |
| SpringBoard sound gate | `sound-canPlay`, `playSoundIfPossibleForNotificationRequest:presentingDestination:`, `sound-play`, `stopSoundForNotificationRequest:` and lock state; ringer state plus validity/value of native Ringtone volume samples. |
| ToneLibrary / haptics | `tone-systemsound` reports type, vibration presence, custom volume and native ringer/accessibility flags; `tone-components`, `tone-vibration`, `tone-task`, loading/started/interrupted transitions, `tone-remove prepareFailed=...`, native playback completion type/error and completed-alert callbacks. No tone names, URLs, pattern contents or identifiers are printed. |
| AudioToolbox | `audio-create status=... soundID=...`, `audio-play event=...` with vibration presence, behavior/flags/loop/volume/ringer options, `audio-stop` and `audio-complete`. Existing nil completions stay nil because adding a completion changes the native server token path. Public play/start/vibration entry points converge on the probed `AudioServicesPlaySystemSoundWithOptions` in 20F66. |
| SystemSoundServer in mediaserverd | `systemsound-peer`, `systemsound-xpc`: filtered exclusively to `com.apple.audio.SystemSoundServer-iOS` listener/clients and accepted peers. Phase 0 = incoming event; 1 = send; 2 = async reply; 3 = synchronous reply. Numeric action/actionID/completion token/activation state, audio/vibration/options presence, scalar replies and connection errors are recorded. Error 1 = interrupted, 2 = invalid, 3 = termination imminent, 4 = other. Fixed protocol preference fields `p0..p5` are volume, alert-audio-on, UI-audio-on, alert-visual-on, should-complete-playback, stop-immediately; absence/type is explicit. This proves service handling and replies; hardware audibility must be correlated with the existing audio HAL playback logs. |

**Coverage and privacy:** each ObjC hook validates its exact 20F66 method encoding before installation.
`hook-installed`, `hook-type-mismatch` and the 20-second `hook-unavailable` / `hook-audit-complete` records
make missing instrumentation visible. `objc_addLoadImageFunc` installs pending hooks after the runtime
registers a late framework and before its `+load`; the loader callback uses runtime/C APIs only.
All 105 unique ObjC hook encodings were checked against the raw cache, Foundation and ScreenTimeAgent
executable metadata. AudioToolbox/libxpc calls inside the shared cache bypass ordinary dyld interposition,
so seven C entry hooks validate the exact non-PC-relative 20F66 arm64 prologue and use an RX trampoline
with private COW text. Installation/protection failures are logged explicitly.

There is **no total-count logging budget**. State/decision records are deduplicated per observed object and
stage, and XPC records per peer/direction/state; new request/batch/playback operations and their callbacks
remain visible. `obj=...` and audio `event=...` are anonymous local ordinals, not message/request identifiers;
PIDs and monotonic timestamps join the stages. Only fixed labels/classes, booleans, counts, numeric
enums/errors/audio handles, inode metadata and stack PCs can be emitted. No message content, sender,
phone number, contact handle, original request ID, object description, XPC string/data, tone filename or
vibration payload is logged. Apple IM verbose logging is not enabled.

`overlays/userland/build.sh` adds imagent, datamigrator, remotemanagementd, donotdisturbd and mediaserverd
to the notification library's injection line. SpringBoard still opens it through its existing notification
constructor. To respect the kernel's one-library-per-process insertion, mediaserverd's notification
constructor opens the existing `libhakios.media.dylib` after installing its probes. Its source and existing
initialization are preserved. The default Bluetooth-enabled `inject.conf` is **500 bytes**, below the
512-byte kernel limit. No daemon is synthesized and CommCenter is not injected or modified.

**Next boot:** capture the complete `hakios: notify:` output from boot, then receive one SMS with Messages
in the background on the home screen and one while locked. Include `init`, hook installation/audit,
destination changes and the full receipt-to-completion timeline. `ready=0` at migration followed by
`post=0`, a nonzero client/server completion error, a specific native suppression decision, empty destination
resolution, or Tone/audio/XPC failure then identifies the failing boundary in that same boot. Do not use
absence of an uninstalled hook as proof of a dropped request.

**Root-cause status:** the supplied 12:33 log does not measure the migration/repost decision or client
insertion callback. It cannot justify a functional preference/job/injection override. Its BB section settings
and Screen Time gate allow this SMS; the failed Focus sample and disappearing store do not establish an
alert rejection. No one-time SSH directory/preference command is justified. No phone commands were run.

Host validation: `bash work/umi-v23/devices/xiaomi-mi-10/flash/build.sh userland` was run directly, without
an additional build-lock wrapper. Final snapshot: `out/xiaomi-mi-10/flash/build-20261112-132639`.
Compilation passes `-Wall -Wextra -Werror`; `nm -u -m` on `libhakios.notif.dylib`, `libhakios.sb.dylib`
and `libhakios.media.dylib` finds **zero dynamically looked-up symbols**, so no new non-weak lookup symbols.
The staged notification source and dylib match the overlay/compiler output; staged `inject.conf` is 500 bytes.
All 105 ObjC method encodings and all seven C hook prologues pass the host audit against stock 20F66.
These are build/static checks; the new probes have not been run on the phone.
Only the notification injection source, notification injection entries in build.sh and this document were
edited for this investigation. Concurrent signal code in inject_sb.c, media source, CommCenter, kernel and
hakios-metal were not edited.
