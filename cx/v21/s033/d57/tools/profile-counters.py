import os, sys, time, json, subprocess, pathlib, urllib.request, hashlib, threading
scratch = pathlib.Path('/root/worktree/zlink-v21-s033/d57-scratch')
job = pathlib.Path('/root/jobs/fx/cx/v21/s033/d57')
level, pattern, label = sys.argv[1:]
out = job/'raw'/label/(level+'-'+pattern)
out.mkdir(parents=True, exist_ok=False)
bench = scratch/'framework/bench/grpc/dotnet'
paths = [bench/'Client/bin/Release/net8.0/WithGrpcBench.Client.dll', bench/'ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll']
for f in paths:
    assert f.exists(), f
    assert f.stat().st_mtime > max(x.stat().st_mtime for x in f.parents[4].rglob('*.cs') if '/obj/' not in str(x) and '/bin/' not in str(x)), f
manifest = dict(commit='29d9fca82b', scratch_archive=True, production_dirty=bool(subprocess.check_output(['git','status','--porcelain'],text=True).strip()), base=subprocess.check_output(['git','merge-base','HEAD','origin/main'],text=True).strip(), pinned='6-7,14-15', tiered_compilation=True, max_process_seconds=20, throughput_verdict=False, artifacts={str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in paths + [paths[0].parent/'Zlink.Framework.dll']})
manifest['production_sources']={str(f.relative_to(scratch)):hashlib.sha256(f.read_bytes()).hexdigest() for f in (scratch/'framework/languages/dotnet/src/Zlink.Framework').rglob('*.cs') if '/bin/' not in str(f) and '/obj/' not in str(f)}
manifest['serializer_sources'] = {str(f.relative_to(scratch)):hashlib.sha256(f.read_bytes()).hexdigest() for f in [scratch/'framework/languages/dotnet/src/Shared/ZLinkJsonSerializerOptions.cs',scratch/'framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkFrameworkJsonPayloadCodec.cs',scratch/'framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs']}
(out/'manifest.json').write_text(json.dumps(manifest,indent=2))
with (job/'progress.md').open('a') as f: f.write('\n- 수신/응답 대조: raw blocking Recv→unpack→typed JSON→fresh reply; L57 owner receive→worker→fresh handler scope→fresh typed JSON reply. 추가 기능은 call-shape.md의 L5~L7만.\n- profile '+str(out.relative_to(job))+': '+json.dumps(manifest)+'\n')
env = os.environ.copy()
env['DOTNET_PerfMapEnabled']='0'
env['DOTNET_TieredCompilation']='1'
env['ZLINK_DOTNET_LADDER_LEVEL']=level
if level=='L4': env.update(ZLINK_DOTNET_LADDER_CLIENT='raw', ZLINK_DOTNET_RAW_TARGET='framework')
else:
    env.pop('ZLINK_DOTNET_LADDER_CLIENT',None); env.pop('ZLINK_DOTNET_RAW_TARGET',None)
processes=[]; traces=[]; files=[]
def launch(cmd,name):
    cmd=['taskset','-c','6-7,14-15']+cmd
    log=(out/(name+'.log')).open('w'); files.append(log)
    p=subprocess.Popen(cmd,env=env,stdout=log,stderr=subprocess.STDOUT)
    processes.append(p)
    timer=threading.Timer(19, lambda: p.kill() if p.poll() is None else None)
    timer.daemon=True; timer.start()
    return p
def counters(p):
    x=pathlib.Path('/proc/'+str(p.pid))
    stat=(x/'stat').read_text().rsplit(')',1)[1].split()
    return dict(cpu_ticks=int(stat[11])+int(stat[12]),utc=time.time(),monotonic=time.monotonic(),clock_ticks=os.sysconf('SC_CLK_TCK'))
def request(port,path='/bench/stats',data=None):
    body=None if data is None else json.dumps(data).encode()
    req=urllib.request.Request('http://127.0.0.1:'+str(port)+path,data=body,headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=1) as r: return json.load(r)
def ready(port):
    until=time.monotonic()+5
    while time.monotonic()<until:
        try: return request(port)
        except Exception: time.sleep(.05)
    raise RuntimeError('readiness timeout '+str(port))
