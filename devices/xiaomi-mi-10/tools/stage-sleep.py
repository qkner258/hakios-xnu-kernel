#!/usr/bin/env python3
"""Add small UMI sleep hooks to generated files, preserving other source overlays.

Supports fixed-frequency upstream and the concurrent 50 ms cpugov overlay.
Fail closed if an owner changes its API; do not silently omit final-write caps.
"""
import sys
from pathlib import Path


def once(text, old, new):
    if text.count(old) != 1:
        raise ValueError('UMI sleep: source anchor changed: ' + repr(old[:100]))
    return text.replace(old, new)


def idle_hooks(text):
    if '#include <arm64/umi_idle.h>' in text:
        if 'umi_idle_wait();' not in text:
            raise ValueError('UMI sleep: incomplete idle hooks')
        return text
    text = once(text, 'TUNABLE(unsigned int, wfi, "wfi", 1);',
                'TUNABLE(unsigned int, wfi, "wfi", 1);\n#include <arm64/umi_idle.h>')
    old = '#if HAS_RETENTION_STATE\n\t\tarm64_retention_wfi();\n#else\n\t\t__builtin_arm_wfi();\n#endif'
    return once(text, old, old.replace('__builtin_arm_wfi()', 'umi_idle_wait()'))


def cpufreq_hooks(text):
    if '#include "umi_cpufreq_screenoff.h"' in text:
        return text
    governor = 'static bool cpugov_enabled;' in text
    separate_cap = governor and 'static int epss_sleep_cap[' in text
    # Install after the owner's LUT/state declarations, before runtime writers.
    anchor = '\nvoid\nhakios_cpufreq_set_cap(' if governor else '\nvoid\nhakios_cpufreq_cap('
    text = once(text, anchor, '\n#define UMI_SLEEP_HAS_CPUGOV ' + str(int(governor)) +
                '\n#define UMI_SLEEP_HAS_EXTERNAL_CAP ' + str(int(separate_cap)) +
                '\n#include "umi_cpufreq_screenoff.h"\n' + anchor)
    start = text.index('\nhakios_cpufreq_cap(')
    end = text.index('\n}', start) + 2
    part = text[start:end]
    write = '\t*(volatile uint32_t *)(epss_base[domain] + EPSS_PERF_STATE) = (uint32_t)row;'
    if governor:
        part = once(part, '\tepss_cap[domain] = row;',
                    '\tlck_mtx_lock(&umi_freq_lock);\n\tepss_cap[domain] = row;\n\trow = umi_freq_limit(domain, row);')
        # Older concurrent drafts conditionally set epss_row; normalize the
        # accounting to the value actually written, without altering the request.
        part = once(part, write, '\tumi_freq_current[domain] = row;\n' + write + '\n\tlck_mtx_unlock(&umi_freq_lock);')
    else:
        part = once(part, write, '\tlck_mtx_lock(&umi_freq_lock);\n\tumi_freq_request[domain] = row;\n'
                    '\trow = umi_freq_limit(domain, row);\n\tumi_freq_current[domain] = row;\n' + write +
                    '\n\tlck_mtx_unlock(&umi_freq_lock);')
    text = text[:start] + part + text[end:]
    if governor:
        start = text.index('\nhakios_cpufreq_set_cap(')
        end = text.index('\n}', start) + 2
        part = text[start:end]
        if '\tepss_sleep_cap[domain] = row;' in part:
            part = once(part, '\tepss_sleep_cap[domain] = row;',
                        '\tlck_mtx_lock(&umi_freq_lock);\n\tepss_sleep_cap[domain] = row;')
            part = once(part, 'int effective = MIN(epss_cap[domain], row);',
                        'int effective = umi_freq_limit(domain, MIN(epss_cap[domain], row));')
            part = part[:-2] + '\n\tlck_mtx_unlock(&umi_freq_lock);\n}'
        else:
            part = once(part, '\tepss_cap[domain] = row;', '\tlck_mtx_lock(&umi_freq_lock);\n\tepss_cap[domain] = row;')
            part = once(part, '\t\tepss_row[domain] = row;', '\t\trow = umi_freq_limit(domain, row);\n\t\tepss_row[domain] = row;')
            part = part[:-2] + '\n\tlck_mtx_unlock(&umi_freq_lock);\n}'
        text = text[:start] + part + text[end:]
        text = once(text, '\t\t\trow = epss_row[d];', '\t\t\tlck_mtx_lock(&umi_freq_lock);\n\t\t\trow = epss_row[d];')
        text = once(text, '\t\t\tnext = MAX(next, 0);', '\t\t\tnext = MAX(next, 0);\n\t\t\tnext = umi_freq_limit(d, next);')
        anchor = '    epss_khz[d][next] / 1000, busy[d], boosted ? " boost" : "");\n\t\t\t\t}\n\t\t\t}'
        text = once(text, anchor, anchor + '\n\t\t\tlck_mtx_unlock(&umi_freq_lock);')
    return text


def stage(tree):
    for relative, transform in [('osfmk/arm64/cpu.c', idle_hooks), ('bsd/dev/hakios.cpufreq.c', cpufreq_hooks)]:
        path = tree / relative
        old = path.read_text()
        new = transform(old)
        if old != new:
            path.write_text(new)


if __name__ == '__main__':
    stage(Path(sys.argv[1]))
