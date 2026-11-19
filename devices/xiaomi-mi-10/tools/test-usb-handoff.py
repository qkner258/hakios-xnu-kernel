#!/usr/bin/env python3
"""Compile actual loader handoff logic with fake CPU registers/USB/cache ops."""
from pathlib import Path
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
pinned = Path(__file__).resolve().parent / 'sources/loader.c'
s = (pinned if pinned.is_file() else root / 'loader/loader.c').read_text()
a = s.index('static void\nfirmware_watch_only(')
b = s.index('static int64_t\nrun_on_other_cpu(', a)
watch = s[a:b]
watch = watch.replace('__asm__ volatile ("msr daifset, #0xf" ::: "memory");', 'mask_daif();')
watch = watch.replace('__asm__ volatile ("msr S3_0_C12_C12_7, %0\\nisb" : : "r" ((uint64_t)0) : "memory");', 'mask_group1();')
assert '__asm__' not in watch
a = s.index('static void\nusb_handoff_poll(')
b = s.index('static const uint8_t usb_dev_desc', a)
handoff = s[a:b]
# Ensure the reset call precedes IRQ restoration in the real run-return path.
run = s[s.index('static int64_t\nrun_on_other_cpu('):s.index('/* Hypervisor watchdog')]
run_tail = run[run.index('\trunning_live = NULL;'):]
assert run_tail.index('if (usb_kernel_requested) usb_kernel_reset();') < run_tail.index('if (watching_only)')
assert '0x32425355494d55ULL' in s
source = r'''
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdio.h>
typedef uint64_t efi_status;
#define EFI_ERROR_BIT (1ULL << 63)
#define EFI_ERROR(s) ((s) & EFI_ERROR_BIT)
#define print(...) ((void)0)
static struct { uint64_t magic; volatile uint64_t request, ack; } usb_handoff;
static uint64_t cpu_daif, cpu_group1, stop_result;
static unsigned stops, publications;
static void mask_daif(void) { assert(usb_handoff.ack == 0); cpu_daif = 0x3c0; }
static void mask_group1(void) { assert(cpu_daif == 0x3c0 && usb_handoff.ack == 0); cpu_group1 = 0; }
#define READ_SYSREG(name) (!strcmp(#name, "DAIF") ? cpu_daif : cpu_group1)
static void flush_dcache(uint64_t a, uint64_t b) { (void)a; (void)b; }
static void clean_dcache(uint64_t a, uint64_t b) {
    (void)a; (void)b; ++publications;
    if (usb_handoff.ack == 1) assert(cpu_daif == 0x3c0 && cpu_group1 == 0 && stops == 1);
}
static efi_status stop(void) { ++stops; assert(usb_handoff.ack == 0); return stop_result; }
static struct { efi_status (*stop)(void); } protocol = { stop }, *USB = &protocol;
''' + watch + handoff + r'''
int main(void) {
    uint64_t saved_daif = 0, saved_group = 0;
    int watching = 0;
    cpu_daif = 0x80; cpu_group1 = 1;
    usb_handoff_poll(&saved_daif, &saved_group, &watching);
    assert(stops == 0 && publications == 0 && !watching);
    usb_handoff.request = 1;
    usb_handoff_poll(&saved_daif, &saved_group, &watching);
    assert(usb_handoff.ack == 1 && watching && saved_daif == 0x80 && saved_group == 1);
    usb_handoff_poll(&saved_daif, &saved_group, &watching);
    assert(stops == 1 && publications == 1); /* no repeated Stop */
    usb_handoff.ack = 0; stop_result = EFI_ERROR_BIT | 7;
    watching = 0; cpu_daif = 0x80; cpu_group1 = 1;
    usb_handoff_poll(&saved_daif, &saved_group, &watching);
    assert(usb_handoff.ack == 2 && !watching && cpu_daif == 0x80 && cpu_group1 == 1);
    usb_handoff.ack = 0; USB = NULL;
    usb_handoff_poll(&saved_daif, &saved_group, &watching);
    assert(usb_handoff.ack == 2 && stops == 2 && !watching);
    puts("PASS: actual loader handoff Stop/quiesce/ACK ordering, failure, idempotence, reset-before-restore (simulated)");
}
'''
with tempfile.TemporaryDirectory(prefix='umi-usb-handoff-') as tmp:
    p = Path(tmp)
    (p/'test.c').write_text(source)
    subprocess.run(['clang', '-std=c11', '-Wall', '-Wextra', '-fsanitize=address,undefined', str(p/'test.c'), '-o', str(p/'test')], check=True)
    subprocess.run([str(p/'test')], check=True)
