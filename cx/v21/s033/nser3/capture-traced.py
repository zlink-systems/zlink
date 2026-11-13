import json, os, pathlib, signal, subprocess, time, urllib.request

repo = pathlib.Path('/root/worktree/zlink-nser')
base = pathlib.Path('/root/jobs/fx/cx/v21/s033/nser3')
for impl in ('zlink-framework-dotnet', 'zlink-dotnet'):
    for payload in (1024, 4096):
        cell = f'{impl}-request-serial-{payload}'
        out = base / 'traced-path' / cell
        out.mkdir(parents=True, exist_ok=True)
        prefix = out / 'trace'
        framework = impl == 'zlink-framework-dotnet'
        source_port, target_port = (5213, 5215) if framework else (5206, 5209)
        env = os.environ.copy()
        env.update({
            'ZLINK_DSEG_TRACE': str(prefix),
            'IMPLEMENTATIONS': impl,
            'PATTERNS': 'request-serial',
            'PAYLOAD_SIZES': str(payload),
            'RUNS': '1',
            'WARMUP_SECONDS': '15',
            'DURATION_SECONDS': '5',
            'SKIP_BUILD': '1',
            'RUN_STAMP': f'nser3-traced-{impl}-{payload}',
            'OUTPUT_DIR': str(out / 'bench'),
        })
        def stats(port):
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{port}/bench/stats', timeout=.5) as response:
                    return json.load(response)
            except Exception:
                return {}
        def pid(port):
            return int(subprocess.check_output(['lsof', '-nP', '-t', f'-iTCP:{port}', '-sTCP:LISTEN'], text=True).splitlines()[-1])
        def avail_kb():
            for line in pathlib.Path('/proc/meminfo').read_text().splitlines():
                if line.startswith('MemAvailable:'):
                    return int(line.split()[1])
            raise RuntimeError('MemAvailable missing')
        log_path = out / 'runner.log'
        with log_path.open('w') as log:
            proc = subprocess.Popen(['bash', 'dotnet/run_local.sh'], cwd=repo / 'framework/bench/grpc', env=env, stdout=log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 900
            try:
                while time.monotonic() < deadline:
                    if stats(source_port).get('phase') == 'active':
                        break
                    if proc.poll() is not None:
                        raise RuntimeError(f'{cell} runner exited early: {proc.returncode}')
                    time.sleep(.05)
                else:
                    raise RuntimeError(f'{cell} never became active')
                if avail_kb() < 12 * 1024 * 1024:
                    raise RuntimeError(f'MemAvailable below 12 GiB: {avail_kb()} kB')
                time.sleep(.3)
                pids = {'source': pid(source_port), 'target': pid(target_port)}
                with (out / 'perf.log').open('w') as perf_log:
                    perf = subprocess.run(
                        ['perf', 'record', '-q', '-k', 'CLOCK_MONOTONIC', '-o', str(out / 'perf.data'),
                         '-e', 'syscalls:sys_enter_sendto,syscalls:sys_exit_sendto,syscalls:sys_enter_recvfrom,syscalls:sys_exit_recvfrom',
                         '-p', f"{pids['source']},{pids['target']}", '--', 'sleep', '3'],
                        stdout=perf_log, stderr=subprocess.STDOUT
                    )
                proc.wait()
                meta = {'cell': cell, 'pids': pids, 'perf_exit': perf.returncode, 'runner_exit': proc.returncode,
                        'mem_available_kib': avail_kb(), 'source_stats_after': stats(source_port), 'target_stats_after': stats(target_port)}
                (out / 'capture.json').write_text(json.dumps(meta, indent=2) + '\n')
                with (out / 'perf.script').open('w') as script, (out / 'perf.script.log').open('w') as error:
                    subprocess.run(['perf', 'script', '-i', str(out / 'perf.data'), '--ns', '-F',
                                    'comm,pid,tid,cpu,time,event,trace'], stdout=script, stderr=error, check=False)
                print(f'DONE {cell} pids={pids} perf={perf.returncode} runner={proc.returncode}', flush=True)
                if perf.returncode or proc.returncode:
                    raise RuntimeError(f'{cell} perf or benchmark failed')
            except BaseException:
                if proc.poll() is None:
                    proc.send_signal(signal.SIGINT)
                    try:
                        proc.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        proc.send_signal(signal.SIGTERM)
                        proc.wait()
                raise
