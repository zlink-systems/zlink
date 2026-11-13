#!/usr/bin/env python3
import json
from pathlib import Path

JOB = Path('/root/jobs/fx/cx/v21/s033/lad-dotnet-s1')
RAW = JOB / 'raw'
OUT = Path('/root/jobs/fx/cx/v21/s033/lad-dotnet-s1/report.md')
CELLS = [
    'send-saturation-1024', 'send-saturation-4096',
    'request-serial-1024', 'request-serial-4096',
    'request-backpressure-1024', 'request-backpressure-4096',
]
LEVELS = ['L0', 'L1', 'L2', 'L3', 'L4', 'L5_7']
IMPL = {level: ('zlink-framework-dotnet' if level == 'L5_7' else 'zlink-dotnet') for level in LEVELS}
ATTEMPTS = {
    ('L0', 'send-saturation-1024'): [1],
    ('L0', 'send-saturation-4096'): [2],
    ('L0', 'request-serial-1024'): [2],
    ('L1', 'send-saturation-4096'): [1, 2, 3],
    ('L1', 'request-serial-1024'): [1, 2, 3],
    ('L2', 'send-saturation-4096'): [1, 2, 3],
    ('L2', 'request-serial-1024'): [1, 2, 3],
    ('L4', 'send-saturation-4096'): [1, 2, 3],
    ('L5_7', 'send-saturation-4096'): [1, 2, 3],
}

def mask_ok(value, low, high):
    try:
        for part in value.split(','):
            nums = [int(x) for x in part.split('-')]
            if len(nums) not in (1, 2): return False
            a, b = nums[0], nums[-1]
            if not low <= a <= b <= high: return False
        return bool(value)
    except Exception:
        return False

def validate(result, sched, source):
    cell = json.loads(result.read_text())['cells'][0]
    assert all(cell.get(k, 0) == 0 for k in ('errors', 'server_errors')), result
    assert cell['target_stats']['errors'] == 0, result
    assert cell['submitted'] == cell['completed'] == cell['target_stats']['received'], result
    txt = sched.read_text()
    assert 'scheduling_invalid=' not in txt and 'affinity_invalid=' not in txt, sched
    lines = txt.splitlines()
    for phase, low, high in (('source_start', 0, 5), ('mid_active_source', 0, 5),
                             ('target_start', 8, 13), ('mid_active_target', 8, 13)):
        tids = [i for i, line in enumerate(lines) if line.startswith(phase + '_tid=')]
        affs = [line.rsplit('cpus=', 1)[1] for line in lines if line.startswith(phase + '_affinity ') and 'cpus=' in line]
        assert tids and len(tids) == len(affs), (phase, sched)
        assert all(mask_ok(v, low, high) for v in affs), (phase, affs)
        assert all('current scheduling policy: SCHED_OTHER' in '\n'.join(lines[i:i+3]) for i in tids), (phase, sched)
    return float(cell['throughput_per_second'])

def run_value(level, cell):
    impl = IMPL[level]
    entries = []
    for attempt in ATTEMPTS.get((level, cell), [1]):
        result = RAW / level / cell / 'counted' / f'attempt{attempt}' / 'run1' / f'{impl}-{cell}' / 'results.json'
        sched = result.with_name('scheduling.txt')
        value = validate(result, sched, level)
        entries.append((value, result, sched))
    return entries

binding_file = Path('/root/jobs/fx/cx/v21/s033/binding-daily/dotnet-2026-11-13b.json')
binding = json.loads(binding_file.read_text())['cells']
for cell in CELLS:
    p = Path(binding[cell]['raw'])
    validate(p, Path(binding[cell]['scheduling']), 'binding')

values = {level: {cell: run_value(level, cell) for cell in CELLS} for level in LEVELS}
means = {level: {cell: sum(x[0] for x in values[level][cell]) / len(values[level][cell]) for cell in CELLS} for level in LEVELS}

def pathlink(path, label):
    return f'[{label}](<{path}>)'

def rate_cell(level, cell):
    entries = values[level][cell]
    if len(entries) == 1:
        v, result, sched = entries[0]
        return f'{v:,.1f} {pathlink(result, "raw")} {pathlink(sched, "sched")}'
    refs = '; '.join(f'{v:,.1f} {pathlink(result, f"r{i}")} {pathlink(sched, f"s{i}")}' for i, (v, result, sched) in enumerate(entries, 1))
    return f'**{means[level][cell]:,.1f} avg** ({refs})'

