#!/usr/bin/env python3
"""Revision-specific, offline NPA/voltage evidence from the installed UMI binaries."""
import argparse
import hashlib
import json
import re
import struct
from pathlib import Path
import pefile

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('mu', type=Path)
args = ap.parse_args()
root = args.mu / 'Binaries/umi/QcomPkg/Drivers'
images, hashes = {}, {}
for name in ('UFSDxe', 'ClockDxe', 'NpaDxe', 'VcsDxe', 'RpmhDxe', 'DALSYSDxe'):
    p = root / name / (name + ('.patched.efi' if name == 'DALSYSDxe' else '.efi'))
    hashes[name] = hashlib.sha256(p.read_bytes()).hexdigest()
    images[name] = pefile.PE(str(p)).get_memory_mapped_image()

def q(name, rva):
    return struct.unpack_from('<Q', images[name], rva)[0]

def string(name, rva):
    return images[name][rva:images[name].index(b'\0', rva)].decode()

# Verify the protocol ABI before documenting/using it in UmiDisplayMmVote.
assert q('NpaDxe', 0x110a8) == 0x10002
assert q('NpaDxe', 0x110b8) == 0x1430
assert q('NpaDxe', 0x110d0) == 0x148c
boot_rails = {string('ClockDxe', q('ClockDxe', r)): q('ClockDxe', r + 8)
              for r in range(0x311d8, 0x31218, 16)}
assert boot_rails['/vcs/vdd_mm'] == 0x100
assert string('VcsDxe', q('VcsDxe', 0x9450)) == '/vcs/vdd_mm'
assert q('VcsDxe', 0x9440) == 0x9d20
assert string('VcsDxe', q('VcsDxe', 0x9d20)) == 'mmcx.lvl'

pmic_nodes = {}
for node in range(0x1a6f0, 0x1a9f0, 64):
    name = string('DALSYSDxe', q('DALSYSDxe', node))
    deps, count = q('DALSYSDxe', node + 48), q('DALSYSDxe', node + 56)
    pmic_nodes[name] = {'table_rva': hex(node), 'dependencies': [
        {'resource': string('DALSYSDxe', q('DALSYSDxe', deps + i * 24)),
         'client_type': hex(q('DALSYSDxe', deps + i * 24 + 8))} for i in range(count)]}
assert '/pmic/client/ufs0' in pmic_nodes

clock_rails = {}
im = images['ClockDxe']
for name in ('gcc_disp_ahb_clk', 'gcc_ufs_phy_ahb_clk', 'gcc_ufs_phy_axi_clk',
             'gcc_disp_hf_axi_clk', 'gcc_disp_sf_axi_clk'):
    s = im.index(name.encode() + b'\0')
    found = []
    for m in re.finditer(re.escape(struct.pack('<Q', s)), im):
        table = m.start()
        domain = q('ClockDxe', table + 8)
        if not 0x2a000 <= domain < 0x2d000:
            continue
        rail = q('ClockDxe', domain + 16)
        found.append(dict(table_rva=hex(table), domain_rva=hex(domain),
                          rail_rva=hex(rail), rail=string('ClockDxe', q('ClockDxe', rail))))
    assert len(found) == 1
    clock_rails[name] = found[0]
print(json.dumps(dict(sha256=hashes, npa_revision='0x10002',
                     npa_slots={'create_sync_client': '0x10 -> RVA 0x1430',
                                'issue_required_request': '0x28 -> RVA 0x148c'},
                     boot_rail_levels=boot_rails, clock_rails=clock_rails,
                     vcs_mm={'rail_object_rva': '0x9420', 'resource_rva': '0x9450',
                             'rpmh_config_rva': '0x9d20', 'rpmh_resource': 'mmcx.lvl'},
                     pmic_nodes=pmic_nodes), indent=2))
