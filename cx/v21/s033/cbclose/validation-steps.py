from pathlib import Path
import json
import subprocess
import sys
import time

root = Path('/root/jobs/fx/cx/v21/s033/cbclose')
revision = sys.argv[1] if len(sys.argv) > 1 else 'after'
commands = [
    ('admission', [str(root / 'build/test_cpp_contract_submit_close_admission')]),
    ('races', [str(root / 'build/test_cpp_submit_close_race'), 'all', '1000']),
    ('full', ['ctest', '--test-dir', str(root / 'build'), '--output-on-failure', '-j3']),
]
results = []
for name, command in commands:
    started = time.time()
    with (root / f'{revision}-{name}.log').open('w') as log:
        completed = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    result = {'phase': name, 'command': command, 'exit': completed.returncode,
              'started_unix': started, 'seconds': time.time() - started}
    results.append(result)
    print(json.dumps(result), flush=True)
    (root / f'{revision}-validation.json').write_text(json.dumps(results, indent=2) + '\n')
    if completed.returncode:
        sys.exit(completed.returncode if completed.returncode > 0 else 128 - completed.returncode)
