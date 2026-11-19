#!/usr/bin/env python3
"""Compare two ARM64 COFF objects or PE images, excluding ONLY their timestamps.
Usage: compare-loader-code.py before.{obj,efi} after.{obj,efi}
No disassembly assumptions: every other byte must match, including all data.
"""
import hashlib
from pathlib import Path
import struct
import sys


def normalized(path):
    data = bytearray(Path(path).read_bytes())
    if data[:2] == b'MZ':
        pe = struct.unpack_from('<I', data, 0x3c)[0]
        assert data[pe:pe + 4] == b'PE\0\0', 'bad PE signature'
        coff = pe + 4
    else:
        coff = 0
    assert struct.unpack_from('<H', data, coff)[0] == 0xaa64, 'not ARM64 COFF'
    data[coff + 4:coff + 8] = bytes(4)
    return data


if __name__ == '__main__':
    if len(sys.argv) != 3:
        raise SystemExit(__doc__)
    before, after = map(normalized, sys.argv[1:])
    if before != after:
        raise SystemExit('FAIL: bytes other than the COFF/PE timestamp changed')
    print('PASS: all bytes match except timestamp; normalized sha256=' + hashlib.sha256(before).hexdigest())
