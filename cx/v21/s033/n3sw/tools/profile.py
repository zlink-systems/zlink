#!/usr/bin/env python3
import hashlib, json, os, pathlib, signal, socket, subprocess, sys, threading, time, urllib.request
ROOT = pathlib.Path('/root/worktree/zlink-v21-s033/n3sw-scratch')
JOB = pathlib.Path('/root/jobs/fx/cx/v21/s033/n3sw')
BENCH = ROOT / 'framework/bench/grpc/node'
CPUS = '6-7,14-15'
level, pattern, size, label = sys.argv[1:]
size = int(size)
out = JOB / 'raw' / label
out.mkdir(parents=True, exist_ok=False)
artifacts = [ROOT/'framework/languages/node/packages/framework/dist/runtime/foundation/service-wire-m6a-codec.js',
             ROOT/'framework/languages/node/packages/framework/dist/runtime/channels/channel-envelope.js',
             ROOT/'framework/languages/node/node_modules/@zlink-systems/zlink/prebuilds/linux-x64/zlink.node']
for a in artifacts[:2]:
    source_file = pathlib.Path(str(a).replace('/dist/','/src/')).with_suffix('.ts')
    assert a.stat().st_mtime > source_file.stat().st_mtime, f'stale build: {a}'
def git(*args): return subprocess.check_output(['git',*args],cwd=ROOT,text=True).strip()
metadata = dict(head=git('rev-parse','HEAD'),base=git('merge-base','HEAD','origin/main'),
                dirty=git('status','--short'),artifacts={str(a):hashlib.sha256(a.read_bytes()).hexdigest() for a in artifacts},
                cpus=CPUS,method='profiling only; no measure.lock; durations <=20s; absolute numbers noisy')
(out/'build.json').write_text(json.dumps(metadata,indent=2)+'\n')
with (JOB/'progress.md').open('a') as progress:
    progress.write(f"\n- 프로파일 {label}: HEAD={metadata['head']}, base={metadata['base']}, dirty={metadata['dirty']!r}; artifact 경로·SHA256·freshness: `raw/{label}/build.json`. Receive raw `zlink-raw-server/main.js:47-53,64-75` ↔ ladder `zlink-framework-server/main.js:209-239` (DontWait/NoData/batch); reply raw `:82-86` ↔ ladder `:109-123` (새 protobuf/body, envelope/service framing 외 동일). Wait·metrics atomic 추가 0; L3 encode part당 copy 1 + binding frame copy 1, decode byte copy 0, final view/wrapper 각각 2. `call-shape.md` 기준.\n")
ports = []
for _ in range(5):
    with socket.socket() as s:
        s.bind(('127.0.0.1', 0)); ports.append(s.getsockname()[1])
endpoint, command, target, trigger, source = ports
url = lambda port: f'http://127.0.0.1:{port}'
env = dict(os.environ, NODE_PATH=str(ROOT/'framework/languages/node/node_modules'),
           ZLINK_LADDER_L0_SERVER='1', ZLINK_LADDER_L0_CLIENT='1',
           ZLINK_LADDER_L0_TWO_ROUTERS='1', ZLINK_LADDER_L0_LAZY_FRAMEWORK='1',
           ZLINK_LADDER_L1_PERMIT='1', ZLINK_LADDER_L2_ENVELOPE='1',
           ZLINK_LADDER_L2_CLIENT='1')
if level == '3':
    env.update(ZLINK_LADDER_L3_SERVICE_WIRE='1', ZLINK_LADDER_L3_CLIENT='1')
