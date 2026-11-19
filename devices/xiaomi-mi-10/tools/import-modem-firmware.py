#!/usr/bin/env python3
"""Check the owner's read-only mainline bundle; optionally copy into a new private directory.

No network, partition or device writes. Calibration hashes/contents are not printed.
"""
import argparse
import hashlib
import json
from pathlib import Path
import shutil


def checked_images(reference):
    mapping = Path(__file__).with_name('modem-sahara.map').read_text(encoding='ascii')
    wanted = {int(words[0]): words[1] for line in mapping.splitlines()
              if (words := line.split()) and not words[0].startswith('#')}
    base = Path(reference) / 'work/iter1/fw'
    rows = json.loads((base / 'staged-manifest.json').read_text())
    if len(rows) != len(wanted) or {r['id']: r['name'] for r in rows} != wanted:
        raise ValueError('manifest does not match the verified 17-image recipe')
    for row in rows:
        path = base / 'staged/qcom/sdx55m' / row['name']
        if path.is_symlink() or not path.is_file() or path.stat().st_size != row['size']:
            raise ValueError('missing, indirect or wrong-sized image: ' + row['name'])
        with path.open('rb') as stream:
            digest = hashlib.file_digest(stream, 'sha256').hexdigest()
        if digest != row['sha256']:
            raise ValueError('checksum mismatch: ' + row['name'])
        if row['id'] in (16, 17, 20) and row['size'] != 8 * 1024 * 1024:
            raise ValueError('EFS must be the original 8 MiB backup')
        if row['id'] == 34 and row['size'] != 1024 * 1024:
            raise ValueError('DDR must be the original 1 MiB backup')
    return base / 'staged/qcom/sdx55m', mapping, rows


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--reference', type=Path, required=True)
    parser.add_argument('--target', type=Path,
                        help='new private directory, e.g. /Users/xm666/X/umi-ios/work/modem-fw; omit to check only')
    args = parser.parse_args()
    source, mapping, rows = checked_images(args.reference)
    print('Verified 17 local boot/calibration images; no identifiers printed')
    if args.target is None:
        return
    # Only create a new destination. Never overwrite existing inputs or follow
    # a target directory symlink into the read-only reference.
    target = args.target
    if target.resolve().is_relative_to(args.reference.resolve()):
        raise ValueError('target must be outside the read-only reference')
    target.mkdir(mode=0o700, parents=False, exist_ok=False)
    for row in rows:
        dest = target / row['name']
        shutil.copyfile(source / row['name'], dest)
        dest.chmod(0o600)
    (target / 'sahara.map').write_text(mapping, encoding='ascii')
    checksums = ''.join(row['sha256'] + '  ' + row['name'] + '\n' for row in rows)
    checksums += hashlib.sha256(mapping.encode('ascii')).hexdigest() + '  sahara.map\n'
    (target / 'SHA256SUMS').write_text(checksums, encoding='ascii')
    for name in ('sahara.map', 'SHA256SUMS'):
        (target / name).chmod(0o600)
    print('Prepared private bundle at', target)


if __name__ == '__main__':
    main()
