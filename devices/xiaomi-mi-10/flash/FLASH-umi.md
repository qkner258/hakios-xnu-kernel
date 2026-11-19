# umi flash root (HakiOS 2.3.4)

Prepared host-side on 2026-11-09. **No adb, fastboot or phone action was executed.**
Artifacts: `out/xiaomi-mi-10/flash/`; exact hashes/sizes: `manifest.json`, `SHA256SUMS`.
The existing RAM images, loader and forward-port sources were not replaced.

## Bytes on darwin

LUN0 `/dev/block/sda`: 30,945,280 logical blocks, **4096 bytes each**. GPT index **34**
(zero-based) is Linux **sda35**, `/dev/block/by-name/darwin`, LBA **14167552..30945274**,
16,777,723 blocks / 68,721,553,408 bytes. Type `7c3457ef-0000-11aa-aa11-00306543ecac`;
unique GUID `d8795ba0-8d83-4cd0-8d8b-03186f8b422f`. No inner GPT is written.

| File in out/xiaomi-mi-10/flash | darwin 4K block | Blocks | Bytes | Byte offset in darwin |
|---|---:|---:|---:|---:|
| `rootfs.apfs` (whole Apple system container) | 0 | 1374208 | 5628755968 | 0 |
| `cryptex-os.apfs` (whole OS/dyld container) | 1374208 | 665600 | 2726297600 | 5628755968 |
| `cryptex-app.apfs` (whole App container) | 2039808 | 2560 | 10485760 | 8355053568 |
| HFSX Data, initialized by `data.simg` | 2042368 | 14735355 | 60356014080 | 8365539328 |

Data ends at darwin's end (physical LBA **16209920..30945274**): **56.211 GiB**.
`data.simg` is a 53,492-byte Android sparse initialization image describing the whole
partition, with DONT_CARE over the system area/free data blocks. The writer writes
only its 38,085 allocated data blocks (155,996,160 bytes), including zero FILLs.
**Writing it again resets the data filesystem.** It is not a raw image for dd.

`rootfs.apfs` is the APFS partition extracted from IPSW member `096-04088-077.dmg`
(`sysimage/rootfs.dmg`, UDIF byte offset 17408). The other IPSW members,
`096-04666-078.dmg` and `096-03536-083.dmg`, are already raw APFS; copied intact.
IPSW: `iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`. Neither `system.apfs` nor
the small userland `root.apfs` belongs at darwin block zero.

RAM only: `flashboot.apfs` is **34,451,456 bytes** (8411 blocks), stage0 + firmware
and mount points; OS/App cryptexes stay on UFS. The payload beside the kernel uses
`root-flash-data-test.apfs` (19,173,376 bytes) or `root-flash-ui.apfs` (same size).
`root-ram-data-test.apfs` is 14,958,592 bytes, for the ordinary resident RAM system.

## Build and lookup

```sh
source /Users/xm666/X/umi-ios/env.sh
cd /Users/xm666/X/umi-ios/work/umi-v23
# Already built. Rebuild only through this wrapper (uses build-lock.sh):
bash devices/xiaomi-mi-10/flash/build.sh         # or images / loader / userland
python3 devices/xiaomi-mi-10/flash/verify.py
```

The build snapshots umi userland into its own `flash/build-*`, sets
`FLASHBOOT=1 CRYPTEXES=flash DEVICE=umi GL=1 BLUETOOTH=1 REST=1`, and stops the
private recipe before the trimmed system-image section. Its two flash adapters
keep Mesa in the payload (no dangling link into `system.apfs`) and bind the RAM
Metal bundle onto a synthetic directory at Apple's plugin path (`flash-init.patch`).
The Apple volume stays unchanged. Kernel + payload fits the loader's 63 MiB limit.
It also builds the RAM test without FLASHBOOT. Mesa/Metal are reused.
The image extractor needs `lzfse==0.4.2.post1` (installed in the workspace venv).
Data uses upstream `tools/mkdatavol.py` with `DARWIN_BLOCKS=16777723`,
`DATA_FIRST=2042368`, `-s -J -b 4096 -U 0 -G 0 -M 755 -v Data`. `build-hfs.py`
builds a SHA256-pinned Apple HFS formatter adapted to fresh regular files only;
no host disk attachment is needed. Dependency sources/license are retained in
`flash/hfs-format/upstream` (APSL-2.0).

