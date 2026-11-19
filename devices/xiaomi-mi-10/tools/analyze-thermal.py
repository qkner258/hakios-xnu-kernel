#!/usr/bin/env python3
"""Summarize existing kernel logs; no device I/O. Rows are requests/caps, not HW residency."""
import argparse
import collections
import csv
import json
from pathlib import Path
import re
import statistics


def distribution(values):
    values = sorted(values)
    if not values:
        return None
    return dict(n=len(values), min=values[0], median=statistics.median(values),
                p95=values[min(len(values)-1, int(.95*len(values)))], max=values[-1])


def analyze(path):
    text = path.read_bytes().decode('utf-8', 'replace')
    # Preserve physical log line numbers despite control bytes in live USB output.
    lines = text.split('\n')
    cmd = next((line for line in lines if line.startswith('cmdline:') and 'bootcpu=' in line), '')
    mapping = {int(cpu): int(phys, 16) >> 8 for cpu, phys in
               re.findall(r'cpufreq: cpu(\d+) -> MPIDR (0x[\da-f]+)', text)}
    heat, temps, minute, gpu, draws, fps = [], [], [], [], collections.defaultdict(lambda: [0, 0]), []
    commit_cpu_ms = 0.0
    for number, line in enumerate(lines, 1):
        m = re.search(r'heat: rows (\d+)/(\d+) (\d+)/(\d+) (\d+)/(\d+) MHz (\d+)/(\d+)/(\d+) busy%\(phys\)=(.*)', line)
        if m:
            v = list(map(int, m.groups()[:9]))
            heat.append(dict(line=number, row=v[0:6:2], cap=v[1:6:2], mhz=v[6:9],
                             busy={int(c): int(b) for c, b in re.findall(r'(\d+):(\d+)', m[10])}))
        m = re.search(r'thermal:.*?hottest ([\w-]+) at (\d+\.\d+) C', line)
        if m:
            record = dict(line=number, sensor=m[1], c=float(m[2]))
            temps.append(record)
            if 'a minute:' in line:
                minute.append(record)
        m = re.search(r"GPU's sensors read .*?its clock is set to (\d+) MHz", line)
        if m:
            gpu.append(int(m[1]))
        if 'draw calls by processor:' in line and 'HakiosMetal[backboardd ' in line:
            for cpu, count, us in re.findall(r'cpu(\d+) (\d+) at (\d+) us', line):
                if int(cpu) not in mapping:
                    continue
                group = 'A55' if mapping[int(cpu)] < 4 else 'A77/prime'
                draws[group][0] += int(count)
                draws[group][1] += int(count)*int(us)
        if 'HakiosMetal[backboardd ' in line:
            m = re.search(r'commit-total=\d+/[\d.]+/([\d.]+)/', line)
            if m:
                commit_cpu_ms += float(m[1])
            m = re.search(r'fps: ([\d.]+) frames/s over ([\d.]+) s \((\d+) presented via panel', line)
            if m:
                fps.append((float(m[1]), float(m[2]), int(m[3])))
    total_draws = sum(v[0] for v in draws.values())
    summary = dict(log=path.name, bytes=path.stat().st_size, lines=len(lines),
                   metal_big=bool(re.search(r'\bmetal_big=1(?:\s|$)', cmd)),
                   default_governor='cpugov: enabled (default)' in text,
                   heat_samples=len(heat), cpu_mapping=mapping,
                   temp_reports=distribution([t['c'] for t in temps]),
                   minute_temps=distribution([t['c'] for t in minute]),
                   hottest=max(temps, key=lambda t:t['c']) if temps else None,
                   first_minute=minute[0] if minute else None, last_minute=minute[-1] if minute else None,
                   thermal_downsteps=max(map(int, re.findall(r'slowed (\d+) times', text)), default=0),
                   gpu_requested_mhz=dict(collections.Counter(gpu)),
                   wfi_records=text.count('umi-idle: cpu=')+text.count('heat-wfi:'),
                   tl_u_records=len(re.findall(r'tl: u at \d+', text)),
                   tl_lines=len(re.findall(r'tl: [a-zA-Z]', text)),
                   ddr_votes=list(map(int, re.findall(r'ddr: memory bandwidth vote (\d+) on MC0', text))),
                   backboard_draws={k:dict(calls=v[0], share_pct=round(100*v[0]/total_draws, 2),
                                         mean_draw_wall_us=round(v[1]/v[0], 2), accounted_draw_wall_s=round(v[1]/1e6, 3))
                                    for k, v in draws.items() if v[0]},
                   backboard_commit_cpu_s=round(commit_cpu_ms/1000, 3),
                   backboard_fps=distribution([v[0] for v in fps]),
                   backboard_active_fps=distribution([v[0] for v in fps if v[0]>=10]),
                   domains=[], cores={})
    for d in range(3):
        if not heat:
            break
        summary['domains'].append(dict(domain=d, mhz=distribution([v['mhz'][d] for v in heat]),
            requests_at_cap=sum(v['row'][d]==v['cap'][d] for v in heat),
            requests_at_row0=sum(v['row'][d]==0 for v in heat)))
    for core in sorted({c for v in heat for c in v['busy']}):
        values = [v['busy'][core] for v in heat if core in v['busy']]
        summary['cores'][core] = dict(mean_busy_pct=round(statistics.mean(values), 2),
                                     busy=distribution(values))
    return summary, heat


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('logs', type=Path, nargs='+')
    parser.add_argument('--out', type=Path, required=True)
    args = parser.parse_args()
    args.out.mkdir(parents=True, exist_ok=True)
    summaries = []
    for path in args.logs:
        summary, heat = analyze(path)
        summaries.append(summary)
        if heat:
            with (args.out/(path.stem+'.heat.csv')).open('w') as output:
                writer = csv.writer(output)
                writer.writerow(['line', 'row_small', 'row_big', 'row_prime', 'cap_small', 'cap_big', 'cap_prime',
                                 'mhz_small', 'mhz_big', 'mhz_prime']+['busy_phys'+str(c) for c in range(8)])
                for v in heat:
                    writer.writerow([v['line']]+v['row']+v['cap']+v['mhz']+[v['busy'].get(c, '') for c in range(8)])
    (args.out/'summary.json').write_text(json.dumps(summaries, indent=2)+'\n')
    for s in summaries:
        print(s['log'], 'metal_big='+str(s['metal_big']), 'heat='+str(s['heat_samples']),
              'thermal='+str(s['temp_reports']), 'draws='+str(s['backboard_draws']))


if __name__ == '__main__':
    main()
