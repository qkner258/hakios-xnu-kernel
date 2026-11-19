#!/usr/bin/env python3
"""Verify the final FV payloads and exact APRIORI edits against the shipped FD."""
import argparse
import importlib.util
import json
from pathlib import Path

spec = importlib.util.spec_from_file_location('inspect_fd', Path(__file__).with_name('inspect-display-firmware.py'))
inspect_fd = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inspect_fd)
ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('shipped', type=Path)
ap.add_argument('variants', type=Path)
args = ap.parse_args()
baseline = inspect_fd.inventory(args.shipped)
b = baseline['files']
by_name = {v['name']: k for k, v in b.items()}
order = b[inspect_fd.APRIORI]['order']
results = []
omissions = {'no-ufs': ['UFSDxe'], 'no-diskio': ['DiskIoDxe'],
             'no-partition': ['PartitionDxe'],
             'ufs-only': ['DiskIoDxe', 'PartitionDxe', 'SdccDxe'],
             'diskpart-only': ['SdccDxe', 'UFSDxe']}
for name in ('probe', 'no-ufs', 'no-diskio', 'no-partition', 'ufs-after-fb',
             'ufs-depex', 'ufs-only', 'diskpart-only', 'mm-vote'):
    item = inspect_fd.inventory(args.variants / f'SILICIUM_UEFI.{name}.fd')
    f = item['files']
    assert item['bytes'] == baseline['bytes'] == 0x300000
    removed = {by_name[n] for n in omissions.get(name, [])}
    assert b.keys() - f.keys() == removed, name
    added = f.keys() - b.keys()
    new_names = {f[g]['name'] for g in added}
    want_new = ({f'UmiDisplayProbe{i}' for i in range(1, 9)} if name == 'probe' else
                {'UmiDisplayMmVote'} if name == 'mm-vote' else set())
    assert new_names == want_new, (name, new_names)
    # All retained PE32/DEPEX/UI/RAW payloads must be byte-identical. This also
    # catches unrelated date/version changes introduced by a fresh compilation.
    for guid in b.keys() & f.keys() - {inspect_fd.APRIORI}:
        assert f[guid] == b[guid], (name, f[guid]['name'], 'unintended payload change')
    expected = [g for g in order if g not in removed]
    f_by_name = {v['name']: k for k, v in f.items()}
    if name in ('ufs-after-fb', 'ufs-depex'):
        expected.remove(by_name['UFSDxe'])
        if name == 'ufs-after-fb':
            expected.insert(expected.index(by_name['SimpleFbDxe']) + 1, by_name['UFSDxe'])
    if name == 'mm-vote':
        expected.insert(expected.index(by_name['ClockDxe']) + 1, f_by_name['UmiDisplayMmVote'])
    if name == 'probe':
        for stage, anchor, before in ((1, 'ClockDxe', True), (2, 'ClockDxe', False),
                                     (3, 'DiskIoDxe', True), (4, 'DiskIoDxe', False),
                                     (5, 'PartitionDxe', False), (6, 'UFSDxe', True),
                                     (7, 'UFSDxe', False), (8, 'SimpleFbDxe', False)):
            expected.insert(expected.index(by_name[anchor]) + (not before), f_by_name[f'UmiDisplayProbe{stage}'])
    assert f[inspect_fd.APRIORI]['order'] == expected, (name, 'APRIORI order')
    results.append(dict(variant=name, sha256=item['sha256'], bytes=item['bytes'],
                        removed=sorted(omissions.get(name, [])), added=sorted(new_names),
                        retained_payloads_identical=True,
                        apriori=[f.get(g, {}).get('name', g) for g in expected]))
print(json.dumps(dict(shipped_sha256=baseline['sha256'], variants=results), indent=2))