Loader `block_start()` enumerates UEFI GPT **names**; kernel `hakios.ufs.c` publishes
`/dev/ufs_NAME`, so read lookup is independent of Android's partition number.
Write permission additionally checks type/GUID/LBAs/size and UEFI **unit index 0**.
The ordinary `out/xiaomi-mi-10/darwin_identity.h` still had the upstream phone's
identity. **Use `flash/loader.efi`**, built from `umi.darwin`, for these tests.
Both loader `BLOCK_WRITE_FIRST` and kernel `UFS_WRITE_FIRST` already equal 2042368;
`ufs_darwin_data` maps precisely that block through the partition's end and exists
only with `ufs_write=1`. The generated umi kernel includes UFS, APFS and HFS.
No kernel offset change is needed. UEFI unit enumeration on this phone remains a
runtime check: require darwin on unit 0 and the correct LBA/size in the first log;
a different unit index refuses writes. Keep `ufs_native=0` for these tests.

## Supervisor: write from Android

All phone commands below are for the supervisor. `check`/`plan`/`sum` never write
partition bytes; they use temporary files under `/data/local/tmp`. `write` requires
`DARWIN_WRITE_UMI=yes`. Only `/dev/block/by-name/darwin` can be the dd destination.
Every chunk rechecks the live GPT (both CRCs, entry/GUID/LBAs, overlap), UFS LUN0,
sysfs start/size/name/partition and blockdev geometry. Transfers use 16 MiB
push + remote SHA256 + bounded dd/fsync + readback file/pull + both SHA256s.
There is **no binary exec-out su stream**. Logs: `flash/writer.jsonl`.

```sh
SERIAL='<Android adb serial>'
F="$PWD/out/xiaomi-mi-10/flash"
dw() { python3 devices/xiaomi-mi-10/flash/darwin-write-umi.py --serial "$SERIAL" "$@"; }
dw check
dw plan "$F/rootfs.apfs" 0
dw plan "$F/cryptex-os.apfs" 1374208
dw plan "$F/cryptex-app.apfs" 2039808
dw plan "$F/data.simg" 0
# Execute individually, proceeding only after each succeeds:
DARWIN_WRITE_UMI=yes dw write "$F/rootfs.apfs" 0
DARWIN_WRITE_UMI=yes dw write "$F/cryptex-os.apfs" 1374208
DARWIN_WRITE_UMI=yes dw write "$F/cryptex-app.apfs" 2039808
dw sum 0 1374208                 # compare rootfs.apfs SHA256 in manifest
# Initialize data once, after read testing if preferred:
DARWIN_WRITE_UMI=yes dw write "$F/data.simg" 0
```

## Supervisor: staged boots

All three stages need **UFS UEFI** (UFSDxe + PartitionDxe/BlockIo). A known local
candidate for log-only testing is the original UFS FD below; its display regression
is tracked separately in `DISPLAY-UEFI.md`. The no-UFS FD cannot service `ufs=2`.
For UI testing choose the display/UFS variant validated by the supervisor.
Start each run in a fresh loader session using the supervisor's normal procedure.
`bootcpu=7 cpus=7` leaves firmware/core0 available to serve UFS; replacing the stock
boot args avoids its `bootcpu=0`. `rd=md0` stays even when init switches to flash.

