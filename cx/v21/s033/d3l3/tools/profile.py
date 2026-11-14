import sys,os,time,json,pathlib,subprocess,urllib.request,signal,threading,hashlib
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/d3l3');scratch=pathlib.Path('/root/worktree/zlink-v21-s033/d3l3-scratch')
label,level,pattern,size=sys.argv[1:];size=int(size);bench=scratch/'bin'/level
out=job/'raw'/label;out.mkdir(parents=True,exist_ok=False)
os.sched_setaffinity(0,{6,7,14,15})
env=os.environ.copy();env.update(ZLINK_DOTNET_LADDER_LEVEL=level,ZLINK_DOTNET_LADDER_CLIENT='raw',ZLINK_DOTNET_RAW_TARGET='framework',DOTNET_gcServer='0')
procs=[];logs=[];timers=[];starts={}
def dump(name,value):(out/name).write_text(json.dumps(value,indent=2))
def spawn(cmd,name):
 f=open(out/(name+'.log'),'w');logs.append(f)
 p=subprocess.Popen(['taskset','-c','6-7,14-15']+cmd,stdout=f,stderr=subprocess.STDOUT,env=env,start_new_session=True);procs.append(p);starts[p.pid]=time.monotonic()
 timer=threading.Timer(19.5,lambda:kill(p));timer.daemon=True;timer.start();timers.append(timer)
 return p
def kill(p):
 if p.poll() is None:
  try:os.killpg(p.pid,signal.SIGKILL)
  except ProcessLookupError:pass

def get(port,path='/bench/stats'):
 with urllib.request.urlopen('http://127.0.0.1:'+str(port)+path,timeout=.8) as r:return json.load(r)
def ready(port,deadline):
 while time.monotonic()<deadline:
  try:
   s=get(port)
   if s.get('ready',True):return s
  except Exception:pass
  time.sleep(.05)
 raise RuntimeError('not ready '+str(port))
