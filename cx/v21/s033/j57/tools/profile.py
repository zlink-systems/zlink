import argparse, json, os, signal, subprocess, time, urllib.request
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
P = Path('/root/jobs/fx/cx/v21/s033/j57')
CPUS = '6-7,14-15'
a = argparse.ArgumentParser()
a.add_argument('language', choices=['java','node'])
a.add_argument('step', choices=['L4','L5_7'])
a.add_argument('label')
a.add_argument('--duration', type=float, default=4)
a.add_argument('--warmup', type=float, default=3)
a.add_argument('--profile', action='store_true')
a.add_argument('--scenario', default='request-backpressure', choices=['request-backpressure','send-saturation'])
a.add_argument('--core')
args = a.parse_args()
root = Path('/root/worktree/zlink-v21-s033/j57-scratch')
bench = root / 'framework/bench/grpc' / args.language
out = P / 'raw' / args.label
out.mkdir(parents=True, exist_ok=False)
env = os.environ.copy()
for key in list(env):
    if 'LADDER' in key or key in ('JAVA_TOOL_OPTIONS','NODE_OPTIONS','JDK_JAVA_OPTIONS'):
        del env[key]
env['NODE_PATH'] = str(root / 'framework/languages/node/node_modules')
env['PROFILE_DIR'] = str(out)
base = 19670 if args.language == 'java' else 19680
trigger, stats, endpoint, target = [f'http://127.0.0.1:{base+i}' for i in (0,1,2,3)]
endpoint = f'tcp://127.0.0.1:{base+2}'
if args.language == 'java':
    env['ZLINK_BENCH_LADDER_' + args.step] = '1'
    java = '/usr/lib/jvm/java-25-openjdk-amd64/bin/java'
    def command(role):
        project, app, main = (('client','bench-client','systems.zlink.bench.withgrpc.client.BenchClient') if role == 'source' else
            ('zlink-framework-server','bench-zlink-framework-server','systems.zlink.bench.withgrpc.frameworkserver.ZLinkFrameworkBenchServer'))
        cmd = [java, '--enable-native-access=ALL-UNNAMED', '-Xms256m', '-Xmx1g']
        if args.profile:
            cmd += [f'-agentpath:/root/jobs/fx/cx/v21/s033/l2bp/tools/libasyncProfiler.so=start,event=ctimer,interval=1ms,wall=10ms,alloc=256k,jfr,file={out}/{role}.jfr']
        classpath = str(bench / project / 'build/install' / app / 'lib/*')
        if args.core: classpath = args.core + os.pathsep + classpath
        cmd += ['-cp', classpath, main]
        return cmd
else:
    env.update(ZLINK_LADDER_L0_SERVER='1', ZLINK_LADDER_L0_CLIENT='1', ZLINK_LADDER_L1_PERMIT='1')
    if args.step == 'L2': env.update(ZLINK_LADDER_L2_ENVELOPE='1', ZLINK_LADDER_L2_CLIENT='1')
    def command(role):
        cmd = ['node']
        if args.profile: cmd += ['--cpu-prof', '--cpu-prof-interval=500', '--cpu-prof-dir=' + str(out),
                                '--require=' + str(P / 'tools/event-loop.cjs')]
        cmd += [str(bench / ('client/main.js' if role == 'source' else 'zlink-framework-server/main.js'))]
        return cmd
procs = []
logs = []
start = time.monotonic()
phases = []
def launch(role, cmd):
    log = open(out / (role + '.log'), 'w')
    logs.append(log)
    proc = subprocess.Popen(['taskset','-c',CPUS,'timeout','--signal=TERM','--kill-after=1','19s'] + cmd,
      cwd=bench, env=env, stdout=log, stderr=subprocess.STDOUT, start_new_session=True)
    procs.append((role, proc, time.monotonic()))
    return proc

def request(url, body=None):
    data = None if body is None else json.dumps(body).encode()
    with urllib.request.urlopen(urllib.request.Request(url, data=data, headers={'content-type':'application/json'}), timeout=max(args.warmup,args.duration)+2 if body is not None else .8) as r:
        return json.load(r)

