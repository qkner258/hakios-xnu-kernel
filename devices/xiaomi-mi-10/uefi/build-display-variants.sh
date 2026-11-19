#!/bin/bash
# Offline builds only. No phone access; never replace the shipped FD/loader.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
source /Users/xm666/X/umi-ios/env.sh
if [ "${1:-}" != --locked ]; then
    exec /Users/xm666/X/umi-ios/build-lock.sh bash "$0" --locked "$@"
fi
shift
variants=${1:-/Users/xm666/X/umi-ios/work/uefi-variants}
mkdir -p "$variants"
variants=$(cd "$variants" && pwd)
stage="$variants/build-tree"
shipped="$MU/Build/umiPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd"
before=$(shasum -a 256 "$shipped")
cp "$shipped" "$variants/SILICIUM_UEFI.ufs-original.fd"

# APFS clones keep sources and build outputs independent without a download.
# Refuse to reuse a partial/stale tree after an interrupted build.
[ ! -e "$stage" ] || { echo "Remove or rename the old variant build-tree first: $stage" >&2; exit 1; }
cp -cR "$MU" "$stage"
cp -R "$here/files/Platforms/Xiaomi/umiPkg/." "$stage/Platforms/Xiaomi/umiPkg/"

# env.sh's shared cache locations can be read-only in an agent sandbox.
export TMPDIR="$variants/tmp/" TMP="$variants/tmp/" TEMP="$variants/tmp/"
export PYTHONDONTWRITEBYTECODE=1
export PYTHONPYCACHEPREFIX="$variants/cache/python"
export XDG_CACHE_HOME="$variants/cache"
export CLANG_MODULE_CACHE_PATH="$variants/cache/clang"
mkdir -p "$TMPDIR" "$XDG_CACHE_HOME"
export MU="$stage"
export PATH="$REPO/work/host-tools/usr/bin:$PATH"
cd "$REPO"

build_variant() {
    local name=$1 vote=$2 sdcc=$3
    "$UEFI_PYTHON" "$stage/Platforms/Xiaomi/umiPkg/DeviceBuild.py" \
        TARGET=RELEASE ENABLE_SECUREBOOT=0 FD_BASE=0x9fc00000 FD_SIZE=0x300000 \
        FD_BLOCKS=0x300 DEVICE_MODEL=0 UMI_DISPLAY_CLOCK_VOTE="$vote" UMI_ENABLE_SDCC="$sdcc" \
        > "$variants/$name.build.log" 2>&1
    cp "$stage/Build/umiPkg/RELEASE_CLANGPDB/FV/SILICIUM_UEFI.fd" "$variants/SILICIUM_UEFI.$name.fd"
    cp "$stage/Build/umiPkg/RELEASE_CLANGPDB/FV/Guid.xref" "$variants/$name.Guid.xref"
    shasum -a 256 "$variants/SILICIUM_UEFI.$name.fd"
}

build_variant ufs-nosdcc 0 0
build_variant ufs-sdcc-display-vote 1 1
build_variant ufs-nosdcc-display-vote 1 0

# Optional diagnostics loader, separate from out/xiaomi-mi-10/loader.efi.
clang --target=aarch64-unknown-windows -ffreestanding -fno-builtin -fno-stack-protector -fshort-wchar \
    -mgeneral-regs-only -O1 -Wall -Wextra -Wno-unused-parameter -DLOADER_UMI \
    -I"$REPO/out/xiaomi-mi-10" -I"$REPO/devices/xiaomi-mi-10/loader" \
    -c "$REPO/devices/xiaomi-mi-10/loader/loader.c" -o "$variants/loader-display-diag.obj"
lld-link /subsystem:efi_application /entry:efi_main /nodefaultlib /base:0xA1000000 \
    /out:"$variants/loader-display-diag.efi" "$variants/loader-display-diag.obj"

test "$before" = "$(shasum -a 256 "$shipped")"
shasum -a 256 "$variants"/*.fd "$variants/loader-display-diag.efi" > "$variants/SHA256SUMS"
echo "Offline variants complete; shipped FD unchanged: $before"
