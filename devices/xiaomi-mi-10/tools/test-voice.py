#!/usr/bin/env python3
"""Exercise the actual gated voice observer and NAS reader without hardware."""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[3]
device = root / 'devices/xiaomi-mi-10'
out = root / 'out/xiaomi-mi-10/voice/tests'
dev = device / 'overlays/xnu/files/bsd/dev'
ports = [int(re.search(r'#define ' + macro + r' (0x[0-9a-f]+)', (dev / name).read_text())[1], 16)
         for name, macro in [('umi_voice.inc', 'VOICE_PORT'), ('umi_gnss_qmi.h', 'UGL_PORT'),
                             ('umi_cellinfo_qmi.h', 'UCI_PORT')]]
assert len(set(ports)) == 3, 'QRTR diagnostic endpoints must be independent'
(out / 'include/sys').mkdir(parents=True, exist_ok=True)
for name in ('vnode.h', 'vnode_if.h', 'uio.h'):
    (out / 'include/sys' / name).write_text('/* Supplied by voice.cpp. */\n')
subprocess.run(['clang++', '-std=c++14', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-fsanitize=address,undefined',
                '-I' + str(out / 'include'), '-I' + str(device / 'overlays/xnu/files/bsd/dev'),
                str(device / 'tools/voice-tests/voice.cpp'), '-o', str(out / 'voice')], check=True)
subprocess.run([str(out / 'voice')], check=True)
