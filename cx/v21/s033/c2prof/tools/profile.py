from pathlib import Path
import os,sys,time,json,subprocess,threading,signal,urllib.request,select,hashlib
J=Path('/root/jobs/fx/cx/v21/s033/c2prof'); W=Path('/root/worktree/zlink-v21-s033/cfx'); B=W/'build-cfx'
os.environ['DEBUGINFOD_URLS']=''; os.environ['PERF_BUILDID_DIR']=str(J/'tools/perf-buildid')

def http(port,path='/bench/stats',data=None):
    req=urllib.request.Request(f'http://127.0.0.1:{port}'+path,data=None if data is None else json.dumps(data).encode(),headers={'Content-Type':'application/json'})
    with urllib.request.urlopen(req,timeout=1) as r:return json.load(r)
def save(p,x):p.write_text(json.dumps(x,indent=2))
def waitfor(f,seconds=4):
    end=time.monotonic()+seconds
    while time.monotonic()<end:
        try:
            x=f()
            if x:return x
        except (OSError,ValueError):pass
        time.sleep(.03)
    raise RuntimeError('wait timeout')
def proc(pid):
    ts={}
    for t in Path(f'/proc/{pid}/task').glob('*'):
        try:
            st={l.split(':',1)[0]:l.split(':',1)[1].strip() for l in (t/'status').read_text().splitlines() if ':' in l}
            ts[t.name]={'comm':(t/'comm').read_text().strip(),'state':st['State'],'affinity':st['Cpus_allowed_list'],'runtime_ns':int((t/'schedstat').read_text().split()[0]),'voluntary':int(st['voluntary_ctxt_switches']),'involuntary':int(st['nonvoluntary_ctxt_switches'])}
        except FileNotFoundError:pass
    return {'pid':pid,'threads':ts}
class Perf:
    def __init__(self,args,path):
        r,self.w=os.pipe();self.r,a=os.pipe()
        self.log=open(str(path)+'.log','w')
        cmd=['perf']+args+['--delay=-1','--control',f'fd:{r},{a}']
        self.p=subprocess.Popen(cmd,pass_fds=(r,a),stdout=self.log,stderr=self.log)
        self.cmd=cmd;os.close(r);os.close(a)
    def control(self,s):
        os.write(self.w,(s+'\n').encode())
        if not select.select([self.r],[],[],3)[0]:raise RuntimeError('perf no ack '+str(self.cmd))
        return os.read(self.r,100).decode()
    def stop(self):
        if self.p.poll() is None:self.p.send_signal(signal.SIGINT)
        self.p.wait(timeout=5);self.log.close();os.close(self.w);os.close(self.r)