```sh
R=devices/xiaomi-mi-10/run.sh
export XL_LOADER="$F/loader.efi"
export UEFI_FD=/Users/xm666/X/umi-ios/ios-port-research/repos/JopangJovan_hackios-xnu-kernel-umi/work/display-uefi/variants/SILICIUM_UEFI.ufs-original.fd
BASE='-v keepsyms=1 rd=md0 bootcpu=7 cpus=7 ufs=2 ufs_native=0'

# (a) RAM root, read-only UFS APFS walk; returns before user space, logs only.
"$R" ramdisk "$PWD/out/xiaomi-mi-10/system.apfs"
XL_RAMDISK="$F/root-ram-data-test.apfs" XL_SECS=180 \
XL_CMDLINE="$BASE ufs_write=0 apfs_check=6 apfs_dev=ufs_darwin apfs_mnt=/System/Volumes/Darwin apfs_subtree=/usr/sbin apfs_max=64" \
  "$R" run umi-ufs-read

# (b) RAM root + HFS data only; init runs the file test and halts, no Apple launchd.
"$R" ramdisk "$PWD/out/xiaomi-mi-10/system.apfs"
XL_RAMDISK="$F/root-ram-data-test.apfs" XL_SECS=180 \
XL_CMDLINE="$BASE ufs_write=1 var_hfs=ufs_darwin_data" \
  "$R" run umi-hfs-write

# (c) Whole Apple flash root + persistent HFS /var; first run without launchd.
"$R" ramdisk "$F/flashboot.apfs"
XL_RAMDISK="$F/root-flash-data-test.apfs" XL_SECS=240 \
XL_CMDLINE="$BASE ufs_write=1 root_apfs=ufs_darwin var_hfs=ufs_darwin_data" \
  "$R" run umi-flash-root
# Repeat (b)/(c) across fresh boots: previous marker lines must survive.
# Then use root-flash-ui.apfs, with the supervisor's validated display UEFI:
"$R" ramdisk "$F/flashboot.apfs"
XL_RAMDISK="$F/root-flash-ui.apfs" XL_SECS=300 \
XL_CMDLINE="$BASE ufs_write=1 root_apfs=ufs_darwin var_hfs=ufs_darwin_data umi_touch=1 umi_keys=1 gpu=1 metal=1 fb_scan=1 fb_threads=6 fb_filter=0 cpufreq=1" \
  "$R" run umi-flash-ui
```

Read success requires APFS mount/walk with no read errors. Write success requires
`/private/var: HFS Plus from /dev/ufs_darwin_data, kept between boots` and
`umi-data-test: PASS`, then the old marker on the next boot. A RAM fallback is a
failure. Flash-root logs must confirm the Darwin volume became root and OS/App
cryptex mounts from `/dev/ufs_darwin,c=1` and `,c=2`. `root_apfs` switches root in
init; it is not a kernel `rd=ufs_darwin` boot. **Do not set `var_store=1` or use the
legacy var_store erase/save commands**: their raw slots overlap the HFS volume.
Compare `dw sum 0 2042368` before/after write tests to verify all Apple containers
are unchanged. Kernel/loader writes are restricted to the data range.

## Full system and validation

No size pruning or font trimming is applied to the flash root. Verified present:
`locationd` 30,931,872 bytes, PingFang 77,871,956 bytes, emoji font 136,714,712 bytes.
Keep hardware-dependent/expensive services disabled through the existing
`mkjobs.py` allow/deny policy and init's launchd bind mounts (locationd, CommCenter,
watchdog, etc.). Full files do not require enabling every job. Bring up storage
with the no-launchd diagnostics, then filtered UI, then add services individually;
monitor memory/crash loops. The full dyld cache still consumes RAM as pages are used.

Offline results: GPT selftest 19 checks; transport regression checks cover CRLF
text and binary hash corruption; all artifact hashes/APFS slot geometry and HFSX
primary/alternate headers, journal and allocated-block coverage pass (`verify.log`).
The host build of the kernel's APFS reader read `/usr/sbin` (33 entries, including
compressed files) successfully (`root-usr-sbin-census.log`). These are host checks;
UFS root boot, writes/persistence and display remain supervisor device tests.
