#!/bin/bash
# Offline only. Clone the shipped build tree, never update the original tree.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
source /Users/xm666/X/umi-ios/env.sh
if [ "${1:-}" != --locked ]; then
    exec /Users/xm666/X/umi-ios/build-lock.sh bash "$0" --locked "$@"
fi
shift
variants=${1:-$REPO/work/display-uefi/variants2}
mkdir -p "$variants"
variants=$(cd "$variants" && pwd)
stage="$variants/build-tree"
shipped="$MU/Build/umiPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd"
before=$(shasum -a 256 "$shipped")
[ ! -e "$stage" ] || { echo "Refusing to reuse build-tree: $stage" >&2; exit 1; }
cp -cR "$MU" "$stage"
# Match the shipped SMBIOS date, so a rebuild on a later day does not change
# an unrelated PE payload. clang honors SOURCE_DATE_EPOCH for __DATE__.
SOURCE_DATE_EPOCH=$("$UEFI_PYTHON" - "$MU" <<'PY'
import datetime, re, sys
from pathlib import Path
p = Path(sys.argv[1]) / 'Build/umiPkg/RELEASE_CLANGPDB/AARCH64/SmBiosTableDxe.efi'
dates = re.findall(rb'(?:[0-9]\x00){2}/\x00(?:[0-9]\x00){2}/\x00(?:[0-9]\x00){4}', p.read_bytes())
assert len(dates) == 1
d = datetime.datetime.strptime(dates[0].decode('utf-16-le'), '%m/%d/%Y')
print(int(d.replace(tzinfo=datetime.timezone.utc).timestamp()))
PY
)
export SOURCE_DATE_EPOCH
touch "$stage/Silicon/Silicium/SiliciumPkg/Drivers/SmBiosTableDxe/SmBiosTable.h"
mkdir -p "$variants/baseline/Include"
for name in Include/APRIORI.inc Include/DXE.inc umi.dsc; do
    cp "$stage/Platforms/Xiaomi/umiPkg/$name" "$variants/baseline/$name"
done
export TMPDIR="$variants/tmp/" TMP="$variants/tmp/" TEMP="$variants/tmp/"
export PYTHONDONTWRITEBYTECODE=1 PYTHONPYCACHEPREFIX="$variants/cache/python"
export XDG_CACHE_HOME="$variants/cache" CLANG_MODULE_CACHE_PATH="$variants/cache/clang"
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME"
export MU="$stage"
export PATH="$REPO/work/host-tools/usr/bin:$PATH"
cd "$REPO"
for name in probe no-ufs no-diskio no-partition ufs-after-fb ufs-depex ufs-only diskpart-only mm-vote; do
    "$UEFI_PYTHON" "$here/configure-display-variant2.py" "$stage" "$variants/baseline" "$name"
    "$UEFI_PYTHON" "$stage/Platforms/Xiaomi/umiPkg/DeviceBuild.py" \
        TARGET=RELEASE ENABLE_SECUREBOOT=0 FD_BASE=0x9fc00000 FD_SIZE=0x300000 \
        FD_BLOCKS=0x300 DEVICE_MODEL=0 > "$variants/$name.build.log" 2>&1
    cp "$stage/Build/umiPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd" "$variants/SILICIUM_UEFI.$name.fd"
    cp "$stage/Build/umiPkg/RELEASE_CLANGPDB/FV/Guid.xref" "$variants/$name.Guid.xref"
    shasum -a 256 "$variants/SILICIUM_UEFI.$name.fd"
done
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
    -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -DLOADER_UMI \
    -I"$REPO/out/xiaomi-mi-10" -I"$REPO/devices/xiaomi-mi-10/loader" \
    -c "$REPO/devices/xiaomi-mi-10/loader/loader.c" -o "$variants/loader-display-probe.obj" \
    > "$variants/loader.build.log" 2>&1
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 \
    /out:"$variants/loader-display-probe.efi" "$variants/loader-display-probe.obj" >> "$variants/loader.build.log" 2>&1
test "$before" = "$(shasum -a 256 "$shipped")"
mkdir -p "$variants/evidence"
"$UEFI_PYTHON" "$here/verify-display-variants2.py" "$shipped" "$variants" > "$variants/evidence/verified-deltas.json"
shasum -a 256 "$variants"/*.fd "$variants/loader-display-probe.efi" > "$variants/SHA256SUMS"
echo "Offline variants complete; shipped FD unchanged: $before"
