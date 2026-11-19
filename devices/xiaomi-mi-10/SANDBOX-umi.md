# Sandbox stand-in: daemons and apps report sandboxed (2026-11-11)

## Symptom

An MT SMS reaches CommCenter and imagent, but never Messages. imagent spawns
`com.apple.MessagesBlastDoorService`, which dies about 50 ms later with SIGTRAP ("sent by exc handler").
Run by hand it prints `BlastDoor/Sandbox.swift:38: Fatal error: BlastDoor cannot run unsandboxed.` (rc 133).

## The check (20F66 dyld shared cache, BlastDoor.framework)

`sub_1a9bf9904` (the only xref to "BlastDoor cannot run unsandboxed."):

```
0x1a9bf9914  bl getpid
0x1a9bf9918  bl __bd_sandbox_check        ; 0x1a9bf9bac: x1 = 0, w2 = 0, b sandbox_check
0x1a9bf991c  cmp w0, #1
0x1a9bf9920  b.ne -> _assertionFailure("Fatal error", "BlastDoor cannot run unsandboxed.", Sandbox.swift:38); brk #1
0x1a9bf9924  bl __bd_sandbox_enable_state_flag   ; sandbox_enable_state_flag("blastdoor-post-launch", own audit token)
             ; on failure only logs "BlastDoor failed to enter sandbox state"
```

So the check is `sandbox_check(getpid(), NULL, SANDBOX_FILTER_NONE) == 1`. In libsystem_sandbox 1846.120.15 that is
`__mac_syscall("Sandbox", 2, &args)` (`_sandbox_check_common`), and the result is "the first 64 bits of the 40-byte
result block are non-zero". The argument block:

| off  | field |
|------|-------|
| 0x00 | user pointer to the result block (40 bytes, zeroed by libsandbox) |
| 0x08 | pid |
| 0x10 | operation name, NULL = "is it sandboxed at all?" |
| 0x18 / 0x20 | filter type / argument |
| 0x28 | SANDBOX_CHECK_* bits; 0x40000000 = named by audit token, 0x80000000 = by unique id |
| 0x30 | pid version (audit token) or unique id |

The entitlement that makes it true on a device is `seatbelt-profiles = [blastdoor-messages]`: Sandbox.kext applies that
builtin profile at exec. `sandbox_enable_state_flag` is request 0x41 `{pid, pidversion, name}`, returning true when the
syscall returns 0.

## What HakiOS did

There is no Sandbox.kext. `bsd/kern/kern_minimac.c` (device overlay) registers a stand-in "Sandbox" MAC policy. It answered
request 4 (container path) and returned 0 for everything else without writing anything. For request 2 that leaves the
result at 0, so every process was "unsandboxed". Hence no `Sandbox: deny` lines and installd's
`container_copy_sandbox_token returned NULL`.

## The fix (kernel only, general)

The stand-in now keeps a table of sandboxed processes, keyed by `p_uniqueid`. Fork and spawn give a new unique id; exec keeps it.

- **exec** (`mpo_proc_notify_exec_complete`):
  1. If the image's XML entitlements name a profile in `seatbelt-profiles`, the process is recorded with that profile.
     About 280 programs of 20F66 have one: imagent, CommCenter, cfprefsd, backboardd, mediaserverd, apsd and both
     BlastDoor services, among others.
  2. Otherwise (and only with `sbx=` other than 0 and 1), if the executable (`vn_getpath` of `p_textvp`) is
     `/private/var/containers/Bundle/Application/<UUID>/...` or `/var/containers/...`, the process is recorded with
     profile `container` (`minisandbox_app_path`, `minisandbox_mark_app`). A profile inherited from the launcher
     is replaced. Third-party apps now get `sandbox_check(getpid(), NULL, 0) == 1`, as on a device.
- **fork/posix_spawn** (`mpo_cred_label_associate_fork`): the child inherits the parent's sandbox. A sandbox also survives
  exec, as on a device.
- **sandbox_apply / sandbox_init\* / sandbox_init_from_pid** (requests 0, 1 and 0x1e): the call still succeeds, and the
  caller is now recorded.