def idle():
    until=time.monotonic()+7
    while time.monotonic()<until:
        s=request(25506)
        if s.get('phase') in ('idle','completed','failed'):
            if s.get('phase')=='failed': raise RuntimeError(s)
            return s
        time.sleep(.05)
    raise RuntimeError('phase timeout')
def snapshot(name, ps):
    records=[]
    for role,p in ps:
        for path in pathlib.Path('/proc/'+str(p.pid)+'/task').glob('*/status'):
            try:
                status=path.read_text(); tid=int(path.parent.name)
                cpus=next(x.split(':',1)[1].strip() for x in status.splitlines() if x.startswith('Cpus_allowed_list:'))
                records.append(dict(role=role,pid=p.pid,tid=tid,cpus=cpus,policy=os.sched_getscheduler(tid)))
                assert set(os.sched_getaffinity(tid)) <= {6,7,14,15}, (tid,cpus)
            except FileNotFoundError: pass
    (out/(name+'.json')).write_text(json.dumps(records,indent=2))
try:
    target=launch(['dotnet',str(paths[1]),'--endpoint','tcp://127.0.0.1:25514','--metrics-url','http://127.0.0.1:25515'],'target')
    started=time.monotonic()
    ready(25515)
    source=launch(['dotnet',str(paths[0]),'--implementation',('zlink-dotnet' if level=='L4' else 'zlink-framework-dotnet'),'--scenario',pattern,'--payload-size','1024','--request-window','100','--submit-concurrency','8','--warmup-seconds','1','--drain-bound-ms','1000','--trigger-url','http://127.0.0.1:25505','--stats-url','http://127.0.0.1:25506','--target-endpoint','tcp://127.0.0.1:25514','--target-command-endpoint','tcp://127.0.0.1:25514','--target-stats-url','http://127.0.0.1:25515','--raw-socket','router','--output',str(out),'--report-file','bench-report.txt','--configuration','Release'],'source')
    ready(25506)
    snapshot('start', [('source',source),('target',target)])
    trigger=dict(runId=label,cellId=level+'-'+pattern,pattern=pattern,payloadBytes=1024,requestWindow=100,submitConcurrency=8,phase='warmup',durationMs=1000)
    request(25505,'/bench/start',trigger); time.sleep(.1); idle()
    for role,port in [('source',25506),('target',25515)]: (out/(role+'-handoffs-before.json')).write_text(json.dumps(request(port,'/d57/handoffs'),indent=2))
    trigger.update(phase='active',durationMs=3000)
    before={role:counters(p) for role,p in [('source',source),('target',target)]}
    manifest['active_start_utc']=time.time()
    request(25505,'/bench/start',trigger)
    time.sleep(1.5)
    snapshot('mid-active',[('source',source),('target',target)])
    idle()
    manifest['active_stop_utc']=time.time()
    for role,port in [('source',25506),('target',25515)]: (out/(role+'-handoffs-after.json')).write_text(json.dumps(request(port,'/d57/handoffs'),indent=2))
    after={role:counters(p) for role,p in [('source',source),('target',target)]}
    (out/'process-counters.json').write_text(json.dumps(dict(before=before,after=after),indent=2))
    for role,port in [('source',25506),('target',25515)]: (out/(role+'-stats.json')).write_text(json.dumps(request(port),indent=2))
    for p in traces: p.wait(timeout=max(.1,18-(time.monotonic()-started)))
    for role,p in [('source',source),('target',target)]:
        for prefix,suffix in [('perf-','.map'),('perfinfo-','.map'),('jit-','.dump')]:
            f=pathlib.Path('/tmp')/(prefix+str(p.pid)+suffix)
            if f.exists(): (out/(role+'-'+f.name)).write_bytes(f.read_bytes())
    manifest.update(elapsed_seconds=time.monotonic()-started)
    assert manifest['elapsed_seconds']<20
    (out/'manifest.json').write_text(json.dumps(manifest,indent=2))
finally:
    for p in processes:
        if p.poll() is None: p.terminate()
    for p in processes:
        try: p.wait(timeout=1)
        except subprocess.TimeoutExpired: p.kill(); p.wait()
    for f in files: f.close()
