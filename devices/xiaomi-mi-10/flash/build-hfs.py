#!/usr/bin/env python3
"""Build Apple's HFS formatter for fresh regular files only, without disk attachment.

Downloaded dependency: apple-oss-distributions/hfs (APSL-2.0). The formatter's
filesystem construction is unchanged. Device pathname rewriting/ioctls and old
device signature wiping are replaced by regular-file size checks. No /dev target
can pass this variant. Called inside build-lock by prepare.py.
"""
import hashlib
from pathlib import Path
import re
import shutil
import subprocess

BASE = Path('/Users/xm666/X/umi-ios')
HERE = Path(__file__).resolve().parent
OUT = HERE.parents[2] / 'out/xiaomi-mi-10/flash/hfs-format'
ARCHIVE = BASE / 'tools/downloads/umi-hfs.tar.gz'
ARCHIVE_SHA = 'ac0831bb715d1d451a13c35c7c7b142dab5e5005c88784fd1a9ec653e7199c66'
UPSTREAM_COMMIT = 'd1bac2f062e6e9c0dfcce302d9aacb10173d0eea'


def replace_once(text, old, new):
    if text.count(old) != 1:
        raise RuntimeError('HFS source changed: ' + old[:60])
    return text.replace(old, new)


def build():
    if not ARCHIVE.exists():
        subprocess.run(['curl', '-fL', '--max-time', '120',
                        'https://codeload.github.com/apple-oss-distributions/hfs/tar.gz/refs/heads/main',
                        '-o', str(ARCHIVE)], check=True)
    if hashlib.sha256(ARCHIVE.read_bytes()).hexdigest() != ARCHIVE_SHA:
        raise RuntimeError('HFS source archive SHA256 mismatch')
    OUT.mkdir(parents=True, exist_ok=True)
    source = OUT / 'upstream'
    source.mkdir(exist_ok=True)
    subprocess.run(['tar', '-xzf', str(ARCHIVE), '-C', str(source), '--strip-components=1'], check=True)
    for name in ('newfs_hfs.c', 'makehfs.c', 'hfs_endian.c', 'hfs_endian.h', 'newfs_hfs.h'):
        shutil.copyfile(source / 'newfs_hfs' / name, OUT / name)
    p = OUT / 'newfs_hfs.c'
    s = p.read_text()
    start = s.index('\t\tspecial = argv[0];')
    end = s.index('\n\t}', start)
    s = s[:start] + '''
        struct stat umi_st;
        if (lstat(argv[0], &umi_st) || !S_ISREG(umi_st.st_mode) ||
            umi_st.st_size < 512 * 1024 || umi_st.st_size % 4096 || umi_st.st_blocks != 0)
            fatal("umi formatter requires a fresh, sparse regular file (no symlinks/devices)");
        if (strlcpy(rawdevice, argv[0], sizeof(rawdevice)) >= sizeof(rawdevice))
            fatal("image pathname too long");
        blkdevice[0] = 0;
''' + s[end:]
    s = replace_once(s, 'if (ioctl(fso, DKIOCGETBLOCKSIZE, &dip.physSectorSize) < 0)',
                     'dip.physSectorSize = 512;\n\t\tif (!S_ISREG(stbuf.st_mode))')
    s = replace_once(s, 'if (ioctl(fso, DKIOCGETBLOCKCOUNT, &dip.physTotalSectors) < 0)',
                     'dip.physTotalSectors = stbuf.st_size / 512;\n\t\tif (stbuf.st_size % 512)')
    for command in ('DKIOCGETMAXBLOCKCOUNTREAD', 'DKIOCGETMAXBLOCKCOUNTWRITE', 'DKIOCGETMAXBYTECOUNTREAD', 'DKIOCGETMAXBYTECOUNTWRITE'):
        s = replace_once(s, f'if (fso != -1 && ioctl(fso, {command}, &maxPhysPerIO) < 0)', 'if (0)')
    p.write_text(s)
    p = OUT / 'makehfs.c'
    s = p.read_bytes().decode('latin1').replace('#include <wipefs.h>', '')
    s, count = re.subn(r'dowipefs\(int fd\)\n\{.*?\n\}',
                      'dowipefs(int fd)\n{\n    (void)fd; return 0; /* fresh zero image, no device discard */\n}',
                      s, flags=re.S)
    if count != 1:
        raise RuntimeError('HFS wipefs boundary changed')
    p.write_bytes(s.encode('latin1'))
    subprocess.run(['clang', '-O2', '-Wno-deprecated-declarations', '-Wno-format',
                    '-o', str(OUT / 'mkfs.hfsplus-image'),
                    str(OUT / 'newfs_hfs.c'), str(OUT / 'makehfs.c'), str(OUT / 'hfs_endian.c'),
                    '-framework', 'CoreFoundation', '-framework', 'CoreServices', '-framework', 'IOKit'], check=True)
    (OUT / 'PROVENANCE').write_text(f'APSL-2.0 source: {ARCHIVE}\nSHA256: {ARCHIVE_SHA}\n'
                                  f'Upstream main at download: {UPSTREAM_COMMIT}\n'
                                  f'Adapter: {HERE / "build-hfs.py"}\n')
    return OUT / 'mkfs.hfsplus-image'


if __name__ == '__main__':
    print(build())