- **exit** (`mpo_proc_notify_exit`): the record is dropped. Shadow procs from exec don't notify, so the record survives exec.
- **request 2 with operation NULL**: the target (pid, checked against the audit token's pid version or the unique id)
  gets `1` written into the result if it is recorded, and nothing otherwise. A vanished target gets `ESRCH`.
  **A non-NULL operation is still "allowed"**: nothing is enforced.
- **request 0x41** (state flag): `0` if the target is sandboxed, `EINVAL` if not.
- **request 5** (extension issue): writes a NUL-terminated token
  (`<64 zeros>;00;00000000;00000000;00000000;<len16>;<class>;<name>`). Before, libsystem_sandbox handed its caller the
  uninitialised `malloc(0x800)` buffer. More issuers now take that path because peers report "sandboxed".
- The table holds 1024 entries; when it is full, a line is logged once.
- **request 6** (`sandbox_extension_consume`, block `{token, strlen+1, &handle}`): any token is accepted and the
  handle is written as 1, 2, 3, ... Before, the call returned 0 without writing anything, so every consume gave handle 0.
- **request 0x1c** (`sandbox_reference_retain_by_audit_token`, block `{pid, pidversion, &ref}`): the reference is the
  target's unique id (`ESRCH` if it is gone). Before, the caller got libsandbox's initial -1.
  A request 2 with both flags 0xc0000000 (by reference) looks that unique id up in the table.
- **request 4** (container path): unchanged. The answer is `CFFIXED_USER_HOME` from the process's exec strings, else
  `ENOENT`. RunningBoard sets that variable for apps, so apps get their data container.
- Everything else (0x7 release, 0x8 update_file, 0x15 check_bulk, 0x16 builtin_query, 0x1d reference_release,
  0x43 get_container_expected, 0x3e/0x3f app bundle registration, 0xd/0x42 set_container_path, ...) still returns 0
  without writing anything. 0x43 then reads as "no container expected"; 0x16 reads as "builtin present".
- **Boot-arg `sbx=`**:
  - `0` = nobody sandboxed (every request but container_path gets 0, as before v1);
  - `1` = only daemons (seatbelt-profiles at exec);
  - absent or any other value = daemons + apps (container profile). This is the default.

  `sbx=0` is already in the loader whitelist. `sbx=1` was added to the same whitelist line in `loader/loader.c`,
  but **the loader has not been rebuilt**: `sbx=1` only takes effect after `bash devices/xiaomi-mi-10/flash/build.sh loader`.

Nothing in user space was patched, and there is no per-app special case: any program that asks the question gets the answer
a real iPhone would give.

Risk: the ~280 daemons that are sandboxed on a device now see themselves as sandboxed, and take their sandboxed code paths
(for example, issuing or consuming extensions). Those paths are the device's own. If one misbehaves, A/B test with `sbx=0`.
Third-party apps now also report sandboxed, which may change their behaviour (sandbox checks, extension requests). Test with `sbx=1`
to isolate app-specific issues.

Host test: `python3 -I devices/xiaomi-mi-10/tools/test-sandbox.py` covers the profile reader, the table, the token, the application path test, the log rate and the
block offsets. Add `MINISANDBOX_BINARY=…/MessagesBlastDoorService` to also read the real signature.

## What to grep on the phone (kernel log)

Where the lines are: since `kernel-sbx-apps2` every `minisandbox:` line also goes into the loader's live log ring
(`loader_log_line`, as the adreno/haptic lines do), so it shows up in the `KERNEL LOG (the kernel is still running)`
part of a USB-followed run. Before that, the lines went only through `printf()`. After the console handover, that output
reaches only the message buffer, which the loader appends only when the run returns to it (`--- kernel message buffer ---`).
No run since v8 (2026-11-10, `usb=1`) has had that section, so v27 (`kernel-sbx-apps`) showed no `minisandbox` line
although the code ran. The evidence is installd's `Consumed sandbox token 315`, a request-6 handle from the new counter.
The ring wraps on long runs (the `tl:` lines), so the earliest lines can be lost in a long session.

Good signs:
- `minisandbox: sandboxed ... by seatbelt-profiles at exec, profile ... (N since boot)` at N = 1, 2, 4, 8, ...
  (daemons, power-of-two logging).
- `minisandbox: sandboxed app <name>[PID], profile container (M apps since boot)` at M = 1, 2, 4, 8, ...
  (third-party apps, also power-of-two).
- `minisandbox: request 2 (first seen from ...): answered`
- `minisandbox: sandbox_check(self) by MessagesBlastDoo[PID]: sandboxed, profile blastdoor-messages`
  (p_comm is cut to 16 characters; this is the line that matters for BlastDoor).
- `minisandbox: sandbox_check(self) by <AppName>[PID]: sandboxed, profile container` (for a third-party app).
- `minisandbox: state flag "blastdoor-post-launch" of [PID] from MessagesBlastDoo[PID]: set`

Bad signs:
- `sandbox_check(self) by MessagesBlastDoo[...]: not sandboxed`
- `sandbox_check(self) by <AppName>[...]: not sandboxed` (app not marked, likely path mismatch)
- `minisandbox: table full` (daemon or `app ... not recorded as sandboxed`)
- launchd still showing `com.apple.MessagesBlastDoorService ... exited due to SIGTRAP`

After that, the MT SMS should reach the Messages UI. If not, the next wall is behind BlastDoor
(imagent ↔ BlastDoor XPC, IMDPersistence). For apps, check that launching one produces the first `sandboxed app` line.
