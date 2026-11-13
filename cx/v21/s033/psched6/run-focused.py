import json, pathlib, signal, subprocess, time
job = pathlib.Path('/root/jobs/fx/cx/v21/s033/psched6')
build = pathlib.Path('/root/worktree/zlink-v21-s033/psched5/.psched5-tests')
name = 'test_cpp_framework_scheduler_drain'
results = []
for attempt in range(1, 51):
    log = job / f'{name}-{attempt}.log'
    with log.open('w') as out:
        started = time.monotonic()
        p = subprocess.Popen([str(build / name)], stdout=out, stderr=subprocess.STDOUT)
        try:
            rc = p.wait(timeout=8)
            result = 'pass' if rc == 0 else 'fail'
        except subprocess.TimeoutExpired:
            with (job / 'scheduler-timeout-stacks.txt').open('w') as stack:
                debugger = subprocess.Popen(['gdb', '-q', '-batch', '-p', str(p.pid), '-ex', 'set pagination off', '-ex', 'thread apply all bt', '-ex', 'detach'], stdout=stack, stderr=subprocess.STDOUT)
                try:
                    debugger.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    debugger.terminate()
                    debugger.wait()
            status = []
            for task in pathlib.Path(f'/proc/{p.pid}/task').iterdir():
                status.append({'tid': task.name, 'wchan': (task / 'wchan').read_text()})
            (job / 'scheduler-timeout-threads.json').write_text(json.dumps(status, indent=2) + '\n')
            p.send_signal(signal.SIGTERM)
            rc = p.wait()
            result = 'timeout; stopped with SIGTERM'
        results.append({'test': name, 'attempt': attempt, 'result': result, 'exit': rc, 'elapsed_seconds': time.monotonic() - started, 'raw': str(log)})
        (job / 'focused-results.json').write_text(json.dumps(results, indent=2) + '\n')
        print(json.dumps(results[-1]), flush=True)
        if result != 'pass':
            raise SystemExit(1)
