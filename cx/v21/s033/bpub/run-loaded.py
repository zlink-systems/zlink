import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

root = Path('/root/jobs/fx/cx/v21/s033/bpub')
label = sys.argv[1]
cmd = sys.argv[2:]
stop = threading.Event()
minimum = [None]
low = threading.Event()

def monitor():
    with (root / (label + '-memory.log')).open('w') as log:
        while not stop.is_set():
            data = Path('/proc/meminfo').read_text()
            available = int(next(s.split()[1] for s in data.splitlines()
                                 if s.startswith('MemAvailable:')))
            minimum[0] = min(minimum[0] or available, available)
            log.write(f'{time.time():.3f} {available}\n')
            log.flush()
            if available < 12 * 1024 * 1024:
                low.set()
            stop.wait(2)

monitor_thread = threading.Thread(target=monitor)
monitor_thread.start()
env = os.environ.copy()
env['ZLINK_LIBRARY_PATH'] = '/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so'
env['ZLINK_CORE_PACKAGE_PREFIX'] = '/root/.cache/zlink/core/1.23.0/linux-x64'
started = time.monotonic()
status = None
stress = None
child = None

def terminate(signum, frame):
    if child is not None and child.poll() is None:
        os.killpg(child.pid, signal.SIGTERM)
    raise KeyboardInterrupt

signal.signal(signal.SIGTERM, terminate)
signal.signal(signal.SIGINT, terminate)
try:
    if low.is_set():
        raise RuntimeError('MemAvailable below 12 GiB')
    with (root / (label + '-load.log')).open('w') as load_log:
        stress = subprocess.Popen(['stress-ng', '--cpu', '16', '--cpu-method',
                                   'loop', '--metrics-brief'],
                                  stdout=load_log, stderr=subprocess.STDOUT)
        with (root / (label + '.log')).open('w') as log:
            child = subprocess.Popen(cmd, cwd=Path.cwd(), env=env,
                                     stdout=log, stderr=subprocess.STDOUT,
                                     start_new_session=True)
            while child.poll() is None:
                if low.is_set() or time.monotonic() - started > 39 * 60:
                    os.killpg(child.pid, signal.SIGTERM)
                    child.wait(timeout=60)
                    raise RuntimeError('memory floor or 39-minute limit')
                stop.wait(1)
            status = child.returncode
finally:
    if stress is not None:
        stress.send_signal(signal.SIGTERM)
        stress.wait(timeout=60)
    stop.set()
    monitor_thread.join()
    summary = {'command': cmd, 'exit': status,
               'elapsed_seconds': time.monotonic() - started,
               'minimum_mem_available_kib': minimum[0],
               'memory_floor_violated': low.is_set(),
               'stress_exit': stress.returncode if stress else None}
    (root / (label + '-summary.json')).write_text(
        json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary))
sys.exit(status if status is not None else 1)
