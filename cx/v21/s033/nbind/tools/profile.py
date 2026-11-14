#!/usr/bin/env python3
import argparse, hashlib, json, mmap, os, pathlib, signal, socket, struct, subprocess, threading, time, urllib.request
ROOT=pathlib.Path('/root/worktree/zlink-v21-s033/nbind')
JOB=pathlib.Path('/root/jobs/fx/cx/v21/s033/nbind')
BENCH=ROOT/'framework/bench/grpc/node'
CPUS='6-7,14-15'
a=argparse.ArgumentParser()
a.add_argument('mode',choices=['audit','cpu','alloc','plain'])
a.add_argument('level',choices=['L1','L2','L3'])
a.add_argument('pattern',choices=['send-saturation','request-backpressure'])
a.add_argument('label')
args=a.parse_args()
out=JOB/'raw'/args.label
out.mkdir(exist_ok=False)
def git(*x):return subprocess.check_output(['git',*x],cwd=ROOT,text=True).strip()
meta=dict(head=git('rev-parse','HEAD'),branch=git('branch','--show-current'),dirty=git('status','--short'),mode=args.mode,level=args.level,pattern=args.pattern,cpus=CPUS)
(out/'build.json').write_text(json.dumps(meta,indent=2)+'\n')
ports=[]
for _ in range(7):
 with socket.socket() as s:s.bind(('127.0.0.1',0));ports.append(s.getsockname()[1])
