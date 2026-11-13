import os, signal, subprocess, sys, time, json
from pathlib import Path
root = Path(__file__).parent
label = sys.argv[1]
assert int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:'))) >= 12 * 1024 * 1024
with (root/'raw'/f'{label}-stress.log').open('w') as stress_log, (root/'raw'/f'{label}.log').open('w') as test_log:
    load = subprocess.Popen(['stress-ng', '--cpu', '4', '--cpu-load', '70', '--cpu-method', 'matrixprod', '--metrics-brief'], stdout=stress_log, stderr=subprocess.STDOUT)
    try:
        result = subprocess.run(sys.argv[2:], stdout=test_log, stderr=subprocess.STDOUT)
    finally:
        load.send_signal(signal.SIGTERM)
        load.wait()
    (root/'raw'/f'{label}.status.json').write_text(json.dumps({'exit_code': result.returncode}))
    print(f'{label}: exit={result.returncode}; raw/{label}.log')
    sys.exit(result.returncode)
