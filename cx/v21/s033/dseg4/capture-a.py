import json, os, pathlib, signal, subprocess, time, urllib.request

repo = pathlib.Path('/root/worktree/zlink-3869a')
raw = pathlib.Path('/root/jobs/fx/cx/v21/s033/dseg4')
out = raw / 'out_a'
trace_prefix = raw / 'trace_a'
out.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env.update(
    ZLINK_DSEG_TRACE=str(trace_prefix),
    IMPLEMENTATIONS='zlink-framework-dotnet',
    PATTERNS='request-serial',
    PAYLOAD_SIZES='1024',
    RUNS='1',
    WARMUP_SECONDS='20',
    DURATION_SECONDS='15',
    SKIP_BUILD='1',
    RUN_STAMP='dseg4-a',
    OUTPUT_DIR=str(out),
)
keys = ('ZLINK_DSEG_TRACE', 'IMPLEMENTATIONS', 'PATTERNS', 'PAYLOAD_SIZES', 'RUNS',
        'WARMUP_SECONDS', 'DURATION_SECONDS', 'SKIP_BUILD', 'RUN_STAMP', 'OUTPUT_DIR')
cmd = ['flock', '-o', '/root/jobs/fx/cx/measure.lock', 'env'] + [f'{k}={env[k]}' for k in keys] + ['bash', 'dotnet/run_local.sh']

def stats(port):
    try:
        with urllib.request.urlopen(f'http://127.0.0.1:{port}/bench/stats', timeout=.5) as response:
            return json.load(response)
    except Exception:
        return {}

def pid(port):
    return int(subprocess.check_output(['lsof', '-nP', '-t', f'-iTCP:{port}', '-sTCP:LISTEN'], text=True).splitlines()[-1])

def available_kb():
    for line in pathlib.Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):
            return int(line.split()[1])
    raise RuntimeError('MemAvailable was not reported')

with (raw / 'runner-a.log').open('w') as log:
    proc = subprocess.Popen(cmd, cwd=repo / 'framework/bench/grpc', env=env, stdout=log, stderr=subprocess.STDOUT)
    deadline = time.monotonic() + 3600
    try:
        while time.monotonic() < deadline:
            if '[bench] cell=' in (raw / 'runner-a.log').read_text(errors='replace'):
                break
            if proc.poll() is not None:
                raise RuntimeError(f'runner exited before cell start: {proc.returncode}')
            time.sleep(.05)
        else:
            raise RuntimeError('runner did not start')
        while time.monotonic() < deadline:
            status = stats(5213)
            if status.get('phase') == 'active':
                break
            if proc.poll() is not None:
                raise RuntimeError(f'runner exited before active: {proc.returncode}')
            time.sleep(.05)
        else:
            raise RuntimeError('source never became active')
        if available_kb() < 12 * 1024 * 1024:
            raise RuntimeError(f'MemAvailable below 12 GiB: {available_kb()} kB')
        time.sleep(.3)
        pids = {'source': pid(5213), 'target': pid(5215)}
        start = time.time()
        with (raw / 'perf-a.log').open('w') as perf_log:
            perf = subprocess.run(
                ['perf', 'record', '-q', '-k', 'CLOCK_MONOTONIC', '-o', str(raw / 'perf-a.data'),
                 '-e', 'raw_syscalls:sys_enter,raw_syscalls:sys_exit,sched:sched_switch,sched:sched_wakeup',
                 '-p', f"{pids['source']},{pids['target']}", '--', 'sleep', '3'],
                stdout=perf_log, stderr=subprocess.STDOUT
            )
        end = time.time()
        proc.wait()
        capture = {
            'pids': pids, 'active_start_wall': start, 'active_end_wall': end,
            'perf_exit': perf.returncode, 'runner_exit': proc.returncode,
            'trace_prefix': str(trace_prefix), 'active_status_at_capture': status,
            'mem_available_kib': available_kb(), 'source_stats_after': stats(5213),
            'target_stats_after': stats(5215),
        }
        (raw / 'capture-a.json').write_text(json.dumps(capture, indent=2) + '\n')
        with (raw / 'perf-a.script').open('w') as script, (raw / 'perf-a.script.log').open('w') as error:
            subprocess.run(['perf', 'script', '-i', str(raw / 'perf-a.data'), '--ns', '-F',
                            'comm,pid,tid,cpu,time,event,trace'], stdout=script, stderr=error, check=False)
        print(f"DONE pids={pids} perf={perf.returncode} runner={proc.returncode}")
        if perf.returncode or proc.returncode:
            raise RuntimeError('perf or benchmark failed')
    except BaseException:
        if proc.poll() is None:
            proc.send_signal(signal.SIGINT)
            try:
                proc.wait(timeout=10)
            except subprocess.TimeoutExpired:
                proc.send_signal(signal.SIGTERM)
                proc.wait()
        raise
