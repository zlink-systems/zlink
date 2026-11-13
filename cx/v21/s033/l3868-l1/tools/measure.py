#!/usr/bin/env python3
"""One lock session: interleave binding/L0/L1, preserve every original run."""
import datetime
import json
import os
from pathlib import Path
import statistics
import subprocess
import sys
import time

JOB = Path('/root/jobs/fx/cx/v21/s033/l3868-l1')
REPO = Path('/root/worktree/zlink-v21-s033/l3868')
RAW = JOB / 'raw'
DEADLINE = int(os.environ['MEASURE_DEADLINE_EPOCH'])
VARIANTS = ('binding', 'L0', 'L1')
SUMMARY = RAW / 'summary.json'


def utc():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write_json(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2) + '\n')


def note(message):
    with (JOB / 'progress.md').open('a') as output:
        output.write(f'\n- {utc()} {message}\n')
    print(message, flush=True)


def run_variant(variant, pattern, size, run, profile=False):
    if time.time() >= DEADLINE:
        raise TimeoutError('five-hour task cap reached')
    implementation = 'zlink-cpp' if variant == 'binding' else 'zlink-framework-cpp'
    level = '1' if variant == 'L1' else '0'
    base = RAW / ('profiles' if profile else 'matrix') / variant / f'{pattern}-{size}'
    env = dict(os.environ, ZLINK_CPP_LADDER_LEVEL=level,
               IMPLEMENTATIONS=implementation, PATTERNS=pattern, PAYLOAD_SIZES=str(size),
               OUTPUT_DIR=str(base), RUN_START=str(run), RUNS=str(run),
               RUN_STAMP=f'4351-{variant}-{pattern}-{size}')
    base.mkdir(parents=True, exist_ok=True)
    runner = 'profile-runner.sh' if profile else 'cpp-cell-runner.sh'
    with (base / f'run{run}-driver.log').open('w') as output:
        subprocess.run([str(JOB / 'tools' / runner)], env=env, stdout=output,
                       stderr=subprocess.STDOUT, check=True,
                       timeout=max(1, DEADLINE - time.time()))


def aggregate(pattern, size, suffix=''):
    result = {}
    for level in ('L0', 'L1'):
        out = RAW / 'aggregates' / f'{level}-{pattern}-{size}{suffix}.json'
        out.parent.mkdir(exist_ok=True)
        command = [sys.executable, str(REPO / 'framework/bench/grpc/tools/bench_aggregate.py'),
                   '--lang', 'cpp', '--min-runs', '3', '--baseline', 'zlink-cpp',
                   '--runs-glob', str(RAW / 'matrix/binding' / f'{pattern}-{size}' / 'run*'),
                   '--runs-glob', str(RAW / 'matrix' / level / f'{pattern}-{size}' / 'run*'),
                   '--json-out', str(out)]
        with out.with_suffix('.md').open('w') as output:
            subprocess.run(command, check=True, stdout=output, stderr=subprocess.STDOUT)
        rows = json.loads(out.read_text())['rows']
        for row in rows:
            key = 'binding' if row['implementation'] == 'zlink-cpp' else level
            result[key] = row
    return result


