#!/usr/bin/env python3
"""ASan/UBSan tests of the actual LOC parser and session code; no phone needed."""
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[3]
device = root / 'devices/xiaomi-mi-10'
out = root / 'out/xiaomi-mi-10/gnss/tests'
(out / 'include/sys').mkdir(parents=True, exist_ok=True)
for header in ('sysctl.h', 'kauth.h'):
    (out / 'include/sys' / header).write_text('/* Facilities supplied by loc.cpp. */\n')
subprocess.run(['clang++', '-std=c++14', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-missing-field-initializers', '-fsanitize=address,undefined',
                '-I' + str(out / 'include'),
                '-I' + str(device / 'overlays/xnu/files/bsd/dev'),
                str(device / 'tools/gnss-tests/loc.cpp'), '-o', str(out / 'loc')], check=True)
subprocess.run([str(out / 'loc')], check=True)
