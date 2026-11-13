import pathlib
import signal
import subprocess
import sys
import time

root = pathlib.Path('/root/jobs/fx/cx/v21/s033/clatefix')
binary = root / 'build-release/test_cpp_framework_actor_late_result'
summary = root / 'after-final-summary.txt'
current = root / 'after-current.log'


def mem_available_gib():
    for line in open('/proc/meminfo'):
        if line.startswith('MemAvailable:'):
            return int(line.split()[1]) / 1024 / 1024


def log(message):
    with summary.open('a') as stream:
        stream.write(message + '\n')
    print(message, flush=True)


def run_scenario(name, args, runs):
    if mem_available_gib() < 12:
        log(f'STOP memory MemAvailable={mem_available_gib():.2f}GiB')
        raise SystemExit(2)
    log(f'BEGIN loaded scenario={name} runs={runs} MemAvailable={mem_available_gib():.2f}GiB')
    stress_log = root / f'after-stress-{name}.log'
    with stress_log.open('ab') as stress_output:
        stress = subprocess.Popen(['stress-ng', '--cpu', '8', '--taskset', '8-15'],
                                  stdout=stress_output, stderr=subprocess.STDOUT)
        try:
            for iteration in range(1, runs + 1):
                if mem_available_gib() < 12:
                    log(f'STOP memory scenario={name} run={iteration} MemAvailable={mem_available_gib():.2f}GiB')
                    raise SystemExit(2)
                cmd = ['timeout', '-s', 'TERM', '30', 'taskset', '-c', '8-11', str(binary)] + args
                start = time.monotonic()
                with current.open('wb') as output:
                    result = subprocess.run(cmd, stdout=output, stderr=subprocess.STDOUT)
                elapsed = time.monotonic() - start
                if result.returncode != 0 or elapsed >= 29:
                    log(f'FAIL scenario={name} run={iteration}/{runs} rc={result.returncode} elapsed={elapsed:.2f}s log={current.name}')
                    raise SystemExit(1)
                if iteration % 100 == 0:
                    log(f'PROGRESS scenario={name} run={iteration}/{runs}')
        finally:
            stress.send_signal(signal.SIGTERM)
            try:
                stress.wait(timeout=10)
            except subprocess.TimeoutExpired:
                log(f'ERROR stress-ng did not exit after SIGTERM scenario={name}')
                raise SystemExit(3)
    log(f'END loaded scenario={name} runs={runs} hangs=0 failures=0')


if not binary.is_file():
    log(f'ERROR missing binary {binary}')
    raise SystemExit(2)
run_scenario('local_registry',
             ['--gtest_filter=local_actor_receiver_t.LocalRegistryTerminalsReachPreparedReceiverOnce'],
             2000)
run_scenario('whole', [], 300)