procs, logs, timers, events = [], [], [], []
started = time.monotonic()
def event(name, **values):
    events.append(dict(event=name, monotonic_us=time.monotonic_ns()//1000, **values))
def request(port, route='/bench/stats', body=None):
    data = None if body is None else json.dumps(body).encode()
    req = urllib.request.Request(url(port)+route, data=data, headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req, timeout=0.3) as reply: return json.load(reply)
def spawn(role, args):
    log = open(out/f'{role}.log', 'w'); logs.append(log)
    p = subprocess.Popen(['taskset', '-c', CPUS, 'node', '--cpu-prof', '--cpu-prof-interval=1000',
        f'--cpu-prof-dir={out}', f'--cpu-prof-name={role}.cpuprofile', '--heap-prof',
        '--heap-prof-interval=16384', f'--heap-prof-dir={out}', f'--heap-prof-name={role}.heapprofile',
        *args], cwd=BENCH, env=env, stdout=log, stderr=subprocess.STDOUT)
    procs.append((role,p,time.monotonic()))
    # This watchdog applies even if a bench process was stopped by the quiet guard.
    timer = threading.Timer(19, lambda: p.kill() if p.poll() is None else None)
    timer.daemon = True; timer.start(); timers.append(timer)
    event('spawn', role=role, pid=p.pid)
    return p
def wait_ready(port):
    while True:
        if time.monotonic()-started > 9: raise TimeoutError('readiness exceeded profile budget')
        try:
            stats = request(port)
            if stats['ready']: return
        except (OSError, ValueError): pass
        time.sleep(0.03)
def threads(tag):
    data = {}
    for role,p,_ in procs:
        entries=[]
        for entry in pathlib.Path(f'/proc/{p.pid}/task').glob('*'):
            status=entry.joinpath('status').read_text()
            entries.append(dict(tid=int(entry.name), policy=os.sched_getscheduler(int(entry.name)),
                affinity=sorted(os.sched_getaffinity(int(entry.name))),
                state=next(x for x in status.splitlines() if x.startswith('State:'))))
        data[role]=entries
    (out/f'threads-{tag}.json').write_text(json.dumps(data,indent=2)+'\n')
def phase(name, duration):
    body=dict(runId=label, cellId=label, pattern=pattern, payloadBytes=size,
        phase=name, durationMs=duration, requestWindow=100, submitConcurrency=8)
    event(name+'-start')
    ack=request(trigger,'/bench/start',body)
    if not ack.get('accepted'): raise RuntimeError(ack)
    mid=False
    while True:
        if time.monotonic()-started > 16: raise TimeoutError('profile phase exceeded budget')
        stats=request(source)
        if stats['phase']=='failed': raise RuntimeError(stats)
        if stats['phase']=='idle': break
        if not mid and name=='active': threads('active'); mid=True
        time.sleep(0.05)
    event(name+'-end')
    return stats
try:
    spawn('target',['zlink-framework-server/main.js','--endpoint',f'tcp://127.0.0.1:{endpoint}',
        '--command-endpoint',f'tcp://127.0.0.1:{command}','--metrics-url',url(target)])
    wait_ready(target)
    spawn('source',['client/main.js','--implementation','zlink-framework-node','--scenario',pattern,
        '--payload-size',str(size),'--warmup-seconds','3','--trigger-url',url(trigger),
        '--stats-url',url(source),'--target-endpoint',f'tcp://127.0.0.1:{endpoint}',
        '--target-command-endpoint',f'tcp://127.0.0.1:{command}',
        '--target-stats-url',url(target),'--output',str(out)])
    wait_ready(source)
    threads('ready')
    phase('warmup',3000)
    request(target,'/bench/reset',{})
    stats=phase('active',5000)
    target_stats=request(target)
    (out/'counters.json').write_text(json.dumps(dict(source=stats,target=target_stats),indent=2)+'\n')
    if stats['errors'] or target_stats['errors']: raise RuntimeError('nonzero errors')
    if not stats['submitted']==stats['completed']==target_stats['received']:
        raise RuntimeError('submitted/completed/received mismatch')
except Exception as error:
    event('failure', error=repr(error))
    raise
finally:
    for role,p,_ in reversed(procs):
        if p.poll() is None: p.terminate()
    for role,p,t0 in reversed(procs):
        try: p.wait(timeout=max(0.01,18-(time.monotonic()-t0)))
        except subprocess.TimeoutExpired: p.kill(); p.wait()
        event('exit',role=role,returncode=p.returncode,lifetime_s=time.monotonic()-t0)
    for timer in timers: timer.cancel()
    for log in logs: log.close()
    (out/'events.json').write_text(json.dumps(events,indent=2)+'\n')
print(out)
