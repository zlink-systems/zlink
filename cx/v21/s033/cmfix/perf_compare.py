import csv
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import statistics
import subprocess
import sys

base = Path('/root/jobs/fx/cx/v21/s033/cmfix')
raw = base / 'raw'
repo = Path('/root/worktree/zlink-4118-multipart')
bench = repo / 'bindings/cpp/perf/single/build/cpp_perf_router_router'
libs = {
    'main': base / 'core-baseline-matched/lib',
    'change': base / 'core-build/lib',
}

def available_kib():
    for row in Path('/proc/meminfo').read_text().splitlines():
        if row.startswith('MemAvailable:'):
            return int(row.split()[1])
    raise RuntimeError('MemAvailable is absent')

def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()

metadata = {
    'started_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
    'bench': str(bench),
    'bench_sha256': sha(bench),
    'command': [str(bench), 'cpp', 'inproc', '1024'],
    'environment': {
        'PERF_PART_COUNT': '2',
        'PERF_SINGLE_DURATION_SECONDS': '5',
        'PERF_CTX_AUTO_HWM_ENABLE': '0',
        'PERF_SINGLE_ALLOW_MANUAL_SOCKET_OVERRIDES': '1',
        'PERF_SINGLE_HWM': '8388608',
    },
    'runtime_sha256': {
        key: sha(path / 'libzlink.so.1.23.0') for key, path in libs.items()
    },
}
if metadata['runtime_sha256']['main'] == metadata['runtime_sha256']['change']:
    raise RuntimeError('Comparison runtimes are identical')
(raw / 'perf-metadata.json').write_text(json.dumps(metadata, indent=2) + '\n')
rows = []
for pair in range(1, 7):
    order = ['main', 'change'] if pair % 2 else ['change', 'main']
    for variant in order:
        memory = available_kib()
        if memory < 12 * 1024 * 1024:
            raise RuntimeError(f'MemAvailable below 12 GiB: {memory} KiB')
        env = os.environ.copy()
        env.update(metadata['environment'])
        env['LD_LIBRARY_PATH'] = str(libs[variant])
        linkage = subprocess.check_output(['ldd', str(bench)], env=env, text=True)
        (raw / f'perf-pair-{pair}-{variant}.ldd.log').write_text(linkage)
        if str(libs[variant]) not in linkage:
            raise RuntimeError(f'Wrong runtime for {variant}')
        path = raw / f'perf-pair-{pair}-{variant}.log'
        with path.open('w') as stream:
            process = subprocess.Popen(metadata['command'], cwd=repo, env=env,
                                       stdout=stream, stderr=subprocess.STDOUT)
            try:
                result = process.wait(timeout=45)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait()
                raise
        if result:
            raise RuntimeError(f'{variant} pair {pair} failed: {result}')
        matches = []
        for row in csv.reader(path.read_text().splitlines()):
            if len(row) >= 7 and row[0] == 'RESULT' and row[5] == 'throughput':
                matches.append(float(row[6]))
        if len(matches) != 1 or matches[0] <= 0:
            raise RuntimeError(f'Invalid throughput: {path}')
        rows.append({'pair': pair, 'variant': variant,
                     'throughput': matches[0], 'mem_available_kib': memory})
        print(f'pair={pair} variant={variant} throughput={matches[0]:.3f}', flush=True)
        (raw / 'perf-results.json').write_text(json.dumps(rows, indent=2) + '\n')
ratios = []
for pair in range(1, 7):
    values = {r['variant']: r['throughput'] for r in rows if r['pair'] == pair}
    ratios.append(values['change'] / values['main'])
summary = {
    'main_median': statistics.median(r['throughput'] for r in rows if r['variant'] == 'main'),
    'change_median': statistics.median(r['throughput'] for r in rows if r['variant'] == 'change'),
    'paired_ratios': ratios,
    'paired_geomean_ratio': math.exp(statistics.mean(math.log(r) for r in ratios)),
    'paired_median_ratio': statistics.median(ratios),
}
(raw / 'perf-summary.json').write_text(json.dumps(summary, indent=2) + '\n')
print(json.dumps(summary), flush=True)
