#!/usr/bin/env python3
"""Offline host test of the real loader's config-table decoder; no MMIO or device I/O."""
import os
import subprocess
import tempfile
from pathlib import Path

device = Path(__file__).resolve().parents[1]
source = (device / 'loader/loader.c').read_text()
start = source.index('static void\numi_display_probe_dump(void)')
end = source.index('static void\numi_display_clock_snapshot', start)
decoder = source[start:end]
harness = r'''
#include <assert.h>
#include <stdarg.h>
#include <string.h>
#include "efi.h"
#include "UmiDisplayProbe.h"
static efi_system_table system_table, *ST = &system_table;
static int platform_umi, *PLAT = &platform_umi;
static int absent, invalid, headers, records;
static const char *expected_stage;
static void print(const char *format, ...) {
    va_list ap;
    va_start(ap, format);
    if (strstr(format, "absent")) ++absent;
    else if (strstr(format, "invalid")) ++invalid;
    else if (strstr(format, "version=")) {
        assert(va_arg(ap, uint64_t) == 1);
        assert(va_arg(ap, uint64_t) == 1);
        assert(va_arg(ap, uint64_t) == 0);
        assert(va_arg(ap, uint64_t) == 0x8000000000000013ULL);
        ++headers;
    } else {
        assert(!strcmp(va_arg(ap, const char *), expected_stage));
        for (unsigned i = 0; i < UMI_DISPLAY_PROBE_REGS; ++i)
            assert(va_arg(ap, uint64_t) == 0x80000001u + i);
        ++records;
    }
    va_end(ap);
}
'''
harness += decoder
harness += r'''
int main(void) {
    _Static_assert(sizeof(UMI_DISPLAY_PROBE_RECORD) == 40, "record ABI");
    _Static_assert(offsetof(UMI_DISPLAY_PROBE_TABLE, Records) == 24, "table ABI");
    _Static_assert(sizeof(UMI_DISPLAY_PROBE_TABLE) == 1304, "table size");
    umi_display_probe_dump();
    assert(absent == 1);
    UMI_DISPLAY_PROBE_TABLE table = {0};
    typeof(*ST->tables) entry = {UMI_DISPLAY_PROBE_GUID, &table};
    ST->tables = &entry; ST->nr_tables = 1;
    umi_display_probe_dump(); assert(invalid == 1);
    table.Magic = UMI_DISPLAY_PROBE_MAGIC;
    table.Version = UMI_DISPLAY_PROBE_VERSION;
    table.Count = UMI_DISPLAY_PROBE_CAPACITY + 1;
    umi_display_probe_dump(); assert(invalid == 2);
    table.Count = 1; table.VoteStatus = 0x8000000000000013ULL;
    table.Records[0].Stage = 7;
    for (unsigned i = 0; i < UMI_DISPLAY_PROBE_REGS; ++i)
        table.Records[0].Gcc[i] = 0x80000001u + i;
    expected_stage = "after-ufs";
    umi_display_probe_dump(); assert(records == 1 && headers == 1);
    table.Records[0].Stage = 1000;
    expected_stage = "unknown";
    umi_display_probe_dump(); assert(records == 2 && headers == 2);
    PLAT = NULL;
    umi_display_probe_dump(); assert(records == 2 && headers == 2);
    return 0;
}
'''
with tempfile.TemporaryDirectory(dir=os.environ['TMPDIR']) as tmp:
    p = Path(tmp)
    (p / 'test.c').write_text(harness)
    subprocess.run(['clang', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                    '-I' + str(device / 'loader'),
                    '-I' + str(device / 'uefi/files/Platforms/Xiaomi/umiPkg/Drivers/UmiDisplayProbeDxe'),
                    str(p / 'test.c'), '-o', str(p / 'test')], check=True)
    subprocess.run([str(p / 'test')], check=True)
print('Probe ABI/loader decoder: absent, invalid, bounds, values, stage names, platform gate passed')
