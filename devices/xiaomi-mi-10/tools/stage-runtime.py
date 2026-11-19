#!/usr/bin/env python3
"""Select UMI artifacts in a generated copy of xl, leaving upstream scripts intact."""
from pathlib import Path
import os

DEVICE = Path(__file__).resolve().parents[1]
ROOT = DEVICE.parents[1]
OUT = ROOT / 'out/xiaomi-mi-10'
sandbox = OUT / 'runtime'
sandbox.mkdir(parents=True, exist_ok=True)
source = (ROOT / 'xl').read_text()
# Upstream renamed the kernel artifact and changed its fallback block. Replace
# the complete artifact-selection block, and fail rather than boot a prebuilt.
start = source.index('\nK=') + 1
end = source.index('mkdir -p logs', start)
source = source[:start] + '''K=${XL_KERNEL:?UMI kernel path required}
L=${XL_LOADER:?UMI loader path required}
[ -f "$K" ] || { echo "Build the UMI kernel first" >&2; exit 1; }
[ -f "$L" ] || { echo "Build the UMI loader first" >&2; exit 1; }
''' + source[end:]
# Use the existing host's phone lock across old and new UMI workspaces.
source = source.replace('/darwin-hakios-phone', '/hackios-phone')
# Kernel USB takeover invalidates fastboot after oem run. Keep all normal runs
# byte-for-byte on the upstream path; the opt-in run follows the EP0 log ABI.
marker = '\tlocal live=; live_loader && live=1\n\tfb $1 >/dev/null\n'
if source.count(marker) != 1:
    raise SystemExit('xl live-run boundary changed')
usb_tool = os.environ.get('XL_USB_LOG_TOOL', str(DEVICE / 'tools/usb-log.py'))
if not Path(usb_tool).is_file() or any(c in usb_tool for c in '\"$`\n'):
    raise SystemExit('invalid USB collector path')
source = source.replace(marker, '''\tlocal umi_usb=
\tcase " ${XL_CMDLINE-} " in
\t*" usb=1 "*)
\t\tpython3 "''' + usb_tool + '''" --check || return 1
\t\tfb getvar umi-usb-handoff | grep -Eq 'umi-usb-handoff: 2([[:space:]]|$)' || {
\t\t\techo 'xl: usb=1 requires the USB ABI2 quiescence/deadline loader'; return 1;
\t\t}
\t\tumi_usb=1 ;;
\tesac
''' + marker + '''\tif [ -n "$umi_usb" ]; then
\t\techo 'xl: usb=1: loader -> kernel EP0 log; autonomous Android reset at SECS (+20 s fallback)'
\t\tpython3 "''' + usb_tool + '''" --seconds "$(( ${XL_SECS:-8} + 30 ))" --log "$log"
\t\tlocal usb_rc=$?
\t\tln -sf "$(basename "$log")" logs/last.log
\t\treturn "$usb_rc"
\tfi
''')
(sandbox / 'xl').write_text(source)
(sandbox / 'xl').chmod(0o755)
for name, target in {'xnuloader': ROOT / 'xnuloader', 'prebuilt': ROOT / 'prebuilt',
                     'mkxnuimg.py': DEVICE / 'tools/mkimage.py',
                     'tools': ROOT / 'tools',
                     'VERSION': ROOT / 'VERSION',
                     '.config': OUT / 'umi.config',
                     'userland': OUT / 'userland-build/userland'}.items():
    path = sandbox / name
    if not path.exists() and not path.is_symlink():
        path.symlink_to(target, target_is_directory=target.is_dir())
# Match the old per-phone Wi-Fi address input without embedding it in source.
mac = ROOT / 'fw-wifi/mac'
if mac.exists():
    address = mac.read_text().strip()
    fields = address.split(':')
    if len(fields) != 6 or any(len(x) != 2 or any(c not in '0123456789abcdefABCDEF' for c in x) for x in fields):
        raise SystemExit('Invalid private Wi-Fi MAC input')
    # run.sh exports XL_CMDLINE before staging. Keep direct callers reproducible
    # by falling back to the device's checked-in defaults.
    arguments = os.environ.get('XL_CMDLINE', (DEVICE / 'boot.args').read_text())
    if 'wifi=1' in arguments and 'wifi_mac=' not in arguments:
        # xl is generated locally; the address file remains outside git.
        # The caller supplies this through a private runtime file read by run.sh.
        (OUT / 'wifi-boot.args').write_text('wifi_mac=' + address + '\n')
