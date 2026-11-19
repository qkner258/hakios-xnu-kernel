#!/usr/bin/env python3
"""Offline FD inventory/DEPEX and UMI ClockDxe pointer-table evidence.

Uses the workspace's existing uefi_firmware and pefile packages. Outputs JSON;
never writes firmware. Clock-table RVAs are specific to the inspected UMI XBL.
"""
import argparse
import contextlib
import hashlib
import io
import json
import re
import struct
import uuid
from pathlib import Path

import pefile
from uefi_firmware.uefi import FirmwareFile, FirmwareFileSystemSection, FirmwareVolume

APRIORI = 'fc510ee7-ffdc-11d4-bd41-0080c73c8881'


def depex(data):
    result = []
    i = 0
    while i < len(data):
        op = data[i]
        i += 1
        if op in (0, 1, 2):
            result.append(['BEFORE', 'AFTER', 'PUSH'][op] + ' ' + str(uuid.UUID(bytes_le=data[i:i+16])))
            i += 16
        else:
            result.append({3: 'AND', 4: 'OR', 5: 'NOT', 6: 'TRUE', 7: 'FALSE', 8: 'END', 9: 'SOR'}[op])
    return result


def inventory(path):
    data = path.read_bytes()
    fv = FirmwareVolume(data[data.index(b'_FVH') - 40:])
    with contextlib.redirect_stdout(io.StringIO()):
        assert fv.process(), path
    files = {}

    def sections(obj):
        for child in getattr(obj, 'objects', []):
            if isinstance(child, FirmwareFile):
                continue  # Never attribute a nested FV's modules to its container.
            if isinstance(child, FirmwareFileSystemSection) and child.type in (0x10, 0x13, 0x15, 0x19):
                yield child
            elif hasattr(child, '__dict__'):
                yield from sections(child)

    def walk(obj):
        if isinstance(obj, FirmwareFile):
            guid = str(uuid.UUID(bytes_le=obj.guid))
            secs = list(sections(obj))
            info = {'name': next((s.name for s in secs if s.type == 0x15), guid), 'sections': []}
            for s in secs:
                entry = {'type': s.type, 'bytes': len(s.data), 'sha256': hashlib.sha256(s.data).hexdigest()}
                if s.type == 0x13:
                    entry['depex'] = depex(s.data)
                if guid == APRIORI and s.type == 0x19:
                    info['order'] = [str(uuid.UUID(bytes_le=s.data[i:i+16])) for i in range(0, len(s.data), 16)]
                info['sections'].append(entry)
            files[guid] = info
        for child in getattr(obj, 'objects', []):
            if hasattr(child, '__dict__'):
                walk(child)
    walk(fv)
    return {'path': str(path), 'bytes': len(data), 'sha256': hashlib.sha256(data).hexdigest(), 'files': files}


def clock_tables(mu):
    p = mu / 'Binaries/umi/QcomPkg/Drivers/ClockDxe/ClockDxe.efi'
    image = pefile.PE(str(p)).get_memory_mapped_image()
    clocks = {}
    for name in ('gcc_disp_ahb_clk', 'gcc_ufs_phy_ahb_clk', 'gcc_sdcc2_ahb_clk',
                 'gcc_disp_hf_axi_clk', 'gcc_disp_sf_axi_clk', 'gcc_ufs_phy_axi_clk'):
        string_rva = image.index(name.encode() + b'\0')
        matches = []
        for match in re.finditer(re.escape(struct.pack('<Q', string_rva)), image):
            table = match.start()
            parent = struct.unpack_from('<Q', image, table + 8)[0]
            if 0x2a000 <= parent < 0x2d000:
                branch = struct.unpack_from('<Q', image, table + 24)[0]
                root = struct.unpack_from('<Q', image, parent + 40)[0]
                matches.append(dict(table_rva=hex(table), domain_rva=hex(parent), cbcr=hex(branch), root_cmd=hex(root)))
        assert len(matches) == 1, (name, 'ClockDxe layout differs from inspected revision')
        clocks[name] = matches[0]
    return {'sha256': hashlib.sha256(p.read_bytes()).hexdigest(), 'clocks': clocks}


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--mu', type=Path, required=True)
    ap.add_argument('fd', type=Path, nargs='+')
    args = ap.parse_args()
    print(json.dumps({'firmware': [inventory(p) for p in args.fd], 'clock_tables': clock_tables(args.mu)}, indent=2))
