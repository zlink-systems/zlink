#!/usr/bin/env python3
import json
import statistics
from pathlib import Path

JOB = Path('/root/jobs/fx/cx/v21/s033/lad-dotnet-l1b')
ROOT = JOB / 'raw' / 'session'
VARIANTS = ('binding', 'L0', 'L1')
CELLS = (
    ('send-saturation', 1024), ('send-saturation', 4096),
    ('request-serial', 1024), ('request-serial', 4096),
    ('request-backpressure', 1024), ('request-backpressure', 4096),
)

def path(v, pattern, size, run):
    if run <= 3:
        d = ROOT / v / f'run-{run}' / 'run1' / f'zlink-dotnet-{pattern}-{size}'
    else:
        d = ROOT / v / f'extra-{pattern}-{size}' / f'run-{run}' / 'run1' / f'zlink-dotnet-{pattern}-{size}'
    return d

def get(v, pattern, size, run):
    p = path(v, pattern, size, run) / 'results.json'
    d = json.loads(p.read_text())['cells'][0]
    valid = (d['errors'] == 0 and d['server_errors'] == 0 and
             d['submitted'] == d['completed'] == d['target_stats']['received'])
    return p, d, valid

def spread(values):
    med = statistics.median(values)
    return max(abs(x - med) for x in values) / med if med else float('inf')

report = ['# .NET ladder L1 측정', '', '## 호출 형태', '',
          'Binding/L0/L1은 공통 raw client API를 사용한다. L0는 고정 handler 호출만 복원하고 L1은 receive 전 permit 획득, handler 첫 명령 전 permit 반환을 더한다. 네 경로와 per-message 비용은 [call-shape.md](call-shape.md)에 적었다.', '',
          '## L0 fresh', '',
          '| 셀 | 반복 | L0 median msg/s | Binding median msg/s | L0/Binding | G5 spread L0/B · B | 판정 | Raw |',
          '|---|---:|---:|---:|---:|---:|---|---|']
aggregates = {}
for pattern, size in CELLS:
    n = 5 if pattern == 'request-serial' or all((path(v, pattern, size, 5) / 'results.json').exists() for v in VARIANTS) else 3
    series = {}
    refs = {}
    valid = True
    for v in VARIANTS:
        series[v] = []
        refs[v] = []
        for run in range(1, n + 1):
            p, d, ok = get(v, pattern, size, run)
            series[v].append(d['throughput_per_second'])
            refs[v].append(p)
            valid &= ok
    bmed = statistics.median(series['binding'])
    l0med = statistics.median(series['L0'])
    l1med = statistics.median(series['L1'])
    l0ratios = [a / b for a, b in zip(series['L0'], series['binding'])]
    l1ratios = [a / b for a, b in zip(series['L1'], series['binding'])]
    r0 = l0med / bmed
    r1 = l1med / bmed
    step = 1 - r1 / r0 if r0 else float('inf')
    s = {v: spread(series[v]) for v in VARIANTS}
    sr0, sr1 = spread(l0ratios), spread(l1ratios)
    if pattern == 'request-serial':
        l0_count, l1_count, b_count = 5, 5, 5
    else:
        l0_count = l1_count = b_count = n
    all_rate_spread = max(s.values())
    verdict0 = '판정 불가' if not valid or max(all_rate_spread, sr0) > .10 else ('통과' if r0 >= .95 else '미달')
    verdict1 = '판정 불가' if not valid or max(all_rate_spread, sr1, sr0) > .10 else ('통과' if step <= .07 else '손실 초과')
    rawref = f'`raw/session/{{binding,L0}}/run-{{1..3}}/run1/zlink-dotnet-{pattern}-{size}/results.json`'
    if n == 5:
        rawref += f'; `raw/session/{{binding,L0}}/extra-{pattern}-{size}/run-{{4,5}}/run1/zlink-dotnet-{pattern}-{size}/results.json`'
    report.append(f'| {pattern} {size // 1024} KiB | {n} | {l0med:,.1f} | {bmed:,.1f} | {r0:.4f} | {sr0:.1%} · {s["binding"]:.1%} | {verdict0} | {rawref} |')
    aggregates[f'{pattern}-{size}'] = {
        'n': n, 'median_msg_s': {v: statistics.median(series[v]) for v in VARIANTS},
        'spread': s, 'ratio_l0_binding': r0, 'ratio_l1_binding': r1,
        'ratio_spread_l0_binding': sr0, 'ratio_spread_l1_binding': sr1,
        'step_loss': step, 'valid': valid, 'verdict_l0': verdict0, 'verdict_l1': verdict1,
        'raw': {v: [str(p.relative_to(JOB)) for p in refs[v]] for v in VARIANTS},
    }
