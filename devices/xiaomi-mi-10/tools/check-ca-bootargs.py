#!/usr/bin/env python3
"""Host-only CA boot line budget; includes xl's additions and UMI loader fields."""
import argparse
from pathlib import Path

DEFAULT = 'bootcpu=7 cpus=7 autounlock=0 ufs=2 ufs_native=0 ufs_write=1 root_apfs=ufs_darwin var_hfs=ufs_darwin_data umi_touch=1 umi_keys=1 umi_haptic=5 adsp=1 adsp_umi=1 sound=4 gpu=1 metal=1 metal_fps=1 fb_scan=1 fb_threads=6 fb_filter=0 cpufreq=1 wifi=1 umi_bt=1 torch=3 battery=1 umi_mic=4 touch_lat=1'
PREFIX = 'debug=0x8 rd=md0 ufs_shared_pa=0x0000000000000000 battery_shared_pa=0x0000000000000000 hakios_kver=2.3.4-hakios-darwin22.5-sm8250-g63fc2b46-dirty loader_return_after=600 epoch=1794212215 sb_delay=8 '

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('args', nargs='?', default=DEFAULT)
    p.add_argument('--prefix', default=PREFIX, help='override if the loader/kernel version or xl options change')
    a = p.parse_args()
    root = Path(__file__).resolve().parents[3]
    wifi = root / 'out/xiaomi-mi-10/wifi-boot.args'
    suffix = ' ' + wifi.read_text().strip() if 'wifi=1' in a.args.split() and 'wifi_mac=' not in a.args and wifi.exists() else ''
    # phone-test.sh passes "$ARGS $EXTRA" with a trailing space when EXTRA is empty.
    line = a.prefix + a.args.strip() + ' ' + suffix
    size = len(line.encode('ascii'))
    print(line)
    print(f'{size}/607 bytes; {607-size} bytes spare (excluding terminating NUL)')
    if size > 607:
        p.error('kernel boot arguments would be truncated')

if __name__ == '__main__':
    main()
