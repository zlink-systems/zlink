#!/usr/bin/env python3
"""Independently check raw counts, controls, provenance and derived verdicts."""
import datetime
import hashlib
import json
from pathlib import Path
import statistics
import subprocess

JOB = Path('/root/jobs/fx/cx/v21/s033/l3868-l1')
REPO = Path('/root/worktree/zlink-v21-s033/l3868')
RAW = JOB / 'raw'


def main():
    summary = json.loads((RAW / 'summary.json').read_text())
    checked = []
    all_outliers = []
    seal = json.loads((RAW / 'build-provenance.json').read_text())
    assert not seal['dirty']
    assert seal['commit'] == subprocess.check_output(['git', '-C', str(REPO), 'rev-parse', 'HEAD'], text=True).strip()
    for path in sorted((RAW / 'matrix').glob('*/*/run*/*/results.json')):
        value = json.loads(path.read_text()); c = value['cells'][0]
        marker = json.loads((path.parent / 'validated.json').read_text())
        assert hashlib.sha256(path.read_bytes()).hexdigest() == marker['result_sha256']
        assert c['submitted'] == c['completed'] == c['target_stats']['received']
        assert c['errors'] == c['target_stats']['errors'] == 0
        assert not c.get('contaminated') and not c.get('drain_bound_hit')
        assert not c.get('abandoned') and not c.get('header_validation_failures')
        assert c['trigger']['durationMs'] == 5000 and c['trigger']['warmup'] == 30000
        assert value['metadata']['sourceCpus'] == '0-7'
        assert value['metadata']['targetCpus'] == '8-15'
        variant = path.relative_to(RAW).parts[1]
        assert value['metadata']['ablationLadderLevel'] == (1 if variant == 'L1' else 0)
        provenance = json.loads((path.parent / 'binary-provenance.json').read_text())
        assert provenance['commit'] == seal['commit'] and not provenance['dirty']
        assert provenance['binaries'] == seal['binaries']
        assert (path.parent / 'reply-parity.md').is_file()
        checked.append(str(path.relative_to(JOB)))
    for c in summary['cells']:
        medians = {}
        for variant, data in c['variants'].items():
            records = data['runs']
            values = [json.loads((JOB / r['path']).read_text())['cells'][0]['throughput_per_second'] for r in records]
            median = statistics.median(values)
            spread = max(abs(v - median) / median for v in values)
            assert median == data['median'] and abs(spread - data['spread']) < 1e-12
            assert data['g5'] == ('pass' if spread <= .10 else 'fail')
            n = len(values)
            assert n in (3, 5)
            if spread > .10 or (c['size'] == 4096 and c['pattern'] in ('send-saturation', 'request-serial')):
                assert n == 5
            if n == 3:
                assert spread <= .10
            medians[variant] = median
            all_outliers += [dict(pattern=c['pattern'], size=c['size'], variant=variant, **r) for r in data['outliers']]
        assert len({len(v['runs']) for v in c['variants'].values()}) == 1
        assert abs(c['L0_ratio'] - medians['L0'] / medians['binding']) < 1e-12
        assert abs(c['L1_ratio'] - medians['L1'] / medians['binding']) < 1e-12
        assert abs(c['step_loss'] - (1 - medians['L1'] / medians['L0'])) < 1e-12
        valid0 = all(c['variants'][k]['g5'] == 'pass' for k in ('binding', 'L0'))
        valid1 = all(v['g5'] == 'pass' for v in c['variants'].values())
        assert c['L0_verdict'] == ('판정 불가' if not valid0 else '통과' if c['L0_ratio'] >= .95 else '미달')
        assert c['L1_verdict'] == ('판정 불가' if not valid1 else '통과' if c['step_loss'] <= .07 else '미달')
    result = {'utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
              'commit': seal['commit'], 'checked_results': checked,
              'cells_completed': len(summary['cells']), 'all_six_complete': len(summary['cells']) == 6,
              'outliers': all_outliers, 'audit': 'pass'}
    (RAW / 'final-audit.json').write_text(json.dumps(result, indent=2) + '\n')
    print(json.dumps({k: v for k, v in result.items() if k != 'checked_results'}))


if __name__ == '__main__':
    main()
