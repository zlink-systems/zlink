#!/usr/bin/env python3
import json, pathlib, statistics
import re

job = pathlib.Path('/root/jobs/fx/cx/v21/s033/lad-node-m2')
base = job / 'raw/session/confirm'
impl = {'binding': 'zlink-node', 'L0': 'zlink-framework-node', 'L0R': 'zlink-framework-node', 'L1': 'zlink-framework-node'}
cells = [('send-saturation', 4096), ('request-serial', 1024), ('request-serial', 4096)]

def load(variant, pattern, payload, run):
    if variant == 'L0' and pattern == 'send-saturation' and payload == 4096:
        progress = (job / 'progress.md').read_text().splitlines()
        prefix = f'First result L0 send-saturation-4096 r{run}:'
        line_no = next(i + 1 for i, line in enumerate(progress) if line.startswith(prefix))
        line = next(line for line in progress if line.startswith(prefix))
        match = re.search(r': ([0-9.]+) msg/s; errors=0 submitted=completed=received=([0-9]+)\.', line)
        if not match:
            raise SystemExit(f'cannot recover pre-overwrite summary for run {run}')
        return {'throughput': float(match.group(1)), 'path': 'progress.md', 'line': line_no,
                'submitted': int(match.group(2)), 'raw_overwritten_by_paired_l0': True}
    path = base / f'{pattern}-{payload}' / variant / f'run{run}' / 'run1' / f'{impl[variant]}-{pattern}-{payload}/results.json'
    result = json.loads(path.read_text())['cells'][0]
    if result['errors'] or result['server_errors'] or result['target_stats']['errors'] or result['submitted'] != result['completed'] or result['submitted'] != result['target_stats']['received']:
        raise SystemExit(f'broken result: {path}')
    return {'throughput': result['throughput_per_second'], 'path': path.relative_to(job).as_posix(), 'submitted': result['submitted']}

def load_paired_l0(pattern, payload, run):
    path = base / f'{pattern}-{payload}' / 'L0' / f'run{run}' / 'run1' / f'{impl["L0"]}-{pattern}-{payload}/results.json'
    result = json.loads(path.read_text())['cells'][0]
    if result['errors'] or result['server_errors'] or result['target_stats']['errors'] or result['submitted'] != result['completed'] or result['submitted'] != result['target_stats']['received']:
        raise SystemExit(f'broken result: {path}')
    return {'throughput': result['throughput_per_second'], 'path': path.relative_to(job).as_posix(), 'submitted': result['submitted']}

rows = []
for pattern, payload in cells:
    variants = ['L0', 'L0R', 'L1'] + (['binding'] if pattern == 'send-saturation' else [])
    samples = {v: [load(v, pattern, payload, i) for i in (1, 2, 3)] for v in variants}
    medians = {v: statistics.median(x['throughput'] for x in a) for v, a in samples.items()}
    spreads = {v: max(abs(x['throughput'] - medians[v]) for x in a) / medians[v] for v, a in samples.items()}
    row = {'pattern': pattern, 'payload': payload, 'samples': samples, 'medians': medians, 'g5': spreads,
           'module_loss': 1 - medians['L0R'] / medians['L0'],
           'permit_loss': 1 - medians['L1'] / medians['L0R'],
           'step_loss': 1 - medians['L1'] / medians['L0'],
           'permit_profile_needed': 1 - medians['L1'] / medians['L0'] > .07,
           'module_profile_needed': pattern == 'request-serial' and payload == 4096 and 1 - medians['L0R'] / medians['L0'] > .02}
    if 'binding' in medians:
        pair_l0 = [load_paired_l0(pattern, payload, i) for i in (1, 2, 3)]
        pair_median = statistics.median(x['throughput'] for x in pair_l0)
        row['binding_pair'] = {'L0': pair_l0, 'L0_median': pair_median,
                               'L0_g5': max(abs(x['throughput'] - pair_median) for x in pair_l0) / pair_median,
                               'ratio': pair_median / medians['binding']}
    rows.append(row)

out = job / 'raw/aggregates/confirm-4351.json'
out.parent.mkdir(parents=True, exist_ok=True)
out.write_text(json.dumps({'rows': rows}, indent=2) + '\n')
for row in rows:
    g5 = ','.join('{}:{:.2%}'.format(v, row['g5'][v]) for v in row['g5'])
    print(row['pattern'], row['payload'], 'L0/L0R/L1=', *(f"{row['medians'][v]:.1f}" for v in ('L0','L0R','L1')),
          f"module={row['module_loss']:.2%}", f"permit={row['permit_loss']:.2%}", f"step={row['step_loss']:.2%}",
          f"G5={g5}",
          f"binding={row.get('binding_pair', {}).get('ratio', 0):.4f}")
print(out)
