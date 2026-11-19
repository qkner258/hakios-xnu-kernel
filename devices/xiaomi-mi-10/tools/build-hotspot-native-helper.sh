#!/bin/bash
# Standalone native-control helper; no payload build.
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
source "$ROOT/xnu-env.sh"
mkdir -p "$ROOT/out/xiaomi-mi-10/hotspot-native3"
exec xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" \
  -idirafter "$XNUDEPS/MacOSX13.3.sdk/System/Library/Frameworks/Kernel.framework/Headers" \
  -idirafter "$ROOT/out/xiaomi-mi-10/xnu/bsd" -fblocks -O2 -Wall -Wextra -Werror \
  "$DEVICE_DIR/overlays/userland/src/hakios_hotspot_native.c" \
  -o "$ROOT/out/xiaomi-mi-10/hotspot-native3/hakios-hotspot-native"
