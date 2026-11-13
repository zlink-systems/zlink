#!/usr/bin/env python3
import datetime
import json
import os
import pathlib
import sys

path = pathlib.Path(sys.argv[1])
result = json.loads(path.read_text())
cell = result['cells'][0]
assert not cell.get('contaminated'), (str(path), 'contaminated')
assert not cell.get('drain_bound_hit'), (str(path), 'drain bound')
assert cell['errors'] == cell['target_stats']['errors'] == 0, str(path)
assert cell['submitted'] == cell['completed'] == cell['target_stats']['received'], (str(path), cell)
assert cell.get('header_validation_failures', 0) == 0, str(path)
assert cell.get('abandoned', 0) == 0, str(path)
assert cell['trigger']['durationMs'] == 5000, str(path)
assert cell['trigger']['warmup'] == 30000, str(path)
print(json.dumps({'raw': str(path), 'msg_s': cell['throughput_per_second'],
                  'submitted': cell['submitted'], 'completed': cell['completed'],
                  'received': cell['target_stats']['received'], 'errors': 0}))
progress = pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868-l0e/progress.md')
with progress.open('a') as handle:
    label='진단 profile' if '/profiles/' in str(path) else 'fresh 유효 실행'
    handle.write(f"\n- {datetime.datetime.now(datetime.timezone.utc).isoformat()} {label}: [{cell['implementation']} {cell['pattern']} {cell['payload_size']} · {cell['throughput_per_second']:,.0f} msg/s]({path}); errors=0, submitted=completed=received={cell['completed']}.\n")
if (os.environ.get('ZLINK_CPP_LADDER_LEVEL') == '0'
        and cell['implementation'] == 'zlink-framework-cpp'
        and cell['pattern'] == 'send-saturation' and cell['payload_size'] == 1024
        and '첫 유효 실행' not in progress.read_text()):
    binding = path.parent.parent / 'zlink-cpp-send-saturation-1024/results.json'
    bind = json.loads(binding.read_text())['cells'][0]
    with progress.open('a') as handle:
        handle.write('\n# C++ L0 첫 측정\n\n'
                       '조건 B: source CPU 0–7, target CPU 8–15, warmup 30초, active 5초. 첫 유효 실행이며 중앙값은 후속 보고서에서 판정한다.\n\n'
                       '| L0 msg/s | Binding msg/s | L0÷binding |\n|---:|---:|---:|\n'
                       f"| [{cell['throughput_per_second']:,.0f}]({path}) | [{bind['throughput_per_second']:,.0f}]({binding}) | {cell['throughput_per_second']/bind['throughput_per_second']:.4f} |\n\n"
                       '양쪽 모두 errors=0, submitted=completed=received이다.\n')

# Only a fully checked result can enter a final partial aggregate.
import hashlib
marker = path.parent / 'validated.json'
verified = {'raw': str(path), 'result_sha256': hashlib.sha256(path.read_bytes()).hexdigest(),
            'checked_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
            'submitted': cell['submitted'], 'completed': cell['completed'],
            'received': cell['target_stats']['received'], 'errors': 0}
if marker.exists():
    assert json.loads(marker.read_text())['result_sha256'] == verified['result_sha256'], str(marker)
else:
    marker.write_text(json.dumps(verified, indent=2) + '\n')
