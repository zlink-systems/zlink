import os, signal, subprocess, sys, time, json
from pathlib import Path
root = Path(__file__).parent
variant, pattern, label = sys.argv[1:4]
env = os.environ.copy()
env.update(PERF_SINGLE_DURATION_SECONDS='10', PERF_PART_COUNT='1', PERF_SINGLE_LATENCY_SAMPLE_CAP='1000000')
command = ['dotnet', str(root/('perf-after-final-02' if variant == 'after' else 'perf-main')/'Zlink.BindingBench.dll'), pattern, 'inproc', '1024']
metadata = {'command': command, 'duration': 10, 'part_count': 1, 'variant': variant, 'pattern': pattern, 'source_cpus': list(range(8)), 'target_cpus': list(range(8,16)), 'threads': [], 'runtime':env.get('ZLINK_LIBRARY_PATH')}
assert int(next(line.split()[1] for line in Path('/proc/meminfo').read_text().splitlines() if line.startswith('MemAvailable:'))) >= 12 * 1024 * 1024
os.sched_setaffinity(0, set(range(8)))
with (root/'raw'/f'{label}.log').open('w') as log:
    process = subprocess.Popen(command, env=env, stdout=log, stderr=subprocess.STDOUT)
    # The single runner receives on its main thread. Pin that thread after
    # the sender exists; all created threads inherit source CPUs 0-7.
    pinned = False
    candidates = {}
    names = {}
    while process.poll() is None and not pinned:
        for task in (Path('/proc')/str(process.pid)/'task').glob('*'):
            try:
                name = (task/'comm').read_text().strip()
                names[task.name] = name
                source_tid = int(task.name)
                candidate = name == '.NET Thread' or name.startswith('single dealer') or (name == 'dotnet' and source_tid != process.pid)
                candidates[source_tid] = candidates.get(source_tid, 0) + 1 if candidate else 0
                if candidates[source_tid] >= 3:
                    os.sched_setaffinity(source_tid, set(range(8)))
                    os.sched_setaffinity(process.pid, set(range(8,16)))
                    metadata['source_tid'] = source_tid
                    metadata['target_tid'] = process.pid
                    metadata['source_affinity'] = sorted(os.sched_getaffinity(source_tid))
                    metadata['target_affinity'] = sorted(os.sched_getaffinity(process.pid))
                    pinned = True
                    break
            except (FileNotFoundError, ProcessLookupError):
                pass
        if not pinned:
            time.sleep(.001)
    if pinned:
        for task in (Path('/proc')/str(process.pid)/'task').glob('*'):
            try:
                metadata['threads'].append({'tid':int(task.name), 'name':(task/'comm').read_text().strip(), 'affinity':sorted(os.sched_getaffinity(int(task.name)))})
            except (FileNotFoundError, ProcessLookupError):
                pass
    metadata['observed_names'] = names
    code = process.wait()
    metadata['exit_code'] = code
    metadata['pinned'] = pinned
    (root/'raw'/f'{label}.json').write_text(json.dumps(metadata, indent=2))
    print(f'{label}: exit={code} pinned={pinned}', flush=True)
    sys.exit(code)
