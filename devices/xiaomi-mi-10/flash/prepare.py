#!/usr/bin/env python3
"""Called through build.sh (build-lock). Artifacts are confined to out/.../flash.

prepare.py [images|loader|loader-prod|userland|all] (default all)
Existing completed raw images/data are kept. Remove only this script's own output
if regeneration is intended. userland builds in a new, private source snapshot.
"""
import argparse
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import time

from layout import BS, BLOCKS, DATA_FIRST, SLOTS

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
OUT = ROOT / 'out/xiaomi-mi-10/flash'


def run(*args, **kwargs):
    print('+', ' '.join(map(str, args)), flush=True)
    subprocess.run(list(map(str, args)), check=True, **kwargs)


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def digest(path):
    with path.open('rb') as f:
        return hashlib.file_digest(f, 'sha256').hexdigest()


def record():
    names = ('rootfs.apfs', 'cryptex-os.apfs', 'cryptex-app.apfs', 'data.simg', 'loader.efi', 'loader-prod.efi',
             'flashboot.apfs', 'root-ram-data-test.apfs', 'root-flash-data-test.apfs',
             'root-flash-ui.apfs', 'root-flash-ui-ca.apfs')
    files = {}
    for name in names:
        path = OUT / name
        if path.exists():
            files[name] = {'bytes': path.stat().st_size, 'sha256': digest(path)}
    manifest = {'block_size': BS, 'darwin_blocks': BLOCKS, 'data_first': DATA_FIRST,
                'slots': {k: {'block': v[0], 'blocks': v[1]} for k, v in SLOTS.items()}, 'files': files}
    (OUT / 'manifest.json').write_text(json.dumps(manifest, indent=2) + '\n')
    (OUT / 'SHA256SUMS').write_text(''.join(f'{v["sha256"]}  {k}\n' for k, v in files.items()))


def apfs_ok(path, slot):
    blocks = SLOTS[slot][1]
    with path.open('rb') as f:
        b = f.read(BS)
    if path.stat().st_size != blocks * BS or b[32:36] != b'NXSB' or struct.unpack_from('<IQ', b, 36) != (BS, blocks):
        raise RuntimeError(f'{path}: wrong APFS magic/size for {slot}')


def images():
    sys.path.insert(0, str(ROOT / 'tools/apfs'))
    import dmg
    for source, target, slot in (('rootfs.dmg', 'rootfs.apfs', 'root'),
                                 ('cryptex-os.dmg', 'cryptex-os.apfs', 'os'),
                                 ('cryptex-app.dmg', 'cryptex-app.apfs', 'app')):
        src, dst = ROOT / 'sysimage' / source, OUT / target
        if not dst.exists():
            part = OUT / (target + '.part')
            with src.open('rb') as f:
                raw = f.read(36)[32:] == b'NXSB'
            if raw:
                shutil.copyfile(src, part)
            else:
                reader = dmg.Dmg(str(src))
                parts = [p for p in dmg.read_gpt(reader) if p.type_guid == dmg.APFS_PART_TYPE]
                if len(parts) != 1:
                    raise RuntimeError('expected exactly one APFS partition')
                p = parts[0]
                if p.length != SLOTS[slot][1] * BS:
                    raise RuntimeError('wrong source APFS length')
                print(f'extract full {source} APFS: {p.length} bytes at DMG offset {p.offset}', flush=True)
                reader.extract(str(part), p.offset, p.length)
            apfs_ok(part, slot)
            part.rename(dst)
        apfs_ok(dst, slot)
    if not (OUT / 'data.simg').exists():
        # Upstream sparse algorithm, umi geometry. No fastboot; writer consumes RAW/FILL only.
        mk = load('mkdatavol', ROOT / 'tools/mkdatavol.py')
        mk.DARWIN_BLOCKS, mk.DATA_FIRST = BLOCKS, DATA_FIRST
        old_argv = sys.argv
        formatter = load('umi_hfs_builder', HERE / 'build-hfs.py').build()
        import tempfile
        old_dir = tempfile.tempdir
        try:
            tempfile.tempdir = str(OUT)
            sys.argv = ['mkdatavol.py', str(OUT / 'data.simg.part'), str(formatter)]
            mk.main()
            writer = load('umi_writer', HERE / 'darwin-write-umi.py')
            writer.image_plan(OUT / 'data.simg.part', 0)
            (OUT / 'data.simg.part').rename(OUT / 'data.simg')
        finally:
            sys.argv = old_argv
            tempfile.tempdir = old_dir
    writer = load('umi_writer', HERE / 'darwin-write-umi.py')
    writer.image_plan(OUT / 'data.simg', 0)