report += ['', '## L1 및 단계 손실', '',
           '| 셀 | 반복 | L1 median msg/s | Binding median msg/s | L1/Binding | L0/Binding | 단계 손실 | G5 spread L1/B · B | 판정 | Raw |',
           '|---|---:|---:|---:|---:|---:|---:|---:|---|---|']
for pattern, size in CELLS:
    a = aggregates[f'{pattern}-{size}']
    n = a['n']; l1 = a['median_msg_s']['L1']; b = a['median_msg_s']['binding']
    ref = f'`raw/session/{{binding,L0,L1}}/run-{{1..3}}/run1/zlink-dotnet-{pattern}-{size}/results.json`'
    if n == 5:
        ref += f'; `raw/session/{{binding,L0,L1}}/extra-{pattern}-{size}/run-{{4,5}}/run1/zlink-dotnet-{pattern}-{size}/results.json`'
    report.append(f'| {pattern} {size // 1024} KiB | {n} | {l1:,.1f} | {b:,.1f} | {a["ratio_l1_binding"]:.4f} | {a["ratio_l0_binding"]:.4f} | {a["step_loss"]:.1%} | {a["ratio_spread_l1_binding"]:.1%} · {a["spread"]["binding"]:.1%} | {a["verdict_l1"]} | {ref} |')

badserial = [k for k, a in aggregates.items() if k.startswith('request-serial') and a['verdict_l1'] == '판정 불가']
if badserial:
    report += ['', '## request-serial G5 판정 불가 근거', '',
               '다음은 각 5회 raw `results.json`의 generatedUtc, source/target CPU와 시작 스탬프다. `lock-windows`의 `*-before.ps.txt`/`*-after.ps.txt` 및 세션 before/after process snapshot에서 잠금 중 실행 중인 프로세스를 확인한다.', '',
               '| 변형/셀 | run | 시작 UTC | source CPU % | target CPU % | raw |', '|---|---:|---|---:|---:|---|']
    for k in badserial:
        pattern, size = k.rsplit('-', 1); size = int(size)
        for v in VARIANTS:
            for run in range(1, 6):
                p, d, _ = get(v, pattern, size, run)
                start = (path(v, pattern, size, run) / 'run-start.txt').read_text().strip()
                report.append(f'| {v} {size // 1024} KiB | {run} | {start} | {d["client_cpu_percent"]:.1f} | {d["server_cpu_percent"]:.1f} | `{p.relative_to(JOB)}` |')

report += ['', '## 측정 기록', '',
           '- Build: `b6f58168a6` Release; `progress.md`에 각 측정 전 commit/dirty/base, DLL 경로/hash와 receive/reply 대조 기록.',
           '- Lock window: `raw/lock-windows/session-before.txt`, `session-after.txt`; 모든 raw 결과는 보존.',
           '- Step loss: `1 − (L1/Binding)/(L0/Binding)`; 통과 한도 7%.']
(JOB / 'summary-data.json').write_text(json.dumps(aggregates, indent=2) + '\n')
(JOB / 'report.md').write_text('\n'.join(report) + '\n')
print(JOB / 'report.md')