def binding_cell(cell):
    return f'{binding[cell]["msg_s"]:,.1f} {pathlink(binding[cell]["raw"], "raw")} {pathlink(binding[cell]["scheduling"], "sched")}'

label = {
    'send-saturation-1024': 'send-saturation · 1K',
    'send-saturation-4096': 'send-saturation · 4K',
    'request-serial-1024': 'request-serial · 1K',
    'request-serial-4096': 'request-serial · 4K',
    'request-backpressure-1024': 'request-backpressure · 1K',
    'request-backpressure-4096': 'request-backpressure · 4K',
}
lines = [
    '# .NET ladder — 측정 결과',
    '',
    '- 측정: 2026-11-13 UTC, tip `29d9fca82b`; warmup 10초, 측정 5초, 셀별 warm 1회 폐기 후 counted 1회. 경계 셀은 총 3회 평균.',
    '- 조건: C2·C3 off, SCHED_OTHER, source 0–5 / target 8–13, guard v8. counted 결과는 errors=0 및 submitted=completed=received, 모든 스레드의 시작/중간 정책·affinity를 확인.',
    '- L4 send 1K의 최초 warm은 source SCHED_IDLE로 무효 폐기 후 재실행. L0 send 4K 무효 attempt1과 serial 1K 중단 attempt1은 제외했고, send 1K의 추가 유효 attempt2도 요청된 단일 측정표에서 제외했다. 원시 기록은 [progress](<' + str(JOB / 'progress.md') + '>).',
    '',
    '## A. 처리량 (msg/s)',
    '',
    '| 셀 | binding | L0 | L1 | L2 | L3 | L4 | L5~L7 | Framework/binding |',
    '|---|---:|---:|---:|---:|---:|---:|---:|---:|',
]
for cell in CELLS:
    ratio = means['L5_7'][cell] / float(binding[cell]['msg_s'])
    lines.append('| ' + ' | '.join([
        label[cell], binding_cell(cell),
        *(rate_cell(level, cell) for level in LEVELS),
        f'{ratio:.3f}',
    ]) + ' |')
lines += [
    '',
    '## B. 단계별 손실 및 기준 판정',
    '',
    '- 손실 = `(이전 단계 − 현재 단계) / 이전 단계`; 음수는 처리량 증가. B의 각 셀은 A 표 같은 행의 이전/현재 단계 값을 비교하며, 그 값의 results.json과 scheduling.txt를 A 표에서 연다.',
    '- 기준: L0/binding ≥0.95, L1~L4 손실 ≤7%, L5~L7 손실 ≤21%. 5–9% 경계(L1/L2)와 19–23% 예산 경계(L4/L5~L7)는 각 관련 단계·셀을 3회 실행해 평균.',
    '',
    '| 셀 | L0/binding (≥.95) | L1/L0 (≤7%) | L2/L1 (≤7%) | L3/L2 (≤7%) | L4/L3 (≤7%) | L5~L7/L4 (≤21%) |',
    '|---|---:|---:|---:|---:|---:|---:|',
]
for cell in CELLS:
    l0ratio = means['L0'][cell] / float(binding[cell]['msg_s'])
    pass_l0 = l0ratio >= 0.95
    cols = [label[cell], f'{l0ratio:.3f} {"PASS" if pass_l0 else "FAIL"}']
    prev = 'L0'
    for level, threshold in [('L1', 7), ('L2', 7), ('L3', 7), ('L4', 7), ('L5_7', 21)]:
        loss = (means[prev][cell] - means[level][cell]) / means[prev][cell] * 100
        verdict = 'PASS' if loss <= threshold else 'FAIL'
        cols.append(f'{loss:+.1f}% {verdict}')
        prev = level
    lines.append('| ' + ' | '.join(cols) + ' |')
lines += [
    '',
    '반복 평균의 모든 원시 경로는 A 표의 `r1–r3` 링크로, 대응 스케줄 증거는 `s1–s3` 링크로 연결했다. 전체 단계별 warm/count, artifact hash, HEAD/dirty 및 무효 실행 이력은 [progress.md](<' + str(JOB / 'progress.md') + '>); 호출 경로 비교는 [call-shape.md](<' + str(JOB / 'call-shape.md') + '>).',
    '',
]
OUT.write_text('\n'.join(lines))
print(OUT)
