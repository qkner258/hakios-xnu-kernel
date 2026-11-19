#!/usr/bin/env python3
"""Stage locally supplied X55 boot images into an APFS build directory; no device I/O."""
import argparse
import hashlib
from pathlib import Path
import re
import shutil


def validate(source):
    source=Path(source)
    names={'sbl1.mbn','sahara.map'}
    ids=set()
    image_map=(source/'sahara.map').read_text(encoding='ascii')
    if len(image_map.encode())>4095:
        raise ValueError('sahara.map exceeds the kernel parser limit')
    for line in image_map.splitlines():
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        fields=line.split()
        if len(fields)!=2 or not fields[0].isdecimal():
            raise ValueError('expected decimal image ID and basename, one pair per line')
        image_id=int(fields[0]);name=fields[1]
        if not 1<=image_id<=65535 or image_id in ids or len(ids)>=32:
            raise ValueError('duplicate, excessive, or invalid image ID')
        if not re.fullmatch(r'[A-Za-z0-9_.-]{1,63}',name) or '..' in name:
            raise ValueError('invalid image basename')
        ids.add(image_id);names.add(name)
    if not ids:
        raise ValueError('empty Sahara image map; recover IDs from the working loader')
    sums={}
    for line in (source/'SHA256SUMS').read_text(encoding='ascii').splitlines():
        digest,name=line.split(maxsplit=1)
        name=name.lstrip('*')
        if re.fullmatch(r'[0-9a-fA-F]{64}',digest):
            sums[name]=digest.lower()
    for name in names:
        path=source/name
        if path.is_symlink() or not path.is_file():
            raise ValueError(f'{name}: regular local file required')
        limit=4095 if name=='sahara.map' else (4*1024*1024 if name=='sbl1.mbn' else 512*1024*1024)
        if not 0<path.stat().st_size<=limit:
            raise ValueError(f'{name}: invalid size')
        with path.open('rb') as stream:
            digest=hashlib.file_digest(stream,'sha256').hexdigest()
        if sums.get(name)!=digest:
            raise ValueError(f'{name}: missing or mismatched SHA256SUMS entry')
    return sorted(names)


def main():
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--source',type=Path,required=True)
    parser.add_argument('--target',type=Path,required=True,help='staging sysroot/fw/sdx55 directory')
    args=parser.parse_args()
    names=validate(args.source)
    args.target.mkdir(parents=True,exist_ok=True)
    if args.target.resolve()==args.source.resolve():
        raise SystemExit('source and target must differ')
    for name in names:
        if (args.target/name).is_symlink():
            raise SystemExit(f'refusing target symlink: {name}')
        shutil.copyfile(args.source/name,args.target/name)
    if (args.target/'SHA256SUMS').is_symlink():
        raise SystemExit('refusing target checksum symlink')
    shutil.copyfile(args.source/'SHA256SUMS',args.target/'SHA256SUMS')
    print(f'UMI X55: staged {len(names)} verified local files into {args.target}')


if __name__=='__main__':
    main()
