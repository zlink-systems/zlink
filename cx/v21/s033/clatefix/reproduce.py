import pathlib
import signal
import subprocess
import sys
import time

root = pathlib.Path('/root/jobs/fx/cx/v21/s033/clatefix')
binary = pathlib.Path(sys.argv[1])
phase = sys.argv[2]
filter_name = 'local_actor_receiver_t.LocalRegistryTerminalsReachPreparedReceiverOnce'

def mem_available_gib():
    for line in open('/proc/meminfo'):
        if line.startswith('MemAvailable:'):
            return int(line.split()[1]) / 1024 / 1024

def log(line):
    with (root / f'{phase}-summary.txt').open('a') as stream:
        stream.write(line + '\n')
    print(line, flush=True)

def test_once(name, args, iteration):
    available = mem_available_gib()
    if available < 12:
        log(f'STOP memory MemAvailable={available:.2f}GiB')
        raise SystemExit(2)
    cmd = ['timeout', '-s', 'TERM', '30', 'taskset', '-c', '8-11', str(binary)] + args
    with (root / 'run-current.log').open('wb') as out:
        start = time.monotonic()
        process = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT)
        rc = process.wait()
    elapsed = time.monotonic() - start
    if rc == 124 or elapsed >= 29:
        (root / f'{phase}-{name}-hang-{iteration}.log').write_bytes(
            (root / 'run-current.log').read_bytes())
        log(f'HANG {name} run={iteration} rc={rc} elapsed={elapsed:.2f}s')
        return 'hang'
    if rc != 0:
        (root / f'{phase}-{name}-fail-{iteration}.log').write_bytes(
            (root / 'run-current.log').read_bytes())
        log(f'FAIL {name} run={iteration} rc={rc} elapsed={elapsed:.2f}s')
        return 'fail'
    return 'ok'

def loaded(name, args, limit, hang_limit=None):
    stress_log = (root / f'{phase}-stress-ng.log').open('ab')
    stress = subprocess.Popen(['stress-ng', '--cpu', '8', '--taskset', '8-15'],
                              stdout=stress_log, stderr=subprocess.STDOUT)
    hangs = fails = runs = 0
    log(f'BEGIN loaded scenario={name} max_runs={limit} max_hangs={hang_limit} MemAvailable={mem_available_gib():.2f}GiB')
    try:
        while runs < limit and (hang_limit is None or hangs < hang_limit):
            runs += 1
            outcome = test_once(name, args, runs)
            hangs += outcome == 'hang'
            fails += outcome == 'fail'
            if phase == 'after' and outcome != 'ok':
                break
    finally:
        stress.send_signal(signal.SIGTERM)
        stress.wait(timeout=10)
        stress_log.close()
    log(f'END loaded scenario={name} runs={runs} hangs={hangs} failures={fails}')
    return hangs, fails

if phase == 'before':
    loaded('local_registry', [f'--gtest_filter={filter_name}'], 500, 3)
elif phase == 'after':
    hangs, fails = loaded('local_registry', [f'--gtest_filter={filter_name}'], 2000)
    if hangs == 0 and fails == 0:
        loaded('whole', [], 300)
else:
    raise SystemExit('phase must be before or after')
