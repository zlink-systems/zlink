import os,sys,pathlib,subprocess,time,json,signal,hashlib,urllib.request,urllib.error,struct,mmap,csv,datetime
from counter_io import read,SampleView
R=pathlib.Path('/root/worktree/zlink-v21-s033/xbp');J=pathlib.Path('/root/jobs/fx/cx/v21/s033/xbp');G=R/'framework/bench/grpc';CORE=pathlib.Path('/root/worktree/zlink-v21-s033/xbp/.artifacts/wsl/install/zlink-core/1.23.0/lib/libzlink.so.1.23.0')
lang,kind,payload,mode,run=sys.argv[1:];payload=int(payload);impl='zlink-'+('framework-' if kind=='framework' else '')+lang
cell=f'{lang}-{kind}-{payload}-{mode}-r{run}';D=J/'raw'/cell;D.mkdir(exist_ok=False)
WARM=10;ACTIVE=12;WIN_BEGIN=2;WIN_END=10
procs=[];logs=[];views={};mem_min=2**63
def dump(name,x): (D/name).write_text(json.dumps(x,indent=2)+'\n')
def http(port,path='/bench/stats',obj=None):
    data=json.dumps(obj).encode() if obj is not None else None
    req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=data,headers={'content-type':'application/json'})
    with urllib.request.urlopen(req,timeout=3) as resp:
        raw=resp.read()
        return json.loads(raw) if raw else None
def mem():
    for line in pathlib.Path('/proc/meminfo').read_text().splitlines():
        if line.startswith('MemAvailable:'):return int(line.split()[1])
def checkmem():
    global mem_min
    value=mem();mem_min=min(mem_min,value)
    if value<12582912:raise RuntimeError(f'MemAvailable {value} below 12 GiB')
    return value
def waitready(port,source):
    deadline=time.monotonic()+60
    while time.monotonic()<deadline:
        checkmem()
        for p in procs:
            if p.poll() is not None:raise RuntimeError(f'process exited {p.pid}: {p.returncode}')
        try:
            x=http(port)
            if not source or x.get('ready') is True:return x
            if x.get('phase')=='failed':raise RuntimeError(str(x))
        except (OSError,urllib.error.URLError,ValueError):pass
        time.sleep(.1)
    raise RuntimeError('ready timeout')
def waitidle(port,bound=60):
    deadline=time.monotonic()+bound
    while time.monotonic()<deadline:
        x=http(port)
        if x.get('phase')=='idle':return x
        if x.get('phase')=='failed':raise RuntimeError(str(x))
        checkmem();time.sleep(.1)
    raise RuntimeError('idle timeout')
def settle(sp,tp,counter):
    previous=None;stable=0;deadline=time.monotonic()+30
    while time.monotonic()<deadline:
        checkmem()
        a=http(sp);b=http(tp);values=(a.get('completed'),a.get('currentInFlight',a.get('inFlight')),b[counter],b.get('errors'))
        stable=stable+1 if values==previous else 0
        if stable>=2:return a,b
        previous=values;time.sleep(.1)
    raise RuntimeError('settle timeout')
def launch(role,args,cpus):
    f=open(D/f'{role}.log','w');logs.append(f)
    e=os.environ.copy();e.update(ZLINK_LIBRARY_PATH=str(CORE),LD_LIBRARY_PATH=str(CORE.parent),JAVA_HOME='/usr/lib/jvm/java-25-openjdk-amd64')
    if mode=='count':
        e.update(XBP_DIR=str(D/role),ZLINK_LIBRARY_PATH=str(J/'counter.so'),LD_PRELOAD=str(J/'counter.so'))
    cmd=['taskset','-c',cpus,*args]
    dump(f'{role}-command.json',cmd)
    p=subprocess.Popen(cmd,env=e,cwd=R,stdout=f,stderr=subprocess.STDOUT,start_new_session=True);procs.append(p);return p

def verify_maps(role,pid):
    maps=pathlib.Path(f'/proc/{pid}/maps').read_text();(D/f'{role}-maps.txt').write_text(maps)
    libs=sorted({line.split()[-1] for line in maps.splitlines() if '/libzlink.so' in line and line.split()[-1].startswith('/')})
    if not libs:raise RuntimeError(f'no native maps {role}')
    shas={p:hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest() for p in libs};dump(f'{role}-native.json',shas)
    expected=hashlib.sha256(CORE.read_bytes()).hexdigest()
    if set(shas.values())!={expected}:raise RuntimeError(str(shas))
    tasks={p.name:(p/'status').read_text().split('Cpus_allowed_list:')[1].splitlines()[0].strip() for p in pathlib.Path(f'/proc/{pid}/task').iterdir()}
    dump(f'{role}-affinity.json',tasks)

