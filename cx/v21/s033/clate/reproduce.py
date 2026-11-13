import time, signal, subprocess, pathlib
root = pathlib.Path('/root/jobs/fx/cx/v21/s033/clate')
binary = str(root / 'test_cpp_framework_actor_late_result')
scenarios = [
    ('whole', []),
    ('shutdown_pending', ['--gtest_filter=submitted_actor_receiver_t.ShutdownWhileAdmissionPendingDeliversOnce']),
    ('local_registry', ['--gtest_filter=local_actor_receiver_t.LocalRegistryTerminalsReachPreparedReceiverOnce']),
]

def mem_available_gib():
    for line in open('/proc/meminfo'):
        if line.startswith('MemAvailable:'):
            return int(line.split()[1]) / 1024 / 1024

def log(s):
    with open(root / 'reproduction-summary.txt', 'a') as f:
        f.write(s + '\n')
    print(s, flush=True)

def test_once(name, args, mode, iteration):
    if mem_available_gib() < 12:
        log(f'STOP memory MemAvailable={mem_available_gib():.2f}GiB')
        raise SystemExit(2)
    cmd = ['timeout', '-s', 'TERM', '30', 'taskset', '-c', '8-11', binary] + args
    outpath = root / 'run-current.log'
    start = time.monotonic()
    with open(outpath, 'wb') as out:
        p = subprocess.Popen(cmd, stdout=out, stderr=subprocess.STDOUT)
        snapped = False
        while p.poll() is None:
            elapsed = time.monotonic() - start
            if elapsed >= 18 and not snapped:
                try:
                    kids = subprocess.check_output(['pgrep', '-P', str(p.pid)], text=True).split()
                    pid = int(kids[0]) if kids else p.pid
                    stackpath = root / f'stacks-{name}-{mode}-{iteration}.txt'
                    with open(stackpath, 'w') as stack:
                        g = subprocess.run(['gdb', '-p', str(pid), '-batch', '-ex', 'thread apply all bt'], stdout=stack, stderr=subprocess.STDOUT, timeout=8)
                    log(f'STACK {name} {mode} run={iteration} pid={pid} gdb_exit={g.returncode} file={stackpath.name}')
                except Exception as e:
                    log(f'STACK_ERROR {name} {mode} run={iteration} {type(e).__name__}: {e}')
                snapped = True
            time.sleep(.1)
        rc = p.returncode
    elapsed = time.monotonic() - start
    if rc == 124 or elapsed >= 29:
        saved = root / f'output-{name}-{mode}-{iteration}.log'
        saved.write_bytes(outpath.read_bytes())
        log(f'HANG {name} {mode} run={iteration} rc={rc} elapsed={elapsed:.2f}s output={saved.name}')
        return True
    if rc != 0:
        saved = root / f'output-{name}-{mode}-{iteration}.log'
        saved.write_bytes(outpath.read_bytes())
        log(f'FAIL {name} {mode} run={iteration} rc={rc} elapsed={elapsed:.2f}s output={saved.name}')
    return False

# Whole-binary loaded testing completed in the first phase: 5 hangs in 69 runs.
for name, args in scenarios[1:]:
    log(f'BEGIN loaded scenario={name} max_runs=2000 max_hangs=5 MemAvailable={mem_available_gib():.2f}GiB')
    stress = subprocess.Popen(['stress-ng', '--cpu', '8', '--taskset', '8-15'], stdout=open(root / 'stress-ng.log', 'ab'), stderr=subprocess.STDOUT)
    hangs = runs = 0
    try:
        while runs < 2000 and hangs < 5:
            runs += 1
            if test_once(name, args, 'loaded', runs):
                hangs += 1
    finally:
        stress.send_signal(signal.SIGTERM)
        try:
            stress.wait(timeout=10)
        except subprocess.TimeoutExpired:
            log('ERROR stress-ng did not exit after SIGTERM')
    log(f'END loaded scenario={name} runs={runs} hangs={hangs} rate={hangs/runs:.6%}')

for name, args in scenarios:
    log(f'BEGIN unloaded scenario={name} runs=1000 MemAvailable={mem_available_gib():.2f}GiB')
    hangs = fails = 0
    for iteration in range(1, 1001):
        if test_once(name, args, 'unloaded', iteration):
            hangs += 1
    log(f'END unloaded scenario={name} runs=1000 hangs={hangs} rate={hangs/1000:.6%}')
