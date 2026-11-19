#!/bin/bash
# Host-only. Serialize with the forward-port agent; never runs a phone command.
set -euo pipefail
source /Users/xm666/X/umi-ios/env.sh
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
exec /Users/xm666/X/umi-ios/build-lock.sh python3 "$here/prepare.py" "$@"
