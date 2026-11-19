#!/usr/bin/env python3
"""The phone's compiled Metal stages (/private/var/db/hakios-metal-cache/*.stage) into userland/hakios-metal-cache,
which the userland build puts into the image at /redwood/share/hakios-metal-cache (the window server's read-only seed,
HakiosMetalCA.m: HM_CACHE_SEED). The next image then starts with every shader the last boots compiled.

--image F    a local copy of the HFS data volume (or of darwin with --offset)
--android    SUPERVISOR ONLY, phone in Android: bounded read-only dd of darwin through read-usb-hfs.py's backend.

A stage is filed under a hash of the compiler's fingerprint (hakios-metal Makefile: IOS_CACHE_BUILD) and its inputs, so
stages of another compiler are never found: they cost room, nothing else. Only the newest fingerprint's are wanted, but
the file name does not tell it, so the seed is replaced whole by what the phone has now. No journal replay: a stage the
last boot wrote may be missing (it is compiled again), and a torn one is refused by the loader (stageCacheLoad).
"""
import argparse
import importlib.util
from pathlib import Path
import shutil
import struct
import subprocess

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('read_usb_hfs', HERE / 'read-usb-hfs.py')
hfs = importlib.util.module_from_spec(spec)
spec.loader.exec_module(hfs)

MAGIC = struct.pack('<I', 0x53434d48)   # compiler.cpp kCacheMagic, "HMCS"


def stage_files(volume):
    """[(name, Fork)] of db/hakios-metal-cache/*.stage, in disk order"""
    header = volume.catalog.read(0, 64)
    node = struct.unpack_from('>I', header, 24)[0]
    node_size = struct.unpack_from('>H', header, 32)[0]
    folders, files, seen = {}, [], set()
    while node:
        if node in seen: raise ValueError('catalog leaf loop')
        seen.add(node)
        next_node, kind, height, records = hfs.node_records(volume.catalog.read(node * node_size, node_size))
        if kind != -1 or height != 1: raise ValueError('invalid catalog leaf')
        for rec in records:
            keylen, parent, chars = struct.unpack_from('>HIH', rec)
            if keylen != 6 + chars * 2 or keylen + 4 > len(rec): raise ValueError('invalid catalog key')
            name = rec[8:8 + chars * 2].decode('utf-16-be')
            data = rec[2 + keylen:]
            kind = struct.unpack_from('>H', data)[0]
            if kind == 1:
                folders[(parent, name)] = struct.unpack_from('>I', data, 8)[0]
            elif kind == 2 and name.endswith('.stage'):
                if len(data) < 168: raise ValueError('short file record')
                files.append((parent, name, struct.unpack_from('>I', data, 8)[0], data[88:168]))
        node = next_node
    db = folders.get((2, 'db'))
    cache = folders.get((db, 'hakios-metal-cache')) if db is not None else None
    if cache is None: raise ValueError('no /private/var/db/hakios-metal-cache on this volume')
    forks = [(name, hfs.Fork(volume, fork, file_id)) for parent, name, file_id, fork in files if parent == cache]
    # (sorted by where they lie: the Android backend keeps eight 1 MiB pages)
    return sorted(forks, key=lambda f: f[1].extents[0][0] if f[1].extents else 0)


def main():
    p = argparse.ArgumentParser(description=__doc__)
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument('--image', type=Path)
    source.add_argument('--android', action='store_true')
    p.add_argument('--serial', default='b54a311e')
    p.add_argument('--offset', type=int, help='bytes; default 0 for an image, the UMI HFS data start for Android')
    p.add_argument('--output', type=Path, default=HERE.parents[2] / 'userland' / 'hakios-metal-cache')
    args = p.parse_args()
    backend = hfs.Android(args.serial) if args.android else hfs.Image(args.image)
    try:
        offset = args.offset if args.offset is not None else hfs.DATA_OFFSET if args.android else 0
        if args.android and offset != hfs.DATA_OFFSET: raise ValueError('Android offset must be UMI HFS data start')
        new = args.output.with_name(args.output.name + '.new')
        shutil.rmtree(new, ignore_errors=True)
        new.mkdir(parents=True)
        kept = refused = total = 0
        for name, fork in stage_files(hfs.HFS(backend, offset)):
            if fork.size < 8 or fork.size > 4 << 20:
                refused += 1
                continue
            raw = fork.read(0, fork.size)
            if raw[:4] != MAGIC:
                refused += 1
                continue
            (new / name).write_bytes(raw)
            kept += 1
            total += len(raw)
        if not kept: raise ValueError('the phone has no compiled stages (was the boot a CA Metal one with var_hfs?)')
        shutil.rmtree(args.output, ignore_errors=True)
        new.rename(args.output)
        print(f'metal-cache-pull: {kept} stages ({total // 1024} KB) into {args.output}, {refused} refused; '
              f'rebuild the CA userland payload to carry them')
    finally:
        backend.close()


if __name__ == '__main__':
    try: main()
    except (ValueError, OSError, struct.error, subprocess.CalledProcessError) as exc:
        raise SystemExit(str(exc))