def summarize_cell(pattern, size, aggregates):
    cell = {'pattern': pattern, 'size': size, 'variants': {}}
    for variant in VARIANTS:
        records = []
        for path in sorted((RAW / 'matrix' / variant / f'{pattern}-{size}').glob('run*/*/results.json')):
            marker = json.loads((path.parent / 'validated.json').read_text())
            data = json.loads(path.read_text())['cells'][0]
            provenance = json.loads((path.parent / 'binary-provenance.json').read_text())
            assert marker['errors'] == data['errors'] == data['target_stats']['errors'] == 0
            assert data['submitted'] == data['completed'] == data['target_stats']['received']
            records.append({'path': str(path.relative_to(JOB)),
                            'msg_s': data['throughput_per_second'],
                            'start_utc': (path.parent / 'run-start.txt').read_text().strip()})
        median = statistics.median(r['msg_s'] for r in records)
        spread = max(abs(r['msg_s'] - median) / median for r in records)
        row = aggregates[variant]
        assert abs(median - row['median']['throughput_per_second']) < 1e-6
        assert abs(100 * spread - row['g5_spread_percent']) < 1e-8
        cell['variants'][variant] = {'runs': records, 'median': median,
                                    'spread': spread, 'g5': row['g5_status'],
                                    'outliers': [r for r in records if abs(r['msg_s'] - median) / median > .10]}
    v = cell['variants']
    assert len({len(v[k]['runs']) for k in VARIANTS}) == 1
    cell['L0_ratio'] = v['L0']['median'] / v['binding']['median']
    cell['L1_ratio'] = v['L1']['median'] / v['binding']['median']
    cell['step_loss'] = 1 - cell['L1_ratio'] / cell['L0_ratio']
    cell['L0_verdict'] = ('판정 불가' if any(v[k]['g5'] != 'pass' for k in ('binding', 'L0'))
                          else '통과' if cell['L0_ratio'] >= .95 else '미달')
    cell['L1_verdict'] = ('판정 불가' if any(v[k]['g5'] != 'pass' for k in VARIANTS)
                          else '통과' if cell['step_loss'] <= .07 else '미달')
    write_json(RAW / 'cells' / f'{pattern}-{size}.json', cell)
    note(f'{pattern} {size}: binding={v["binding"]["median"]:,.0f}, '
         f'L0={v["L0"]["median"]:,.0f}, L1={v["L1"]["median"]:,.0f}; '
         f'L0/binding={cell["L0_ratio"]:.4f} ({cell["L0_verdict"]}), '
         f'L1 step loss={100 * cell["step_loss"]:.2f}% ({cell["L1_verdict"]}); '
         f'[raw](raw/cells/{pattern}-{size}.json).')
    return cell


def profiles(cells):
    for cell in cells:
        if cell['step_loss'] <= .07:
            continue
        pattern, size = cell['pattern'], cell['size']
        note(f'{pattern} {size}: L1 step loss >7%; binding/L0/L1 paired perf 수집. 구현 변경 없음.')
        for variant in VARIANTS:
            run_variant(variant, pattern, size, 1, profile=True)
            capture = next((RAW / 'profiles' / variant / f'{pattern}-{size}').glob('run1/*/capture'))
            source, target = (capture.parent / 'capture-pids.txt').read_text().split()
            for name, args in [
                ('target-self.txt', ['--no-children', '--pid', target, '--sort', 'comm,dso,symbol']),
                ('target-stacks.txt', ['--children', '--pid', target, '--call-graph', 'graph,0.5,caller']),
                ('source-self.txt', ['--no-children', '--pid', source, '--sort', 'comm,dso,symbol']),
            ]:
                with (capture / name).open('w') as output, (capture / (name + '.stderr')).open('w') as err:
                    subprocess.run(['perf', 'report', '--stdio', '-i', str(capture / 'perf.data'), *args],
                                   check=True, stdout=output, stderr=err)


def main():
    cells = []
    write_json(RAW / 'session-start.json', {'utc': utc(), 'pid': os.getpid(), 'deadline': DEADLINE})
    try:
        for pattern in ('send-saturation', 'request-serial', 'request-backpressure'):
            for size in (1024, 4096):
                runs = 5 if size == 4096 and pattern in ('request-serial', 'send-saturation') else 3
                for run in range(1, runs + 1):
                    for variant in VARIANTS:
                        run_variant(variant, pattern, size, run)
                rows = aggregate(pattern, size)
                if runs == 3 and any(row['g5_status'] == 'fail' for row in rows.values()):
                    aggregate(pattern, size, '-first3')
                    note(f'{pattern} {size}: G5 실패; binding/L0/L1 모두 원래 실행을 보존하고 run4/5 추가.')
                    for run in (4, 5):
                        for variant in VARIANTS:
                            run_variant(variant, pattern, size, run)
                    rows = aggregate(pattern, size)
                cells.append(summarize_cell(pattern, size, rows))
                write_json(SUMMARY, {'updated_utc': utc(), 'cells': cells})
        profiles(cells)
        subprocess.run([sys.executable, str(JOB / "tools/audit.py")], check=True)
    finally:
        write_json(RAW / 'session-end.json', {'utc': utc(), 'cells_completed': len(cells)})


if __name__ == '__main__':
    main()