endpoint,command,target,trigger,source,tc,sc=ports
url=lambda p:f'http://127.0.0.1:{p}'
env={k:v for k,v in os.environ.items() if 'LADDER' not in k and k not in ['NODE_OPTIONS','LD_PRELOAD','ZLINK_LOCAL_PACKAGE_ROOT','ZLinkLocalPackageRoot']}
env.update(NODE_PATH=str(ROOT/'framework/languages/node/node_modules'),ZLINK_LADDER_L0_SERVER='1',ZLINK_LADDER_L0_CLIENT='1',NBIND_MODE=args.mode,NBIND_OUT=str(out))
if args.level=='L1':env.update(ZLINK_LADDER_L1_PERMIT='1')
if args.level=='L2':env.update(ZLINK_LADDER_L2_ENVELOPE='1',ZLINK_LADDER_L2_CLIENT='1')
if args.level=='L3':env.update(ZLINK_LADDER_L3_SERVICE_WIRE='1',ZLINK_LADDER_L3_CLIENT='1')
if args.mode=='alloc':env.update(NBIND_ALLOC_DIR=str(out))
procs=[];perfprocs=[];logs=[];timers=[];events=[]
started=time.monotonic()
def event(name,**v):events.append(dict(event=name,monotonic_us=time.monotonic_ns()//1000,epoch_ms=time.time()*1000,**v))
def request(port,route='/bench/stats',body=None,timeout=.7):
 req=urllib.request.Request(url(port)+route,data=None if body is None else json.dumps(body).encode(),headers={'content-type':'application/json'})
 with urllib.request.urlopen(req,timeout=timeout) as r:return json.load(r)
def spawn(role,cmd,control):
 log=open(out/f'{role}.log','w');logs.append(log)
 flags=['--perf-basic-prof','--interpreted-frames-native-stack'] if args.mode=='cpu' else []
 launch_env=['env','LD_PRELOAD='+str(JOB/'tools/alloc-counter.so')] if args.mode=='alloc' else []
 proc=subprocess.Popen(['taskset','-c',CPUS,*launch_env,'node',*flags,'--require',str(JOB/'tools/measure-preload.cjs'),*cmd],cwd=BENCH,env=dict(env,NBIND_ROLE=role,NBIND_CONTROL_PORT=str(control)),stdout=log,stderr=subprocess.STDOUT)
 procs.append((role,proc,time.monotonic()))
 timer=threading.Timer(19.4,lambda:proc.kill() if proc.poll() is None else None);timer.daemon=True;timer.start();timers.append(timer)
 event('spawn',role=role,pid=proc.pid)
 return proc
def ready(port):
 while True:
  if time.monotonic()-started>8:raise TimeoutError('readiness deadline')
  if any(p.poll() is not None for _,p,_ in procs):raise RuntimeError('early process exit')
  try:
   data=request(port)
   if data.get('ready'):return data
  except (OSError,ValueError):pass
  time.sleep(.04)
def threads(tag):
 data={}
 for role,p,_ in procs:
  entries=[]
  for t in pathlib.Path(f'/proc/{p.pid}/task').glob('*'):
   try:
    entries.append(dict(tid=int(t.name),policy=os.sched_getscheduler(int(t.name)),affinity=sorted(os.sched_getaffinity(int(t.name))),stat=t.joinpath('stat').read_text(),wchan=t.joinpath('wchan').read_text()))
   except FileNotFoundError:pass
  data[role]=entries
 (out/f'threads-{tag}.json').write_text(json.dumps(data,indent=2)+'\n')
def alloc_snapshot(tag):
 if args.mode!='alloc':return
 all={}
 for role,p,_ in procs:
  file=out/f'alloc-{p.pid}.bin'
  buf=file.read_bytes();magic,version,n=struct.unpack_from('QQQ',buf)
  assert magic==0x435050414c4c4f43 and version==1
  all[role]=[dict(zip(['tid','malloc','calloc','realloc','free','bytes'],struct.unpack_from('QQQQQQ',buf,4096+64*i))) for i in range(min(n,4096))]
 (out/f'alloc-{tag}.json').write_text(json.dumps(all,indent=2)+'\n')
def phase(name,duration):
 event(name+'-begin')
 ack=request(trigger,'/bench/start',dict(runId=args.label,cellId=args.label,pattern=args.pattern,payloadBytes=1024,phase=name,durationMs=duration,requestWindow=100,submitConcurrency=8))
 if not ack.get('accepted'):raise RuntimeError(ack)
 mid=False
 while True:
  if time.monotonic()-started>17.5:raise TimeoutError('phase deadline')
  stats=request(source)
  if stats['phase']=='failed':raise RuntimeError(stats)
  if stats['phase']=='idle':break
  if name=='active' and not mid and time.monotonic()-phase_start>2.5:threads('active');mid=True
  time.sleep(.06)
 event(name+'-end')
 (out/f'{name}-source.json').write_text(json.dumps(stats,indent=2)+'\n')
 ts=request(target)
 (out/f'{name}-target.json').write_text(json.dumps(ts,indent=2)+'\n')
 assert stats['errors']==ts['errors']==0,(stats,ts)
 assert stats['submitted']==stats['completed']==ts['allPhaseMessages' if name=='warmup' else 'received'],(stats,ts)
 return stats
try:
 tp=spawn('target',['zlink-framework-server/main.js','--endpoint',f'tcp://127.0.0.1:{endpoint}','--command-endpoint',f'tcp://127.0.0.1:{command}','--metrics-url',url(target)],tc)
 sp=spawn('source',['client/main.js','--implementation','zlink-framework-node','--scenario',args.pattern,'--payload-size','1024','--warmup-seconds','1','--request-window','100','--submit-concurrency','8','--trigger-url',url(trigger),'--stats-url',url(source),'--target-endpoint',f'tcp://127.0.0.1:{endpoint}','--target-command-endpoint',f'tcp://127.0.0.1:{command}','--target-stats-url',url(target),'--output',str(out)],sc)
 ready(target);ready(source);threads('ready')
 phase_start=time.monotonic();phase('warmup',1000)
 request(target,'/bench/reset',{})
 for role,p,_ in procs:
  if args.mode=='cpu':
   for kind,cmd in [('stat',['perf','stat','-e','task-clock','-x',';','-o',str(out/f'{role}-perf-stat.txt'),'-p',str(p.pid)]),('record',['perf','record','-e','cpu-clock','-F','399','--call-graph','dwarf,8192','-o',str(out/f'{role}.perf.data'),'-p',str(p.pid)])]:
    log=open(out/f'{role}-perf-{kind}.log','w');logs.append(log)
    pp=subprocess.Popen(['taskset','-c',CPUS,*cmd],stdout=log,stderr=subprocess.STDOUT)
    perfprocs.append((role,kind,pp))
 time.sleep(.12 if args.mode=='cpu' else .01)
 for port in [tc,sc]:request(port,'/begin',timeout=2)
 alloc_snapshot('start');threads('active-start')
 phase_start=time.monotonic();stats=phase('active',5000)
 alloc_snapshot('end')
 for port in [sc,tc]:request(port,'/end',timeout=2)
 for role,kind,pp in perfprocs:
  if pp.poll() is None:pp.send_signal(signal.SIGINT)
 for role,kind,pp in perfprocs:pp.wait(timeout=2)
 for role,p,_ in procs:
  (out/f'{role}-maps.txt').write_text(pathlib.Path(f'/proc/{p.pid}/maps').read_text())
  m=pathlib.Path(f'/tmp/perf-{p.pid}.map')
  if m.exists():(out/f'{role}-perf.map').write_bytes(m.read_bytes())
 event('valid',messages=stats['completed'])
except Exception as e:
 event('failure',error=repr(e));raise
finally:
 for _,_,pp in perfprocs:
  if pp.poll() is None:pp.send_signal(signal.SIGINT)
 for role,p,born in reversed(procs):
  if p.poll() is None:p.terminate()
 for role,p,born in reversed(procs):
  try:p.wait(timeout=max(.01,19.4-(time.monotonic()-born)))
  except subprocess.TimeoutExpired:p.kill();p.wait()
  event('exit',role=role,pid=p.pid,returncode=p.returncode,lifetime_s=time.monotonic()-born)
 for timer in timers:timer.cancel()
 for f in logs:f.close()
 (out/'events.json').write_text(json.dumps(events,indent=2)+'\n')
print(out,flush=True)
