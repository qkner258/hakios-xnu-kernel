#!/bin/bash
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
"$DEVICE_DIR/build-metal.sh"
export HAKIOS_METAL=${HAKIOS_METAL:-$ROOT/hakios-metal}
export HAKIOS_ROOT=${HAKIOS_ROOT:-$ROOT}
python3 "$DEVICE_DIR/prepare.py" userland
export DEVICE=umi BLUETOOTH=${BLUETOOTH:-1} GL=${GL:-1}
if [ -f "$HAKIOS_METAL/out/ios/hakios-metal-probe" ]; then
  export EXTRA="${EXTRA:+$EXTRA }$HAKIOS_METAL/out/ios/hakios-metal-probe"
fi
# Normal images hand launchd through the stock rc.jobs chain to SpringBoard.
# The UMI hello_dyn and Metal/shader recipes remain explicit diagnostics via
# RC_JOBS or the UMI_*_TEST switches below.
export RC_JOBS=${RC_JOBS:-rc.jobs}
if [ "${UMI_METAL_TEST:-0}" = 1 ]; then
  export RC_JOBS=rc.jobs.umi-metal
fi
if [ "${UMI_METAL_SHADERS:-0}" = 1 ]; then
  export AIR=1 RC_JOBS=rc.jobs.umi-shaders
  "$ROOT/tools/mk-mesa.sh"
fi
# A cached system image from before Metal integration lacks the plugin link.
# Rebuild it once when this device's system layout changes, then cache it again.
layout="umi-${METAL_PLUGIN:-gl}-v1"
stamp="$ROOT/out/xiaomi-mi-10/system-layout"
if [ "$(cat "$stamp" 2>/dev/null || true)" != "$layout" ]; then
  export SYSTEM=1
fi
export ADSP= REST=${REST:-1}
"$ROOT/out/xiaomi-mi-10/userland-build/userland/build.sh" "$@"
for image in root.apfs system.apfs; do
  [ -f "$ROOT/out/xiaomi-mi-10/userland-build/userland/$image" ] || {
    echo "UMI userland build did not produce $image; unpack iOS 16.5 sysimage into $ROOT/sysimage and rerun" >&2
    exit 1
  }
done
if [ -f "$ROOT/out/xiaomi-mi-10/system.apfs" ]; then
  listing=$(python3 "$ROOT/tools/apfs/apfs.py" ls "$ROOT/out/xiaomi-mi-10/system.apfs" /System/Library/Extensions)
  [[ "$listing" == *HakiosMetal.bundle* ]] || { echo 'System image lacks HakiosMetal.bundle; rebuild with SYSTEM=1' >&2; exit 1; }
  printf '%s\n' "$layout" > "$stamp"
fi
# Keep the validation image separate from the ordinary boot image.
if [ "${UMI_METAL_TEST:-0}" = 1 ]; then
  cp "$ROOT/out/xiaomi-mi-10/root.apfs" "$ROOT/out/xiaomi-mi-10/metal-test.apfs"
fi
if [ "${UMI_METAL_SHADERS:-0}" = 1 ]; then
  cp "$ROOT/out/xiaomi-mi-10/root.apfs" "$ROOT/out/xiaomi-mi-10/metal-shader-test.apfs"
fi
