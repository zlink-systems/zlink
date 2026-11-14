import argparse, concurrent.futures, hashlib, json, os, signal, subprocess, time, urllib.request
from pathlib import Path

JOB = Path('/root/jobs/fx/cx/v21/s033/j2l2')
BENCH = Path('/root/worktree/zlink-v21-s033/j2l2-scratch/framework/bench/grpc/java')
JAVA = '/usr/lib/jvm/java-25-openjdk-amd64/bin/java'
parser = argparse.ArgumentParser()
parser.add_argument('label')
parser.add_argument('step', choices=['L1', 'L2'])
parser.add_argument('pattern', choices=['request-backpressure', 'send-saturation'])
args = parser.parse_args()
out = JOB / 'raw' / args.label
out.mkdir(exist_ok=False)
env = {k:v for k,v in os.environ.items() if 'LADDER' not in k and k not in ('JAVA_TOOL_OPTIONS','JDK_JAVA_OPTIONS')}
env['ZLINK_BENCH_LADDER_' + args.step] = '1'
base = 19630
trigger, stats, target = [f'http://127.0.0.1:{base+i}' for i in (0,1,3)]
endpoint = f'tcp://127.0.0.1:{base+2}'
processes, files, phases = [], [], []
start = time.monotonic()

def request(url, body=None):
    data = None if body is None else json.dumps(body).encode()
    with urllib.request.urlopen(urllib.request.Request(url, data=data, headers={'content-type':'application/json'}), timeout=23 if body else .8) as r:
        return json.load(r)

def launch(role, extra):
    project, app, main = (('client','bench-client','systems.zlink.bench.withgrpc.client.BenchClient') if role == 'source' else
        ('zlink-framework-server','bench-zlink-framework-server','systems.zlink.bench.withgrpc.frameworkserver.ZLinkFrameworkBenchServer'))
    lib = BENCH / project / 'build/install' / app / 'lib'
    hashes = {p.name:hashlib.sha256(p.read_bytes()).hexdigest() for p in sorted(lib.glob('*.jar'))}
    (out / (role+'-jars.json')).write_text(json.dumps(hashes,indent=2))
    cmd = ['taskset','-c','6-7,14-15','timeout','--signal=TERM','--kill-after=1','29s',JAVA,
        '--enable-native-access=ALL-UNNAMED','-Xms256m','-Xmx1g','-XX:+UnlockDiagnosticVMOptions','-XX:+LogCompilation',
        f'-XX:LogFile={out}/{role}-compilation.xml', '-XX:+PrintCompilation',
        f'-agentpath:{JOB}/tools/libasyncProfiler.so=start,event=ctimer,interval=1ms,alloc=128k,jfr,file={out}/{role}.jfr',
        '-cp',str(lib/'*'),main] + extra
    log = open(out/(role+'.log'),'w'); files.append(log)
    born = time.time()
    proc = subprocess.Popen(cmd,cwd=BENCH,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
    processes.append((role,proc,born,cmd))

def ready(url, require):
    while time.monotonic()-start < 7:
        try:
            val = request(url+'/bench/stats')
            if not require or val.get('ready'): return
        except (OSError,ValueError): pass
        if any(p.poll() is not None for _,p,_,_ in processes): raise RuntimeError('child exited')
        time.sleep(.04)
    raise RuntimeError('startup exceeded 7 seconds')

def snapshot(label):
    result = {}
    for role,p,_,_ in processes:
        tids = []
        for child in Path(f'/proc/{p.pid}/task/{p.pid}/children').read_text().split():
            for t in Path(f'/proc/{child}/task').glob('*'):
                try:
                    status = t.joinpath('status').read_text()
                    affinity = next(s.split(':',1)[1].strip() for s in status.splitlines() if s.startswith('Cpus_allowed_list:'))
                    tids.append({'tid':t.name,'affinity':affinity})
                except FileNotFoundError: pass
        result[role]=tids
    (out/(label+'-affinity.json')).write_text(json.dumps(result,indent=2))

try:
    launch('target',['--endpoint',endpoint,'--metrics-url',target])
    ready(target,False)
    launch('source',['--implementation','zlink-framework-java','--scenario',args.pattern,'--payload-size','1024',
        '--request-window','100','--submit-concurrency','8','--warmup-seconds','15','--drain-bound-ms','1500',
        '--trigger-url',trigger,'--stats-url',stats,'--target-endpoint',endpoint,'--target-stats-url',target,
        '--run-id','j2l2-'+args.label,'--output',str(out),'--report-file','report.txt'])
    ready(stats,True)
    snapshot('start')
    for phase,duration in [('warmup',15),('active',5)]:
        body = dict(runId='j2l2-'+args.label,cellId=args.label,pattern=args.pattern,payloadBytes=1024,
            phase=phase,durationMs=duration*1000,requestWindow=100,submitConcurrency=8)
        before = time.time()*1000
        phases.append({'phase':phase,'epochMs':before})
        (out/'phases.json').write_text(json.dumps(phases,indent=2))
        with concurrent.futures.ThreadPoolExecutor(max_workers=1) as pool:
            future = pool.submit(request,trigger+'/bench/start',body)
            cold = False
            while not future.done():
                if time.monotonic()-start > 27: raise RuntimeError('run exceeded 27 seconds')
                now = time.time()*1000
                val = request(stats+'/bench/stats')
                with (out/'stats.jsonl').open('a') as f: f.write(json.dumps({'epochMs':now,'phaseRequested':phase,**val})+'\n')
                if phase == 'warmup' and not cold and now-before >= 2000:
                    (out/'cold-source-stats.json').write_text(json.dumps(val,indent=2))
                    (out/'cold-target-stats.json').write_text(json.dumps(request(target+'/bench/stats'),indent=2))
                    phases[0]['coldEndMs']=now
                    cold = True
                time.sleep(.08)
            result = future.result()
        (out/(phase+'-trigger.json')).write_text(json.dumps(result,indent=2))
        source = request(stats+'/bench/stats'); dest = request(target+'/bench/stats')
        (out/(phase+'-source-stats.json')).write_text(json.dumps(source,indent=2))
        (out/(phase+'-target-stats.json')).write_text(json.dumps(dest,indent=2))
        phases[-1]['endMs']=time.time()*1000
        snapshot(phase)
    (out/'phases.json').write_text(json.dumps(phases,indent=2))
    print(out,flush=True)
finally:
    for role,p,_,_ in reversed(processes):
        if p.poll() is None: os.killpg(p.pid,signal.SIGTERM)
    for role,p,_,_ in processes:
        try: p.wait(timeout=1)
        except subprocess.TimeoutExpired: os.killpg(p.pid,signal.SIGKILL); p.wait()
    (out/'lifecycle.json').write_text(json.dumps({'elapsed':time.monotonic()-start,'processes':[
        {'role':r,'pid':p.pid,'returncode':p.returncode,'bornEpochMs':b*1000,'command':c} for r,p,b,c in processes]},indent=2))
    for f in files: f.close()