def loader(prod=False):
    stem = 'loader-prod' if prod else 'loader'
    # Deliberately do not invoke build-loader.sh: that would replace the other agent's loader/header.
    for name in ('identity.h', 'owner_key.h'):
        shutil.copyfile(ROOT / 'out/xiaomi-mi-10' / name, OUT / name)
    run(sys.executable, ROOT / 'devices/xiaomi-mi-10/tools/darwinid.py', 'header', HERE / 'umi.darwin', OUT / 'darwin_identity.h')
    run('clang', '--target=aarch64-unknown-windows', '-ffreestanding', '-fno-builtin', '-fno-stack-protector',
        '-fshort-wchar', '-mgeneral-regs-only', '-O1', '-Wall', '-Wextra', '-Wno-unused-parameter', '-DLOADER_UMI',
        *(['-DHAKIOS_LOADER_PROD'] if prod else []),
        '-I' + str(OUT), '-I' + str(ROOT / 'devices/xiaomi-mi-10/loader'),
        '-c', ROOT / 'devices/xiaomi-mi-10/loader/loader.c', '-o', OUT / (stem + '.obj'))
    run('lld-link', '/subsystem:efi_application', '/entry:efi_main', '/nodefaultlib', '/base:0xA1000000',
        *(['/timestamp:0'] if prod else []),
        '/out:' + str(OUT / (stem + '.efi')), OUT / (stem + '.obj'))


