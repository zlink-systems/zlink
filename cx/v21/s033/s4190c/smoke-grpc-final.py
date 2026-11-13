import json, os, pathlib, subprocess, time, urllib.request, sys
job = pathlib.Path('/root/jobs/fx/cx/v21/s033/s4190c')
root = pathlib.Path('/root/worktree/zlink-v21-s033/s4190c')
lang = sys.argv[1]
output = job / 'smoke-grpc-final'
implementations = 'grpc-cpp'
env = dict(os.environ, OUTPUT_DIR=str(output), IMPLEMENTATIONS=implementations, PATTERNS='request-backpressure', PAYLOAD_SIZES='1024', WARMUP_SECONDS='5', DURATION_SECONDS='5', RUNS='1', SKIP_BUILD='1', ZLINK_CORE_SOURCE='release', ZLINK_CORE_PACKAGE_PREFIX='/root/.cache/zlink/core-release/1.24.0/linux-x64')
start_marker = job / 'smoke-grpc-final.started'
start_marker.unlink(missing_ok=True)
seen = set()
c_warmup_threads = {}
with (job / ('smoke-grpc-final' + '.log')).open('w') as log, (job / ('threads-grpc-final' + '.txt')).open('w') as threads:
    process = subprocess.Popen(['flock', '-o', '/root/jobs/fx/cx/measure.lock', 'bash', '-c', 'touch "$2"; exec bash "$1"', '_', str(root / 'framework/bench/grpc' / lang / 'run_local.sh'), str(start_marker)], env=env, stdout=log, stderr=subprocess.STDOUT, cwd=root)
    while process.poll() is None:
        if not start_marker.exists():
            time.sleep(.25)
            continue
        for entry in pathlib.Path('/proc').iterdir():
            if not entry.name.isdecimal() or entry.name in seen:
                continue
            try:
                cmdline = (entry / 'cmdline').read_bytes().split(b'\0')
                executable = cmdline[0].decode()
                expected = str(root / 'framework/bench/grpc' / lang / 'build' / ('bench_c_with_grpc_zlink_client' if lang == 'c' else 'bench_cpp_client'))
                if executable != expected:
                    continue
                if lang == 'cpp':
                    if b'request-backpressure' not in cmdline:
                        continue
                    implementation = cmdline[cmdline.index(b'--implementation') + 1].decode()
                    if implementation != 'grpc-cpp':
                        continue
                    port = int(cmdline[cmdline.index(b'--stats-port') + 1])
                    with urllib.request.urlopen(f'http://127.0.0.1:{port}/bench/stats', timeout=.15) as response:
                        if json.load(response)['phase'] not in ('warmup', 'active'):
                            continue
                else:
                    if 'request-backpressure' not in os.readlink(entry / 'fd/1'):
                        continue
                    implementation = 'zlink-c'
                names = [(task.name, (task / 'comm').read_text().strip()) for task in (entry / 'task').iterdir()]
                count = sum(name.startswith('bench-submit-') for _, name in names)
                if count != 8:
                    continue
                if lang == 'c':
                    submit_ids = {tid for tid, name in names if name.startswith('bench-submit-')}
                    if entry.name not in c_warmup_threads:
                        c_warmup_threads[entry.name] = submit_ids
                        continue
                    if submit_ids == c_warmup_threads[entry.name]:
                        continue
                seen.add(entry.name)
                threads.write(f'{implementation} phase=warmup-or-active pid={entry.name} submit_threads={count}\n')
                threads.writelines(f'{tid} {name}\n' for tid, name in names)
                threads.write('\n'); threads.flush()
            except (OSError, IndexError, ValueError):
                pass
        time.sleep(.05)
    code = process.wait()
print(f'{lang} runner exit={code}; recorded source processes={len(seen)}')
if code:
    sys.exit(code)

path = output / 'run1/grpc-cpp-request-backpressure-1024/results.json'
document = json.loads(path.read_text())
cell = document['cells'][0]
print(json.dumps({'metadata': document['metadata'], 'cell': cell}, indent=2))
assert document['metadata']['submitConcurrency'] == 8
assert cell['streams']['count'] == 8 and cell['streams']['inFlightPerStream'] is None
assert len(seen) == 1, 'missing gRPC submit thread evidence'
print('gRPC final outcome:', 'CONTAMINATED' if cell.get('contaminated') else 'HEALTHY')
