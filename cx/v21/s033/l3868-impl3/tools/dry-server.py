import json, os, pathlib, signal, socket, subprocess, sys, time, urllib.request
job = pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868-impl3')
root = pathlib.Path('/root/worktree/zlink-v21-s033/l3868-next')
level, stage = sys.argv[1:]
ports=[]
for unused in range(2):
    with socket.socket() as reservation:
        reservation.bind(('127.0.0.1',0))
        ports.append(reservation.getsockname()[1])
env = dict(os.environ, ZLINK_CPP_LADDER_LEVEL=level)
command = [str(root/'build-l3868-next/bench_cpp_framework_server'),
           '--endpoint',f'tcp://127.0.0.1:{ports[0]}','--stats-port',str(ports[1])]
result = dict(level=level, command=command, traffic_sent=0)
with (job/f'raw/dry-{stage}-server.log').open('x') as log:
    server = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    try:
        deadline=time.monotonic()+30
        while True:
            if server.poll() is not None:
                raise RuntimeError(f'server stopped during startup: {server.returncode}')
            try:
                with urllib.request.urlopen(f'http://127.0.0.1:{ports[1]}/ready',timeout=1) as response:
                    result['ready'] = response.read().decode()
                with urllib.request.urlopen(f'http://127.0.0.1:{ports[1]}/bench/stats',timeout=1) as response:
                    result['stats'] = json.load(response)
                marker = f'#4350 target L{level}:' if int(level)<=4 else 'zlink-framework-cpp target:'
                if marker in (job/f'raw/dry-{stage}-server.log').read_text():
                    break
            except OSError:
                pass
            if time.monotonic()>deadline:
                raise TimeoutError('server startup timed out')
            time.sleep(.05)
        for name in ('received','allPhaseMessages','errors'):
            assert result['stats'][name] == 0, result['stats']
    finally:
        if server.poll() is None:
            server.send_signal(signal.SIGTERM)
        try:
            result['exit_code']=server.wait(timeout=30)
        except subprocess.TimeoutExpired:
            server.kill()
            result['exit_code']=server.wait()
            raise
        (job/f'raw/dry-{stage}.json').write_text(json.dumps(result,indent=2)+'\n')
assert result['exit_code']==0, result
print(json.dumps(result))
