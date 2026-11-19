#!/bin/bash
# Build only the optional AP helper; supervisor includes it via EXTRA in merged userland.
set -euo pipefail
DEVICE_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
ROOT=$(cd "$DEVICE_DIR/../.." && pwd)
source "$ROOT/xnu-env.sh"
mkdir -p "$ROOT/out/xiaomi-mi-10/hotspot-ap"
exec xnu-clang -arch arm64 -isysroot "$THEOS/sdks/iPhoneOS16.5.sdk" \
  -O1 -Wall -Wextra -Werror "$DEVICE_DIR/overlays/userland/src/umi_wifi_ap.c" \
  -o "$ROOT/out/xiaomi-mi-10/hotspot-ap/umi_wifi_ap"
