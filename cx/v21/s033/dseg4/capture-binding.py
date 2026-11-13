import json, os, pathlib, subprocess, time, urllib.request
repo = pathlib.Path('/root/worktree/zlink-dseg')
raw = pathlib.Path('/root/jobs/fx/cx/v21/s033/dseg3')
out = raw / 'out_confirmed'
trace_prefix = raw / 'trace_confirmed'
out.mkdir(parents=True, exist_ok=True)
env = os.environ.copy()
env.update(ZLINK_DSEG_TRACE=str(trace_prefix), IMPLEMENTATIONS='zlink-dotnet', PATTERNS='request-serial', PAYLOAD_SIZES='1024', RUNS='1', WARMUP_SECONDS='20', DURATION_SECONDS='15', SKIP_BUILD='1', RUN_STAMP='dseg3-confirmed', OUTPUT_DIR=str(out))
keys = ('ZLINK_DSEG_TRACE', 'IMPLEMENTATIONS', 'PATTERNS', 'PAYLOAD_SIZES', 'RUNS', 'WARMUP_SECONDS', 'DURATION_SECONDS', 'SKIP_BUILD', 'RUN_STAMP', 'OUTPUT_DIR')
cmd = ['flock', '-o', '/root/jobs/fx/cx/measure.lock', 'env'] + [f'{key}={env[key]}' for key in keys] + ['bash', 'dotnet/run_local.sh']
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
with (raw / 'runner.log').open('w') as log:
    proc = subprocess.Popen(cmd, cwd=repo / 'framework/bench/grpc', env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    deadline = time.monotonic() + 3600
    while time.monotonic() < deadline:
        try:
            runner_started = '[bench] cell=' in (raw / 'runner.log').read_text()
        except FileNotFoundError:
            runner_started = False
        if runner_started:
            break
        if proc.poll() is not None:
            raise RuntimeError(f'runner exited before cell start: {proc.returncode}')
        time.sleep(.02)
    else:
        proc.send_signal(15)
        raise RuntimeError('runner never acquired measure.lock and started its cell')
    while time.monotonic() < deadline:
        status = stats(5206)
        if status.get('phase') == 'active':
            break
        if proc.poll() is not None:
            raise RuntimeError(f'runner exited before active: {proc.returncode}')
        time.sleep(.02)
    else:
        proc.send_signal(15)
        raise RuntimeError('source never active')
    if available_kb() < 12 * 1024 * 1024:
        proc.send_signal(2)
        proc.wait()
        raise RuntimeError(f'MemAvailable below 12 GiB at active window: {available_kb()} kB')
    time.sleep(.3)
    pids = {'source': pid(5206), 'target': pid(5209)}
    start = time.time()
    with (raw / 'perf.log').open('w') as perf_log:
        perf = subprocess.run(['perf', 'record', '-v', '-k', 'mono', '-o', str(raw / 'perf.data'), '-e', 'raw_syscalls:sys_enter,raw_syscalls:sys_exit,sched:sched_switch,sched:sched_wakeup', '-p', f"{pids['source']},{pids['target']}", '--', 'sleep', '3'], stdout=perf_log, stderr=subprocess.STDOUT)
    end = time.time()
    proc.wait()
    capture = {'pids': pids, 'active_start_wall': start, 'active_end_wall': end, 'perf_exit': perf.returncode, 'runner_exit': proc.returncode, 'trace_prefix': str(trace_prefix), 'active_status_at_capture': status, 'source_stats_after': stats(5206), 'target_stats_after': stats(5209)}
    (raw / 'capture.json').write_text(json.dumps(capture, indent=2) + '\n')
    with (raw / 'perf.script').open('w') as script, (raw / 'perf.script.log').open('w') as error:
        subprocess.run(['perf', 'script', '-i', str(raw / 'perf.data'), '--ns', '-F', 'comm,pid,tid,cpu,time,event,trace'], stdout=script, stderr=error, check=False)
    print(f"DONE pids={pids} perf={perf.returncode} runner={proc.returncode}")
