import json
import os
from pathlib import Path
import signal
import subprocess
import sys
import threading
import time

root = Path('/root/jobs/fx/cx/v21/s033/cbclose')
label = sys.argv[1]
command = sys.argv[2:]
stopped = threading.Event()
def interrupted(signum, frame):
    raise InterruptedError(f'signal {signum}')
signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGINT, interrupted)
child = None

def memory():
    with (root / (label + '-memory.log')).open('w') as log:
        while not stopped.is_set():
            available = int(next(x.split()[1] for x in Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')))
            log.write(f'{time.time()} MemAvailable_kB={available}\n')
            log.flush()
            if available < 12 * 1024 * 1024:
                os.kill(os.getpid(), signal.SIGTERM)
            stopped.wait(1)

with (root / (label + '-load.log')).open('w') as log:
    load = subprocess.Popen(['stress-ng', '--cpu', '16', '--cpu-method', 'loop', '--metrics-brief'], stdout=log, stderr=subprocess.STDOUT)
    monitor = threading.Thread(target=memory)
    monitor.start()
    try:
        with (root / (label + '.log')).open('w') as output:
            child = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT, start_new_session=True)
            code = child.wait()
        print(json.dumps({'label': label, 'command': command, 'exit': code}), flush=True)
    finally:
        if child is not None and child.poll() is None:
            os.killpg(child.pid, signal.SIGTERM)
            child.wait()
        load.send_signal(signal.SIGTERM)
        load.wait()
        stopped.set()
        monitor.join()

sys.exit(code if code >= 0 else 128 - code)
