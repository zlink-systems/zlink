import os, signal, subprocess, sys, time
from pathlib import Path
log = Path(sys.argv[1])
minimum = float('inf')
with log.open('w') as output, log.with_suffix('.memory.tsv').open('w') as memory:
    process = subprocess.Popen(sys.argv[2:], stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
    while process.poll() is None:
        available = next(int(line.split()[1]) / 1048576 for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:'))
        minimum = min(minimum, available)
        memory.write(f'{time.time():.3f}\t{available:.3f}\n')
        memory.flush()
        if available < 12.75:
            os.killpg(process.pid, signal.SIGTERM)
            process.wait()
            print(f'Memory guard stopped command: {available:.3f} GiB')
            sys.exit(125)
        time.sleep(1)
    print(f'exit={process.returncode} min_MemAvailable_GiB={minimum:.3f} log={log}')
    sys.exit(process.returncode)
