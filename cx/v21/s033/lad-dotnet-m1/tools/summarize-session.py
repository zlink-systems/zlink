#!/usr/bin/env python3
import json
import statistics
from datetime import datetime, timezone
from pathlib import Path

JOB = Path('/root/jobs/fx/cx/v21/s033/lad-dotnet-m1')
ROOT = JOB / 'raw'
DAILY = Path('/root/jobs/fx/cx/v21/s033/binding-daily') / f'dotnet-{datetime.now(timezone.utc):%Y-%m-%d}.json'
CELLS = (
    ('send-saturation', 1024), ('send-saturation', 4096),
    ('request-serial', 1024), ('request-serial', 4096),
    ('request-backpressure', 1024), ('request-backpressure', 4096),
)

def result_path(label, pattern, size, run=1):
    impl = 'zlink-dotnet' if label == 'binding' else 'zlink-framework-dotnet'
    if run == 1:
        base = ROOT / label / f'{pattern}-{size}'
    else:
        base = ROOT / f'{label}-request-serial-4096-confirm' / f'run{run}'
    path = base / 'run1' / f'{impl}-{pattern}-{size}' / 'results.json'
    if Path(str(path) + '.scheduling-invalid').exists():
        retry = ROOT / f'{label}-retry' / f'{pattern}-{size}'
        candidates = sorted(retry.glob(f'attempt*/run1/{impl}-{pattern}-{size}/results.json'))
        valid = [candidate for candidate in candidates if not Path(str(candidate) + '.scheduling-invalid').exists()]
        if valid:
            return valid[-1]
    return path

def read_result(path):
    cell = json.loads(path.read_text())['cells'][0]
    assert cell['errors'] == 0 and cell['server_errors'] == 0 and cell['target_stats']['errors'] == 0
    assert cell['submitted'] == cell['completed'] == cell['target_stats']['received']
    return cell

def rate(path):
    return read_result(path)['throughput_per_second']

if not DAILY.exists():
    cells = {}
    for pattern, size in CELLS:
        path = result_path('binding', pattern, size)
        cells[f'{pattern}-{size}'] = {
            'msg_s': rate(path),
            'raw': str(path),
        }
    DAILY.parent.mkdir(parents=True, exist_ok=True)
    DAILY.write_text(json.dumps({'date_utc': datetime.now(timezone.utc).strftime('%Y-%m-%d'), 'cells': cells}, indent=2) + '\n')

binding = json.loads(DAILY.read_text())['cells']
rows = []
for pattern, size in CELLS:
    key = f'{pattern}-{size}'
    b = binding[key].get('msg_s', binding[key].get('msg_per_second'))
    vals = {}
    paths = {}
    for label in ('L0', 'L1', 'L2'):
        p = result_path(label, pattern, size)
        vals[label] = [rate(p)]
        paths[label] = [str(p)]
    if pattern == 'request-serial' and size == 4096:
        for label in ('L0', 'L1'):
            for run in (2, 3):
                p = ROOT / f'{label}-request-serial-4096-confirm' / f'run{run}' / 'run1' / f'zlink-framework-dotnet-{pattern}-{size}' / 'results.json'
                if Path(str(p) + '.scheduling-invalid').exists():
                    candidates = sorted((ROOT / f'{label}-confirm-retry' / f'{pattern}-{size}').glob('attempt*/run1/zlink-framework-dotnet-request-serial-4096/results.json'))
                    valid = [candidate for candidate in candidates if not Path(str(candidate) + '.scheduling-invalid').exists()]
                    if valid:
                        p = valid[-1]
                vals[label].append(rate(p))
                paths[label].append(str(p))
    med = {label: statistics.median(values) for label, values in vals.items()}
    ratio = {label: med[label] / b for label in med}
    loss10 = 1 - med['L1'] / med['L0']
    # L0/L1 serial-4K confirmation reuses L1's extra runs only for that pair;
    # L2/L1 remains the single-run comparison from the same base hold.
    loss21 = 1 - vals['L2'][0] / vals['L1'][0]
    spreads = {
        label: (max(abs(value - med[label]) for value in values) / med[label] if len(values) > 1 else None)
        for label, values in vals.items()
    }
    rows.append({'pattern': pattern, 'size': size, 'binding': b, 'values': vals, 'median': med,
                 'ratio': ratio, 'loss10': loss10, 'loss21': loss21, 'spreads': spreads,
                 'paths': paths, 'binding_raw': binding[key].get('raw') or binding[key].get('raw_paths')})

