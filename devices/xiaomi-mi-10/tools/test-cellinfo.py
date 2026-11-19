#!/usr/bin/env python3
"""Test the actual portable translator and bridge session, without a phone."""
from pathlib import Path
import subprocess
root = Path(__file__).resolve().parents[3]
device = root / 'devices/xiaomi-mi-10'
out = root / 'out/xiaomi-mi-10/cellinfo/tests'
(out / 'include/sys').mkdir(parents=True, exist_ok=True)
for header in ('vnode.h', 'vnode_internal.h'):
    (out / 'include/sys' / header).write_text('/* Kernel shims are in cellinfo.cpp. */\n')
subprocess.run(['clang++', '-std=c++14', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                '-Wno-unused-function', '-Wno-missing-field-initializers', '-fsanitize=address,undefined',
                '-I' + str(out / 'include'), '-I' + str(device / 'overlays/xnu/files/bsd/dev'),
                str(device / 'tools/cellinfo-tests/cellinfo.cpp'), '-o', str(out / 'cellinfo')], check=True)
subprocess.run([str(out / 'cellinfo')], check=True)