def userland():
    ca = os.environ.get('FLASH_CA') == '1'
    if ca and os.environ.get('METAL_PLUGIN') != 'ca':
        raise RuntimeError('FLASH_CA=1 requires METAL_PLUGIN=ca')
    if ca and not (OUT / 'flashboot.apfs').is_file():
        raise RuntimeError('CA-only payload requires the existing flashboot.apfs')
    build = OUT / ('build-' + time.strftime('%Y%m%d-%H%M%S'))
    build.mkdir()
    shutil.copytree(ROOT / 'userland', build / 'userland',
                    ignore=shutil.ignore_patterns('obj', 'root', 'sysroot', 'flashboot', 'flashboot.os', '*.apfs', '__pycache__'))
    shutil.copytree(ROOT / 'devices/xiaomi-mi-10/overlays/userland', build / 'userland', dirs_exist_ok=True)
    run('patch', '--batch', '--fuzz=0', '-p1', '-i', HERE / 'flash-init.patch', cwd=build / 'userland')
    for name in ('xnu-env.sh', 'xnu-deps', 'tools', 'prebuilt', 'sysimage', 'devices', 'wifi', 'hakios-metal',
                 'fw-gpu', 'fw-audio', 'fw-haptic', 'fw-wifi', 'fw-bluetooth', 'fw-camera', 'fw-baseband', 'jb'):
        if (ROOT / name).exists():
            (build / name).symlink_to(ROOT / name)
    script = build / 'userland/build.sh'
    source = script.read_text()
    # Only this private snapshot is changed. Neither build nor trim a system.apfs.
    boundary = '\n# --- the system image '
    if source.count(boundary) != 1:
        raise RuntimeError('userland system-image boundary changed')
    source = source[:source.index(boundary)] + '\n'
    big_loop = 'for f in $(cd root && find lib bin -type f -size +4M 2>/dev/null); do\n'
    if source.count(big_loop) != 1:
        raise RuntimeError('userland large-library relocation changed')
    # The flash root has no /usr/local/redwood. Keep these libraries in md1 for
    # FLASHBOOT; with the current kernel this still fits the 63 MiB payload limit.
    source = source.replace(big_loop, big_loop + '\t[ -n "$FLASHBOOT" ] && continue\n')
    script.write_text(source)
    env = os.environ.copy()
    # Install and pre-spawn signature/trust failures use os_log. mkjobs enables
    # OS_ACTIVITY_DT_MODE for these labels, verified in the payload LaunchDaemons.
    env.update(DEVICE='umi', BLUETOOTH='1', GL=os.environ.get('GL', '1'), REST='1', ADSP='',
               CRYPTEXES='flash', PREBUILT='', CHECK='', SYSTEM='',
               OSLOG=os.environ.get('OSLOG', 'com.apple.backboardd com.apple.mobile.installd '
                   'com.apple.mobile_installation_proxy com.apple.streaming_zip_conduit '
                   'com.apple.SpringBoard com.apple.runningboardd com.apple.lsd com.apple.lsd.system '
                   'com.apple.CommCenter com.apple.CommCenterMobileHelper'),
               HAKIOS_METAL=str(ROOT / 'hakios-metal'))
    # GL=1 is allowed only with the ready Mesa artifacts; this task does not rebuild shared dependencies.
    if env['GL']:
        mesa = ROOT / 'xnu-deps/build/mesa/out'
        expected = subprocess.check_output(['bash', '-c',
            'cat tools/mesa/*.patch tools/mesa/hakios-drm/* | sha256sum'], cwd=ROOT, text=True).strip()
        if not (mesa / 'built-from').exists() or (mesa / 'built-from').read_text().strip() != expected:
            raise RuntimeError('Mesa is not ready; use GL= ./build.sh userland for storage diagnostics')
    # Compile the file-only HFS test using the same SDK/toolchain as userland.
    run(ROOT / 'xnu-deps/host/bin/xnu-clang', '-arch', 'arm64', '-isysroot', Path(env['THEOS']) / 'sdks/iPhoneOS16.5.sdk',
        '-O1', '-Wall', '-Wextra', '-o', OUT / 'data-test', HERE / 'data-test.c', env=env)
    env['EXTRA'] = str(OUT / 'data-test')
    (build / 'source-sha256.json').write_text(json.dumps({
        str(p.relative_to(build)): digest(p) for p in (build / 'userland').rglob('*') if p.is_file()
    }, indent=2) + '\n')
    targets = ((('root-flash-ui-ca.apfs', '1', False),) if ca else
               (('root-ram-data-test.apfs', '', True),
                ('root-flash-data-test.apfs', '1', True),
                ('root-flash-ui.apfs', '1', False)))
    for target, flash, diagnostic in targets:
        env.update(FLASHBOOT=flash, RC=str(HERE / 'rc.data-test') if diagnostic else 'rc.launchd',
                   RC_JOBS='rc.jobs')
        run('bash', script, env=env)
        shutil.copyfile(build / 'userland/root.apfs', OUT / target)
    if ca:
        # USB NCM: /redwood/bin/usb-network gives en2 172.16.42.1/24 and starts sshd (only with
        # usb=1 usb_ncm=1). Without an address, inet never attaches to en2 and every frame is ENOENT.
        tools = ROOT / 'devices/xiaomi-mi-10/tools'
        run(sys.executable, tools / 'fetch-usb-ssh.py', '--out', OUT / 'usb-ssh')
        run(sys.executable, tools / 'build-usb-ca.py', '--base', OUT / 'root-flash-ui-ca.apfs',
            '--packages', OUT / 'usb-ssh/extract', '--keys', ROOT.parents[1] / 'ssh', '--out', build / 'usb-ca')
        shutil.copyfile(build / 'usb-ca/root-flash-ui-ca.apfs', OUT / 'root-flash-ui-ca.apfs')
    # The CA plug-in and metallib live in the payload root. flashboot.apfs only
    # carries stage0, firmware and cryptexes, so the existing flashboot remains
    # valid and is deliberately left untouched by a CA-only rebuild.
    if not ca:
        shutil.copyfile(build / 'userland/flashboot.apfs', OUT / 'flashboot.apfs')
    kernel = ROOT / 'out/xiaomi-mi-10/xnu/BUILD/obj/DEVELOPMENT_ARM64_BCM2837/mach.development.bcm2837'
    if kernel.exists():
        for name in ('root-flash-data-test.apfs', 'root-flash-ui.apfs', 'root-flash-ui-ca.apfs'):
            if not (OUT / name).exists():
                continue
            if kernel.stat().st_size + (OUT / name).stat().st_size + 4096 > 63 * 1024 * 1024:
                raise RuntimeError('kernel + flash payload exceeds loader limit')
    (OUT / ('userland-ca-build-path' if ca else 'userland-build-path')).write_text(str(build) + '\n')


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('action', nargs='?', choices=('images', 'loader', 'loader-prod', 'userland', 'all'), default='all')
    args = p.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    if args.action == 'loader-prod':
        loader(prod=True)
    for name, fn in (('images', images), ('loader', loader), ('userland', userland)):
        if args.action in (name, 'all'):
            fn()
    record()
    print('Artifacts and checksums:', OUT, flush=True)


if __name__ == '__main__':
    main()