def run(level,pattern,rep,mode='perf',freq=2999):
    label=f'{mode}'+('-final' if mode=='perf' else '-v2' if mode.startswith('alloc') else '')+f'-L{level}-{pattern}-r{rep}'; d=J/'raw'/label;d.mkdir(exist_ok=False)
    processes={};perfs=[];timers=[];env=dict(os.environ,ZLINK_CPP_LADDER_LEVEL=str(level))
    if mode.startswith('alloc'):
        env['C2_PRELOAD_LIBRARY']=str(J/'tools/libwindow-counter.so')
    try:
        args={'target':[str(B/'bench_cpp_framework_server'),'--endpoint','tcp://127.0.0.1:5294','--stats-port','5295'],
              'source':[str(B/'bench_cpp_client'),'--implementation','zlink-framework-cpp','--pattern',pattern,'--payload-size','1024','--trigger-port','5292','--stats-port','5293','--target-stats-port','5295','--endpoint','tcp://127.0.0.1:5294','--output-file',str(d/'results.json'),'--warmup-seconds','1','--warmup-segments','10','--request-timeout-ms','30000','--drain-bound-ms','30000']}
        for side in ['target','source']:
            e=env.copy()
            if mode.startswith('alloc'):
                e['C2_COUNTER']=str(d/(side+'.counter'));e['C2_STACKS']=str(d/(side+'.alloc-stacks.bin')) if mode=='alloc-stacks' else ''
            f=open(d/(side+'.log'),'w');p=subprocess.Popen(['taskset','-c','6-7,14-15']+(['env','LD_PRELOAD='+env['C2_PRELOAD_LIBRARY']] if mode.startswith('alloc') else [])+args[side],cwd=J,env=e,stdout=f,stderr=f,start_new_session=True);processes[side]=p
            # Hard KILL does not issue SIGCONT (GNU timeout normally can).
            timer=threading.Timer(20,lambda p=p:os.kill(p.pid,signal.SIGKILL) if p.poll() is None else None);timer.start();timers.append(timer)
            waitfor(lambda:http(5295 if side=='target' else 5293).get('ready',True))
        save(d/'launch.json',{'level':level,'pattern':pattern,'rep':rep,'mode':mode,'utc':time.time(),'commands':args,'pids':{s:p.pid for s,p in processes.items()},'affinity':'6-7,14-15 shared','hard_timeout_seconds':20})
        def phase(name,dur):return http(5292,'/bench/start',{'runId':label,'cellId':label,'pattern':pattern,'payloadBytes':1024,'phase':name,'durationMs':dur,'requestWindow':100,'submitConcurrency':8})
        if mode=='perf':
            for side,p in processes.items():
                perfs.append(Perf(['record','-q','-e','cpu-clock','-F',str(freq),'-g','--call-graph','dwarf,8192','--clockid','mono','-m','256','--no-buildid','-o',str(d/(side+'.perf.data')),'-p',str(p.pid)],d/(side+'.record')))
            for pf in perfs:pf.control('enable');pf.control('disable')
        save(d/'warmup-start.json',phase('warmup',1000));waitfor(lambda:http(5293).get('phase')=='idle',4)
        time.sleep(.25);save(d/'warmup-source.json',http(5293));save(d/'warmup-target.json',http(5295));http(5295,'/bench/reset',{})
        save(d/'active-start.json',phase('active',5000));active=time.monotonic();time.sleep(.35)
        if mode=='perf':
            for side,p in processes.items():
                tids=','.join(proc(p.pid)['threads'])
                perfs.append(Perf(['stat','--per-thread','-t',tids,'-e','task-clock,context-switches,cpu-migrations','-x',';','-o',str(d/(side+'.stat.csv'))],d/(side+'.stat')))
            for pf in perfs:pf.control('enable')
        if mode in ('trace','stat','threadstat'):
            for side,p in processes.items():
                perfs.append(Perf(['stat']+(['--per-thread','-t',','.join(proc(p.pid)['threads'])] if mode=='threadstat' else ['-p',str(p.pid)])+['-e',('syscalls:sys_enter_futex,syscalls:sys_enter_sched_yield,syscalls:sys_enter_epoll_wait,task-clock,context-switches,cpu-migrations' if mode=='trace' else 'task-clock,context-switches,cpu-migrations'),'-x',';','-o',str(d/(side+'.syscalls.csv'))],d/(side+'.trace')))
            for pf in perfs:pf.control('enable')
        counters={}
        if mode.startswith('alloc'):
            import mmap
            for side in processes:
                f=open(d/(side+'.counter'),'r+b');counters[side]=mmap.mmap(f.fileno(),0)
            for m in counters.values():m[0:4]=(1).to_bytes(4,'little')
        before={'monotonic':time.monotonic(),'monotonic_raw':time.clock_gettime(time.CLOCK_MONOTONIC_RAW),'stats':{'source':http(5293),'target':http(5295)},'processes':{s:proc(p.pid) for s,p in processes.items()}}
        save(d/'before.json',before)
        until=min(active+4.65,before['monotonic']+4.0)
        timeline=[]
        while time.monotonic()<until:
            timeline.append({'monotonic':time.monotonic(),'processes':{s:proc(p.pid) for s,p in processes.items()}})
            time.sleep(min(.1,max(0,until-time.monotonic())))
        after={'monotonic':time.monotonic(),'monotonic_raw':time.clock_gettime(time.CLOCK_MONOTONIC_RAW),'stats':{'source':http(5293),'target':http(5295)},'processes':{s:proc(p.pid) for s,p in processes.items()}}
        if mode.startswith('alloc'):
            for m in counters.values():m[0:4]=(0).to_bytes(4,'little')
        if mode in ('perf','trace','stat','threadstat'):
            for pf in perfs:pf.control('disable')
        save(d/'after.json',after);save(d/'timeline.json',timeline)
        for side,p in processes.items():(d/(side+'.maps')).write_text(Path(f'/proc/{p.pid}/maps').read_text())
        for pf in perfs:pf.stop()
        perfs=[]
        waitfor(lambda:http(5293).get('phase')=='idle',3);time.sleep(.25)
        src=http(5293);tar=http(5295);save(d/'source-final.json',src);save(d/'target-final.json',tar)
        result=json.loads((d/'results.json').read_text());c=result['cells'][0]
        assert c['errors']==tar['errors']==0,(c,tar)
        assert c['submitted']==c['completed']==tar['received'],(c,tar)
        assert c.get('abandoned',0)==0
        assert not c.get('drain_bound_hit',False)
        states=[t['state'] for snap in [before,after]+timeline for pr in snap['processes'].values() for t in pr['threads'].values()]
        assert not any(x.startswith('T') for x in states),'measurement guard pause observed; reject run'
        if mode.startswith('alloc'):
            assert all(int.from_bytes(m[4:8],'little')>0 for m in counters.values()),'counter has no registered thread'
        save(d/'validated.json',{'submitted':c['submitted'],'completed':c['completed'],'received':tar['received'],'errors':0,'window_source_completed':after['stats']['source']['completed']-before['stats']['source']['completed'],'window_target_received':after['stats']['target']['received']-before['stats']['target']['received'],'seconds':after['monotonic']-before['monotonic']})
        print(label,json.loads((d/'validated.json').read_text()),flush=True)
    except BaseException as e:
        save(d/'failure.json',{'error':str(e)});raise
    finally:
        for pf in perfs:
            try:pf.stop()
            except Exception:pass
        for timer in timers:timer.cancel()
        for side,p in processes.items():
            if p.poll() is None:p.kill()
            p.wait()
        time.sleep(3)
if __name__=='__main__':
    mode=sys.argv[1] if len(sys.argv)>1 else 'perf'
    if len(sys.argv)>2:run(int(sys.argv[2]),sys.argv[3],int(sys.argv[4]),mode)
    else:
        for rep in [1,2] if mode=='perf' else [1]:
            for pattern in ['request-backpressure','send-saturation']:
                for level in ([1,2] if rep==1 else [2,1]):run(level,pattern,rep,mode)