def await_ready(url, require=True):
    while time.monotonic() - start < 9:
        try:
            val = request(url + '/bench/stats')
            if not require or val.get('ready'): return val
        except (OSError,ValueError): pass
        if any(p.poll() is not None for _,p,_ in procs): raise RuntimeError('process exited during startup')
        time.sleep(.04)
    raise RuntimeError('startup deadline')

def snapshot(label):
    result = {'time':time.monotonic()-start,'processes':{}}
    for role, p, _ in procs:
        # timeout has one child (the JVM/Node); inspect that process and all threads.
        children = Path(f'/proc/{p.pid}/task/{p.pid}/children').read_text().split()
        threads = []
        for child in children:
            for t in Path(f'/proc/{child}/task').glob('*'):
                status = t.joinpath('status').read_text()
                affinity = next(s.split(':',1)[1].strip() for s in status.splitlines() if s.startswith('Cpus_allowed_list:'))
                threads.append({'tid':t.name,'affinity':affinity,'stat':t.joinpath('stat').read_text(),'wchan':t.joinpath('wchan').read_text()})
        result['processes'][role] = threads
    (out / (label + '.json')).write_text(json.dumps(result,indent=2))

try:
    launch('target', command('target') + ['--endpoint',endpoint,'--metrics-url',target, '--command-endpoint',f'tcp://127.0.0.1:{base+4}'])
    await_ready(target, False)
    launch('source', command('source') + ['--implementation','zlink-framework-' + args.language,
      '--scenario',args.scenario,'--payload-size','1024','--request-window','100',
      '--submit-concurrency','8','--warmup-seconds',str(args.warmup),
      '--trigger-url',trigger,'--stats-url',stats,'--target-endpoint',endpoint,
      '--target-stats-url',target,'--run-id','j57-' + args.label,'--output',str(out),'--report-file','report.txt'])
    await_ready(stats)
    snapshot('start')
    for phase,duration in [('warmup', args.warmup), ('active',args.duration)]:
        body = dict(runId='j57-' + args.label,cellId=args.label,pattern=args.scenario,payloadBytes=1024,
                    phase=phase,durationMs=int(duration*1000),requestWindow=100,submitConcurrency=8)
        phases.append({'phase':phase,'monotonicUs':time.monotonic_ns()/1000,'epochMs':time.time()*1000})
        (out / 'phases.json').write_text(json.dumps(phases,indent=2))
        pool = ThreadPoolExecutor(max_workers=1)
        trigger_future = pool.submit(request, trigger + '/bench/start', body)
        halfway = False
        while True:
            if time.monotonic() - start > 18: raise RuntimeError('profile run deadline')
            val = request(stats + '/bench/stats')
            with (out / 'stats.jsonl').open('a') as f: f.write(json.dumps(dict(phaseRequested=phase, **val)) + '\n')
            if phase == 'active' and not halfway:
                snapshot('active')
                halfway = True
            if val.get('phase') in ('idle','completed') and trigger_future.done(): break
            time.sleep(.15)
        (out / (phase + '-trigger.json')).write_text(json.dumps(trigger_future.result()))
        pool.shutdown()
        (out / (phase + '-source-stats.json')).write_text(json.dumps(val,indent=2))
        (out / (phase + '-target-stats.json')).write_text(json.dumps(request(target + '/bench/stats'),indent=2))
    print(str(out), flush=True)
finally:
    for role,p,born in reversed(procs):
        if p.poll() is None: os.killpg(p.pid, signal.SIGTERM)
    for role,p,born in procs:
        try: p.wait(timeout=1)
        except subprocess.TimeoutExpired: os.killpg(p.pid, signal.SIGKILL); p.wait()
    (out / 'lifecycle.json').write_text(json.dumps({'elapsed':time.monotonic()-start, 'startMonotonicUs':start*1e6,
       'processes':[{'role':role,'pid':p.pid,'returncode':p.returncode} for role,p,_ in procs]},indent=2))
    for f in logs: f.close()