lines = [
    '# .NET L0/L1/L2 측정', '',
    '- 실험 브랜치 `exp/4350-dotnet-ladder`; L0 header parity fix `ab21f1eb31`, L2 envelope header `2590b2a030`, scheduler 기록 `bfc15a8dd6` 로컬 커밋.',
    '- L0 raw header byte parity, L1 permit, L2 production envelope 호출 형태: [call-shape.md](call-shape.md).',
    f'- Binding 기준: `{DAILY}`. 각 cell은 같은 날 1회 측정이며 raw 경로는 아래 표에 적었다.',
    '- Hold: `raw/lock-windows/session-before.txt`–`session-after.txt`; 각 raw run의 `.scheduling.txt`에 시작/중간 thread 정책을 기록한다. 유효 조건: 전 thread SCHED_OTHER, errors=0, submitted=completed=received.', '',
    '| 셀 | binding msg/s | L0 msg/s · /binding | L1 msg/s · L1/L0 손실 | L2 msg/s · L2/L1 손실 | G5 (3회 셀) | 판정 | raw |',
    '|---|---:|---:|---:|---:|---|---|---|'
]
for r in rows:
    pct = lambda x: f'{x:.1%}'
    g5 = '—'
    if len(r['values']['L0']) == 3:
        g5 = ' / '.join(f"{key}: {r['spreads'][key]:.1%}" for key in ('L0', 'L1'))
    verdict = []
    verdict.append('L0 통과' if r['ratio']['L0'] >= .95 else 'L0 미달')
    for level, loss in (('L1/L0', r['loss10']), ('L2/L1', r['loss21'])):
        if len(r['values']['L0']) == 3 and max(r['spreads']['L0'], r['spreads']['L1']) > .10:
            verdict.append(f'{level} 판정 불가(G5)')
        elif loss <= .07:
            verdict.append(f'{level} 통과')
        else:
            verdict.append(f'{level} 손실 초과')
    l0paths = ', '.join('`'+p.replace(str(JOB)+'/', '')+'`' for p in r['paths']['L0'])
    l1paths = ', '.join('`'+p.replace(str(JOB)+'/', '')+'`' for p in r['paths']['L1'])
    l2paths = ', '.join('`'+p.replace(str(JOB)+'/', '')+'`' for p in r['paths']['L2'])
    binding_path = binding[f"{r['pattern']}-{r['size']}"].get('raw', 'binding-daily')
    lines.append(
        f"| {r['pattern']} {r['size']//1024}K | {r['binding']:,.1f} | {r['median']['L0']:,.1f} · {r['ratio']['L0']:.4f} | "
        f"{r['median']['L1']:,.1f} · {pct(r['loss10'])} | {r['median']['L2']:,.1f} · {pct(r['loss21'])} | {g5} | "
        f"{' · '.join(verdict)} | binding `{binding_path}`; L0 {l0paths}; L1 {l1paths}; L2 {l2paths} |"
    )
invalid_runs = sorted(JOB.glob('raw/**/*.scheduling-invalid'))
lines += ['', '## 스케줄 무효 run', '']
if invalid_runs:
    lines.extend(f'- 재실행 후 제외: `{path.relative_to(JOB)}`' for path in invalid_runs)
else:
    lines.append('- SCHED_OTHER가 아닌 thread가 없어 무효 run은 없었다; per-run 정책은 위 result의 `.scheduling.txt` sidecar에 기록했다.')
lines += ['', '## 결과별 판단', '']
for r in rows:
    lines.append(f"- {r['pattern']} {r['size']//1024}K: L0/binding {r['ratio']['L0']:.4f}; L1/L0 손실 {r['loss10']:.1%}; L2/L1 손실 {r['loss21']:.1%}.")
    if r['loss10'] > .07 or r['loss21'] > .07:
        lines.append('  - call-shape는 [call-shape.md](call-shape.md)에서 production encode/decode 경로와 per-message 차이를 대조했다. 제품 runtime은 변경하지 않았다.')
(JOB / 'report.md').write_text('\n'.join(lines) + '\n')
print(JOB / 'report.md')
print(DAILY)
