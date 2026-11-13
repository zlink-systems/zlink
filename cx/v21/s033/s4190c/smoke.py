import json, os, pathlib, subprocess, time, urllib.request, sys
job = pathlib.Path('/root/jobs/fx/cx/v21/s033/s4190c')
root = pathlib.Path('/root/worktree/zlink-v21-s033/s4190c')
lang = sys.argv[1]
output = job / ('smoke-' + lang)
implementations = os.environ.get('IMPLEMENTATIONS', 'zlink-c' if lang == 'c' else 'grpc-cpp,zlink-cpp,zlink-framework-cpp')
env = dict(os.environ, OUTPUT_DIR=str(output), IMPLEMENTATIONS=implementations, PATTERNS='request-serial,request-backpressure,send-saturation', PAYLOAD_SIZES='1024', WARMUP_SECONDS='5', DURATION_SECONDS='5', RUNS='1', SKIP_BUILD='1', ZLINK_CORE_SOURCE='release', ZLINK_CORE_PACKAGE_PREFIX='/root/.cache/zlink/core-release/1.24.0/linux-x64')
start_marker = job / ("smoke-" + lang + ".started")
start_marker.unlink(missing_ok=True)
seen = set()
c_warmup_threads = {}
with (job / ('smoke-' + lang + '.log')).open('w') as log, (job / ('threads-' + lang + '.txt')).open('w') as threads:
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
                    port = int(cmdline[cmdline.index(b'--stats-port') + 1])
                    with urllib.request.urlopen(f'http://127.0.0.1:{port}/bench/stats', timeout=.15) as response:
                        if json.load(response)['phase'] != 'active':
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
                threads.write(f'{implementation} phase=active pid={entry.name} submit_threads={count}\n')
                threads.writelines(f'{tid} {name}\n' for tid, name in names)
                threads.write('\n'); threads.flush()
            except (OSError, IndexError, ValueError):
                pass
        time.sleep(.05)
    code = process.wait()
print(f'{lang} runner exit={code}; recorded source processes={len(seen)}')
if code:
    sys.exit(code)
for path in sorted(output.glob('run*/*/results.json')):
    document = json.loads(path.read_text())
    cell = document['cells'][0]
    count = 1 if cell['pattern'] == 'request-serial' else 8
    assert not cell.get('contaminated', False), (path, cell.get('contamination_reason'))
    assert document['metadata']['submitConcurrency'] == 8, path
    assert cell['streams']['count'] == count, path
    assert cell['streams']['inFlightPerStream'] == (None if cell['pattern'] == 'request-backpressure' else 1), path
    completed = cell.get('completed', cell.get('extra', {}).get('completed'))
    assert completed > 0 and cell['errors'] == 0, (path, completed, cell['errors'])
    assert not cell.get('drain_bound_hit', False) and cell.get('abandoned', 0) == 0, path
    print(path.relative_to(job), 'completed=', completed, 'errors=', cell['errors'], 'streams=', count)
assert len(seen) >= len(implementations.split(',')), 'missing thread evidence'
