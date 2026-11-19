#!/usr/bin/env python3
"""Unpack iPhone10,3's Mav17 baseband files from the user's own IPSW into fw-baseband/Mav17 (local only, never in git).

usage: stage-baseband-firmware.py IPSW [OUTPUT]      (OUTPUT defaults to <repo>/fw-baseband)

With umi_modem=2 CommCenter's BBUpdater looks for them in /usr/local/standalone/firmware/Baseband/Mav17
(libBasebandCapabilities) before it "boots" the baseband. The kernel's bridge takes what it sends and drops it: the X55
runs its own firmware (COMMCENTER-umi.md). userland/build.sh puts the folder in the payload, src/init.c binds it there.
"""
from pathlib import Path
import re
import sys
import zipfile


def main():
    if len(sys.argv) not in (2, 3):
        sys.exit(__doc__.split('\n\n')[1])
    ipsw = Path(sys.argv[1])
    output = Path(sys.argv[2]) if len(sys.argv) == 3 else Path(__file__).resolve().parents[3] / 'fw-baseband'
    with zipfile.ZipFile(ipsw) as archive:
        members = [n for n in archive.namelist() if re.fullmatch(r'Firmware/Mav17-[0-9.]+\.Release\.bbfw', n)]
        if len(members) != 1:
            sys.exit(f'stage-baseband-firmware.py: {ipsw}: expected one Firmware/Mav17-*.Release.bbfw, found {members}')
        target = output / 'Mav17'
        target.mkdir(parents=True, exist_ok=True)
        with archive.open(members[0]) as raw, zipfile.ZipFile(raw) as bbfw:
            for info in bbfw.infolist():
                name = info.filename
                if not re.fullmatch(r'[A-Za-z0-9_.-]{1,64}', name) or name.startswith('.'):
                    sys.exit(f'stage-baseband-firmware.py: unexpected member {name!r} in {members[0]}')
                (target / name).write_bytes(bbfw.read(info))
    print(f'{members[0]} -> {target}: {", ".join(sorted(p.name for p in target.iterdir()))}')


if __name__ == '__main__':
    main()
