#!/usr/bin/env python3
"""Hardware-free tests of the IMS protocol and actual bridge observer/audit.

The C++ bridge run also covers the synthesized WDS DAD-complete (0x5561) arming rules:
no address ioctl, no request-side APN, once per activation, link-local/teardown/switch-off,
under either IMS owner (ims-on and route B modem-ims, with the off-edge guard and the idempotent
switch), the 0x2d reply held for the IMS context until pdp_ipN has a global IPv6 address (bounded
at IMS_DAD_HOLD_MS), the delivery-time address sent with its 17th byte, and the IMSA 0x20/0x23
error-code TLVs (numeric code, text length only).
"""
from pathlib import Path
import re
import subprocess

root = Path(__file__).resolve().parents[3]
device = root / 'devices/xiaomi-mi-10'
dev = device / 'overlays/xnu/files/bsd/dev'
ports = [int(re.search(r'#define ' + macro + r' (0x[0-9a-f]+)', (dev / name).read_text())[1], 16)
         for name, macro in [('umi_ims_protocol.h', 'UMI_IMS_AUDIT_PORT'), ('umi_voice.inc', 'VOICE_PORT'),
                             ('umi_gnss_qmi.h', 'UGL_PORT'), ('umi_cellinfo_qmi.h', 'UCI_PORT'),
                             ('umi_ims_protocol.h', 'UMI_IMS_IMSS_AUDIT_PORT'),
                             ('umi_ims_protocol.h', 'UMI_IMS_DCM_PORT')]]
assert len(set(ports)) == 6, 'IMS, IMSS, DCM, voice, GNSS and cell-info need independent diagnostic endpoints'
out = root / 'out/xiaomi-mi-10/ims/tests'
out.mkdir(parents=True, exist_ok=True)
binary = out / 'ims'
subprocess.run([
    'clang', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
    '-fsanitize=address,undefined',
    '-I' + str(device / 'overlays/xnu/files/bsd/dev'),
    str(device / 'tools/ims-tests/ims.c'), '-o', str(binary)
], check=True)
subprocess.run([str(binary)], check=True)
for name in ('sys/vnode.h', 'sys/vnode_if.h', 'netinet/in_var.h', 'netinet6/in6_var.h'):
    header = out / 'include' / name
    header.parent.mkdir(parents=True, exist_ok=True)
    header.write_text('/* Kernel declarations supplied by bridge.cpp. */\n')
subprocess.run([
    'clang++', '-std=c++14', '-O1', '-Wall', '-Wextra', '-Werror',
    '-Wno-unused-function', '-Wno-unused-variable', '-fsanitize=address,undefined',
    '-I' + str(out / 'include'),
    '-I' + str(device / 'overlays/xnu/files/bsd/dev'),
    str(device / 'tools/ims-tests/bridge.cpp'), '-o', str(out / 'bridge')
], check=True)
subprocess.run([str(out / 'bridge')], check=True)