def specs():
    if lang=='cpp':
        sp,tp,tr=(5293,5295,5292) if kind=='framework' else (5286,5289,5285);ep=f'tcp://127.0.0.1:{5294 if kind=="framework" else 5287}'
        target=[str(G/'cpp/build'/('bench_cpp_framework_server' if kind=='framework' else 'bench_cpp_zlink_server')),'--endpoint',ep,'--stats-port',str(tp)]
        source=[str(G/'cpp/build/bench_cpp_client'),'--implementation',impl,'--pattern','request-backpressure','--payload-size',str(payload),'--trigger-port',str(tr),'--stats-port',str(sp),'--target-stats-port',str(tp),'--endpoint',ep,'--output-file',str(D/'results.json'),'--warmup-seconds',str(WARM),'--warmup-segments','10','--request-timeout-ms','30000','--drain-bound-ms','30000']
        if kind=='binding':
            source+=['--command-endpoint','tcp://127.0.0.1:5288'];target+=['--command-endpoint','tcp://127.0.0.1:5288']
    else:
        if lang=='dotnet':sp,tp,tr=(5213,5215,5212) if kind=='framework' else (5206,5209,5205);port=5214 if kind=='framework' else 5207;ce='tcp://127.0.0.1:5208'
        else:sp,tp,tr=(5253,5255,5252) if kind=='framework' else (5246,5249,5245);port=5254 if kind=='framework' else 5247;ce='tcp://127.0.0.1:5248'
        ep=f'tcp://127.0.0.1:{port}'
        if lang=='dotnet':
            sub='ZLinkServer' if kind=='framework' else 'ZLinkRawServer';target=['dotnet',str(G/f'dotnet/{sub}/bin/Release/net8.0/WithGrpcBench.{sub}.dll')];source=['dotnet',str(G/'dotnet/Client/bin/Release/net8.0/WithGrpcBench.Client.dll')]
        else:
            sub='zlink-framework-server' if kind=='framework' else 'zlink-raw-server';bn='bench-zlink-framework-server' if kind=='framework' else 'bench-zlink-raw-server'
            target=[str(G/f'java/{sub}/build/install/{bn}/bin/{bn}')];source=[str(G/'java/client/build/install/bench-client/bin/bench-client')]
        target+=['--endpoint',ep,'--metrics-url',f'http://127.0.0.1:{tp}']
        source+=['--implementation',impl,'--scenario','request-backpressure','--payload-size',str(payload),'--request-window','100','--send-concurrency','8','--latency-sample-limit','200000','--warmup-seconds',str(WARM),'--drain-bound-ms','30000','--trigger-url',f'http://127.0.0.1:{tr}','--stats-url',f'http://127.0.0.1:{sp}','--target-endpoint',ep,'--target-stats-url',f'http://127.0.0.1:{tp}','--raw-socket','router','--output',str(D),'--report-file','report.txt']
        if lang=='dotnet':source+=['--configuration','Release','--timeout-seconds','300']
        else:source+=['--request-timeout-ms','30000','--route-ready-ms','30000','--run-id',cell,'--cell-id',cell]
        if kind=='binding':source+=['--target-command-endpoint',ce];target+=['--command-endpoint',ce]
    return sp,tp,tr,source,target