def start(phase,seconds):
 data=dict(runId=label,cellId=label,pattern=pattern,payloadBytes=size,phase=phase,durationMs=seconds*1000,requestWindow=100,submitConcurrency=8)
 q=urllib.request.Request('http://127.0.0.1:5435/bench/start',data=json.dumps(data).encode(),headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(q,timeout=1) as r:return json.load(r)
def done(deadline):
 while time.monotonic()<deadline:
  s=get(5436)
  if s['phase'] in ('completed','idle','failed'):
   if s['phase']=='failed':raise RuntimeError(s)
   return s
  time.sleep(.05)
 raise RuntimeError('phase timeout')
def snapshot(name):
 rows=[]
 for role,p in [('source',source),('target',target)]:
  for t in pathlib.Path(f'/proc/{p.pid}/task').iterdir():
   try:
    status=t.joinpath('status').read_text();aff=[l.split(':')[1].strip() for l in status.splitlines() if l.startswith('Cpus_allowed_list:')][0]
    st=t.joinpath('stat').read_text();st=st[st.rfind(')')+2:].split()
    rows.append(dict(role=role,pid=p.pid,tid=int(t.name),name=t.joinpath('comm').read_text().strip(),state=st[0],utime=int(st[11]),stime=int(st[12]),processor=int(st[36]),affinity=aff,policy=os.sched_getscheduler(int(t.name)),timestamp=time.time()))
   except (FileNotFoundError,ProcessLookupError):pass
 dump(name+'-threads.json',rows)
 assert all(set(os.sched_getaffinity(r['tid'])) <= {6,7,14,15} for r in rows),'bad affinity'
 return rows
head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip();dirty=subprocess.check_output(['git','status','--porcelain'],text=True).strip()
dump('fingerprint.json',dict(head=head,dirty=dirty,level=level,pattern=pattern,payload=size,gcServer='0',artifacts=[dict(path=str(p),sha256=hashlib.sha256(p.read_bytes()).hexdigest(),mtime=p.stat().st_mtime) for p in bench.rglob('*') if p.is_file() and (p.name in ['Zlink.Framework.dll','Zlink.Framework.UnitTests.dll','Systems.Zlink.dll'] or '.so.' in p.name)]))
starttime=time.monotonic();failure=None
try:
 target=spawn(['dotnet',str(bench/'ZLinkServer/Zlink.Framework.UnitTests.dll'),'--endpoint','tcp://127.0.0.1:5444','--metrics-url','http://127.0.0.1:5445'],'target')
 ready(5445,starttime+8)
 source=spawn(['dotnet',str(bench/'Client/Zlink.Framework.UnitTests.dll'),'--implementation','zlink-dotnet','--scenario',pattern,'--payload-size',str(size),'--request-window','100','--submit-concurrency','8','--warmup-seconds','1','--trigger-url','http://127.0.0.1:5435','--stats-url','http://127.0.0.1:5436','--target-endpoint','tcp://127.0.0.1:5444','--target-command-endpoint','tcp://127.0.0.1:5444','--target-stats-url','http://127.0.0.1:5445','--raw-socket','router','--output',str(out),'--configuration','Release','--timeout-seconds','18','--drain-bound-ms','2000'],'source')
 ready(5436,starttime+10);dump('pids.json',dict(source=source.pid,target=target.pid));snapshot('start')
 for role,p in [('source',source),('target',target)]:spawn(['/root/jobs/fx/cx/v21/s033/d2l2/tools/dotnet8/dotnet-trace','collect','--process-id',str(p.pid),'--profile','cpu-sampling,gc-verbose','--duration','00:00:09','--output',str(out/(role+'.nettrace'))],role+'-trace')
 time.sleep(.6)
 dump('warmup-trigger.json',start('warmup',1));dump('warmup-stats.json',done(starttime+14))
 before=snapshot('before-active');ack=start('active',5);dump('active-trigger.json',ack)
 wall=ack.get('receivedAtUnixMs',int(time.time()*1000));dump('window.json',dict(startUnixMs=wall+100,endUnixMs=wall+4900))
 time.sleep(2);snapshot('mid-active');time.sleep(3.05);after=snapshot('after-active')
 active=done(starttime+18.7);targetStats=get(5445);dump('active-stats.json',active);dump('target-stats.json',targetStats)
 assert active['errors']==0 and active['submitted']==active['completed']==targetStats['received'] and targetStats['errors']==0,(active,targetStats)
 cpu=[];old={(r['role'],r['tid']):r for r in before};hz=os.sysconf('SC_CLK_TCK')
 for r in after:
  if (r['role'],r['tid']) in old:
   b=old[(r['role'],r['tid'])];seconds=r['timestamp']-b['timestamp'];cpu.append(dict(role=r['role'],tid=r['tid'],name=r['name'],cpuMs=(r['utime']+r['stime']-b['utime']-b['stime'])*1000/hz,wallMs=seconds*1000,corePercent=(r['utime']+r['stime']-b['utime']-b['stime'])/hz/seconds*100))
 dump('thread-cpu.json',sorted(cpu,key=lambda r:-r['cpuMs']))
 for p in procs[2:]:p.wait(timeout=max(.1,19.2-(time.monotonic()-starttime)))
 assert all(p.returncode==0 for p in procs[2:]),'trace failed'
except BaseException as e:
 failure=repr(e);dump('failure.json',dict(error=failure));raise
finally:
 life=[]
 for p in procs:
  life.append(dict(pid=p.pid,elapsed=time.monotonic()-starts[p.pid],preKillRc=p.poll()));kill(p)
 for p in procs:p.wait()
 for t in timers:t.cancel()
 for f in logs:f.close()
 dump('lifecycle.json',dict(elapsed=time.monotonic()-starttime,processes=life,returncodes=[p.returncode for p in procs],failure=failure))
