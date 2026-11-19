#!/usr/bin/env python3
"""Configure only an isolated Mu tree; start each experiment from its saved baseline."""
import argparse
import shutil
import uuid
from pathlib import Path

ap = argparse.ArgumentParser(description=__doc__)
ap.add_argument('tree', type=Path)
ap.add_argument('baseline', type=Path)
ap.add_argument('variant', choices=('probe', 'no-ufs', 'no-diskio', 'no-partition',
                                  'ufs-after-fb', 'ufs-depex', 'ufs-only', 'diskpart-only', 'mm-vote'))
args = ap.parse_args()
pkg = args.tree / 'Platforms/Xiaomi/umiPkg'
for name in ('Include/APRIORI.inc', 'Include/DXE.inc', 'umi.dsc'):
    shutil.copy2(args.baseline / name, pkg / name)
apriori = (pkg / 'Include/APRIORI.inc').read_text()
dxe = (pkg / 'Include/DXE.inc').read_text()
dsc = (pkg / 'umi.dsc').read_text()

def inf(name):
    if name in ('DiskIoDxe', 'PartitionDxe'):
        return f'  INF Binaries/umi/MdeModulePkg/Universal/Disk/{name}/{name}.inf\n'
    if name == 'SimpleFbDxe':
        return '  INF SiliciumPkg/Drivers/SimpleFbDxe/SimpleFbDxe.inf\n'
    return f'  INF Binaries/umi/QcomPkg/Drivers/{name}/{name}.inf\n'

remove = {
    'no-ufs': ('UFSDxe',), 'no-diskio': ('DiskIoDxe',),
    'no-partition': ('PartitionDxe',),
    'ufs-only': ('DiskIoDxe', 'PartitionDxe', 'SdccDxe'),
    'diskpart-only': ('UFSDxe', 'SdccDxe'),
}.get(args.variant, ())
for name in remove:
    assert apriori.count(inf(name)) == dxe.count(inf(name)) == 1
    apriori = apriori.replace(inf(name), '')
    dxe = dxe.replace(inf(name), '')
if args.variant in ('ufs-after-fb', 'ufs-depex'):
    assert apriori.count(inf('UFSDxe')) == 1
    apriori = apriori.replace(inf('UFSDxe'), '')
    if args.variant == 'ufs-after-fb':
        assert apriori.count(inf('SimpleFbDxe')) == 1
        apriori = apriori.replace(inf('SimpleFbDxe'), inf('SimpleFbDxe') + inf('UFSDxe'))
if args.variant in ('probe', 'mm-vote'):
    driver = pkg / 'Drivers/UmiDisplayProbeDxe'
    driver.mkdir(parents=True, exist_ok=True)
    here = Path(__file__).resolve().parent
    for src in ('UmiDisplayProbe.h', 'UmiDisplayProbe.c'):
        shutil.copy2(here / 'files/Platforms/Xiaomi/umiPkg/Drivers/UmiDisplayProbeDxe' / src, driver / src)
if args.variant == 'mm-vote':
    for src in ('UmiDisplayMmVote.c', 'UmiDisplayMmVote.inf'):
        shutil.copy2(here / 'files/Platforms/Xiaomi/umiPkg/Drivers/UmiDisplayProbeDxe' / src, driver / src)
    path = 'umiPkg/Drivers/UmiDisplayProbeDxe/UmiDisplayMmVote.inf'
    marker = f'  INF {path}\n'
    apriori = apriori.replace(inf('ClockDxe'), inf('ClockDxe') + marker)
    dxe += marker
    dsc += f'\n[Components]\n  {path}\n'
if args.variant == 'probe':
    # One source, eight entry-point instances with distinct FFS GUIDs. APRIORI
    # runs each exactly where placed; event callbacks belong to the first one.
    positions = ((1, 'ClockDxe', True), (2, 'ClockDxe', False),
                 (3, 'DiskIoDxe', True), (4, 'DiskIoDxe', False),
                 (5, 'PartitionDxe', False), (6, 'UFSDxe', True),
                 (7, 'UFSDxe', False), (8, 'SimpleFbDxe', False))
    for stage, anchor, before in positions:
        name = f'UmiDisplayProbe{stage}'
        guid = uuid.uuid5(uuid.UUID('8e4060d2-31bc-4d78-8e73-a1624c952b09'), name)
        (driver / f'{name}.c').write_text(f'#define UMI_PROBE_STAGE {stage}\n#include "UmiDisplayProbe.c"\n')
        (driver / f'{name}.inf').write_text(f'''[Defines]
  INF_VERSION = 0x0001001B
  BASE_NAME = {name}
  FILE_GUID = {guid}
  MODULE_TYPE = DXE_DRIVER
  VERSION_STRING = 1.0
  ENTRY_POINT = UmiDisplayProbeEntry
[Sources]
  {name}.c
[Packages]
  MdePkg/MdePkg.dec
[LibraryClasses]
  UefiDriverEntryPoint
  UefiBootServicesTableLib
  MemoryAllocationLib
  BaseMemoryLib
  IoLib
[Guids]
  gEfiEndOfDxeEventGroupGuid
  gEfiEventReadyToBootGuid
[Depex]
  TRUE
''')
        path = f'umiPkg/Drivers/UmiDisplayProbeDxe/{name}.inf'
        marker = f'  INF {path}\n'
        assert apriori.count(inf(anchor)) == 1
        apriori = apriori.replace(inf(anchor), marker + inf(anchor) if before else inf(anchor) + marker)
        dxe += marker
        dsc += f'\n[Components]\n  {path}\n'
(pkg / 'Include/APRIORI.inc').write_text(apriori)
(pkg / 'Include/DXE.inc').write_text(dxe)
(pkg / 'umi.dsc').write_text(dsc)