try:
    if not (pathlib.Path('/root/jobs/fx/cx/v21/s033/core124/verdict.md').exists()):raise RuntimeError('gate verdict absent')
    initial_mem=checkmem()
    for role in ['source','target']:
        (D/role).mkdir();(D/role/'control.bin').write_bytes(struct.pack('QQ',0,0))
    sp,tp,tr,sa,ta=specs();b=launch('target',ta,'8-15');waitready(tp,False);a=launch('source',sa,'0-7');dump('ready.json',waitready(sp,True));dump('pids.json',{'source':a.pid,'target':b.pid,'memAvailableKB':initial_mem,'wallTime':datetime.datetime.now(datetime.timezone.utc).isoformat()})
    verify_maps('source',a.pid);verify_maps('target',b.pid)
    base={'runId':cell,'cellId':cell,'pattern':'request-backpressure','payloadBytes':payload,'requestWindow':100,'sendConcurrency':8}
    dump('warmup-start.json',http(tr,'/bench/start',dict(base,phase='warmup',durationMs=WARM*1000)))
    wa=waitidle(sp);dump('warmup-source.json',wa)
    if wa.get('currentInFlight',wa.get('inFlight',0)):raise RuntimeError('warmup outstanding')
    ws,wt=settle(sp,tp,'allPhaseMessages');dump('warmup-target.json',wt)
    if lang=='cpp' and kind=='framework':dump('reset.json',http(tp,'/bench/reset',{}))
    before=time.monotonic_ns();dump('active-start.json',http(tr,'/bench/start',dict(base,phase='active',durationMs=ACTIVE*1000)));after=time.monotonic_ns()
    start=after+WIN_BEGIN*10**9;end=after+WIN_END*10**9
    if mode=='count':
        for role in ['source','target']:
            with (D/role/'control.bin').open('r+b',buffering=0) as control_file:
                control_file.write(struct.pack('QQ',start,end))
    dump('window.json',{'triggerBefore':before,'triggerAfter':after,'begin':start,'end':end,'warmupSeconds':WARM,'activeSeconds':ACTIVE})
    samples=[];native_samples=[];mmin=initial_mem;begin_stats=None;end_stats=None;next_mem=0
    with open(D/'samples.jsonl','w') as samplelog:
        while time.monotonic_ns()<end:
            t=time.monotonic_ns()
            if t>next_mem:mmin=min(mmin,checkmem());next_mem=t+10**9
            if t<start:time.sleep(min(.02,(start-t)/1e9));continue
            if begin_stats is None:begin_stats=http(sp);dump('window-start-source.json',begin_stats)
            x=http(sp);ts=time.monotonic_ns();row={'ns':ts,'stats':x};samplelog.write(json.dumps(row)+'\n');samples.append(row)
            if mode=='count':
                for file in (D/'source').glob('[0-9]*.bin'):
                    if file not in views:views[file]=SampleView(file)
                    native_samples.append({'ns':ts,'stats':views[file].sample()})
            if ts>next_mem:mmin=min(mmin,checkmem());next_mem=ts+10**9
            time.sleep(.01 if mode=='count' else .1)
    end_stats=http(sp);dump('window-end-source.json',end_stats)
    if mode=='count':dump('native-samples.json',native_samples)
    finish=waitidle(sp);dump('finish-source.json',finish);fs,ft=settle(sp,tp,'received');dump('target-stats.json',ft)
    # Keep the source result and add exactly the target count verification used by runner_common.sh.
    result=json.loads((D/'results.json').read_text());c=result['cells'][0];completed=int(c['completed']);errors=int(c['errors']);abandoned=int(c['abandoned']);received=int(ft['received'])
    assert int(ft.get('errors',0))==0 and completed<=received<=completed+errors+abandoned,(c,ft)
    if errors==0 and abandoned==0:assert completed==received,(completed,received)
    if 'submitted' in c:assert int(c['submitted'])==completed+errors+abandoned
    if errors or abandoned or c.get('header_failures',0) or c.get('contaminated',False):raise RuntimeError(f'invalid cell {c}')
    dump('validation.json',{'countsValid':True,'completed':completed,'received':received,'errors':errors,'abandoned':abandoned,'memMinKB':min(mmin,mem_min),'windowBenchmarkCompleted':end_stats['completed']-begin_stats['completed']})
    if mode=='count':
        for role in ['source','target']:
            for file in (D/role).glob('[0-9]*.bin'):dump(f'{role}-counter.json',read(file))
    print(cell,c.get('throughput_per_second'),flush=True)
except BaseException as error:
    dump('failure.json',{'type':type(error).__name__,'message':str(error)});raise
finally:
    for p in reversed(procs):
        if p.poll() is None:
            try:os.killpg(p.pid,signal.SIGTERM)
            except ProcessLookupError:pass
    for p in reversed(procs):
        try:p.wait(timeout=15)
        except subprocess.TimeoutExpired:
            os.killpg(p.pid,signal.SIGINT)
            try:p.wait(timeout=10)
            except subprocess.TimeoutExpired:print('process did not terminate',p.pid,file=sys.stderr)
    for f in logs:f.close()
    for v in views.values():v.close()
