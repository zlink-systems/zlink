import os,sys,json,time,signal,subprocess,urllib.request,hashlib
from pathlib import Path
job=Path('/root/jobs/fx/cx/v21/s033/c4json')
build=Path('/root/worktree/zlink-v21-s033/c4json/build-c4json-ladder')
stage,level,pattern,size=sys.argv[1],sys.argv[2],sys.argv[3],int(sys.argv[4])
out=job/'raw'/f'{stage}-L{level}-{pattern}-{size}'
out.mkdir(exist_ok=False)
procs=[]
files=[]
start=time.monotonic()
scratch=build.parent.with_name('c4json-scratch')
source_time=max(p.stat().st_mtime for top in ['framework/bench/grpc/cpp','framework/languages/cpp','bindings/cpp'] for p in (scratch/top).rglob('*') if p.is_file() and p.suffix in ['.hpp','.cpp','.h'])
artifacts={str(build/n):{'sha256':hashlib.sha256((build/n).read_bytes()).hexdigest(),'newer_than_sources':(build/n).stat().st_mtime>source_time} for n in ['bench_cpp_client','bench_cpp_framework_server']}
assert all(v['newer_than_sources'] for v in artifacts.values()),artifacts
with (job/'progress.md').open('a') as f:
 f.write('\n- BEFORE '+str(out)+': scratch commit 62e766f88d, production base df42a4df40; profile only, call-shape.md; artifacts='+json.dumps(artifacts)+'\n')
(out/'before.json').write_text(json.dumps({'artifacts':artifacts,'scratch_commit':'62e766f88d','worktree_base':'df42a4df40','call_shape':'call-shape.md','cpus':'6-7,14-15'},indent=2))
def expired(*args):raise TimeoutError('19 second profile run limit')
signal.signal(signal.SIGALRM,expired);signal.alarm(19)
def spawn(args,name,env=None):
 f=(out/(name+'.log')).open('w');files.append(f)
 p=subprocess.Popen(['taskset','-c','6-7,14-15']+args,stdout=f,stderr=subprocess.STDOUT,env=env)
 procs.append(p);return p
def http(port,path,data=None):
 req=urllib.request.Request(f'http://127.0.0.1:{port}{path}',data=None if data is None else json.dumps(data).encode(),headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=.5) as r:return json.load(r)
def wait_ready(port):
 while True:
  try:
   r=http(port,'/ready')
   if r.get('ready'):return r
  except (OSError,ValueError):pass
  if time.monotonic()-start>7:raise TimeoutError('startup >7 s')
  time.sleep(.04)
def wait_idle():
 while True:
  r=http(58243,'/bench/stats')
  if r.get('phase') in ('idle','failed'):
   if r.get('failure'):raise RuntimeError(r)
   return r
  time.sleep(.04)
def scheduling(label):
 rows=[]
 for p in procs[:2]:
  for t in Path(f'/proc/{p.pid}/task').iterdir():
   status=(t/'status').read_text()
   cpus=next(l.split(':',1)[1].strip() for l in status.splitlines() if l.startswith('Cpus_allowed_list:'))
   rows.append({'pid':p.pid,'tid':int(t.name),'cpus':cpus,'policy':os.sched_getscheduler(int(t.name))})
 (out/(label+'-scheduling.json')).write_text(json.dumps(rows,indent=2))
try:
 env=dict(os.environ,ZLINK_CPP_LADDER_LEVEL=level)
 target=spawn([str(build/'bench_cpp_framework_server'),'--endpoint','tcp://127.0.0.1:58240','--stats-port','58241'],'target',env)
 wait_ready(58241)
 source=spawn([str(build/'bench_cpp_client'),'--implementation','zlink-framework-cpp','--endpoint','tcp://127.0.0.1:58240','--target-stats-port','58241','--trigger-port','58242','--stats-port','58243','--pattern',pattern,'--payload-size',str(size),'--warmup-seconds','1','--warmup-segments','1','--output-file',str(out/'cells.json')],'source',env)
 wait_ready(58242)
 trigger={'runId':'profile-'+stage,'cellId':f'L{level}-{pattern}-{size}','pattern':pattern,'payloadBytes':size,'phase':'warmup','durationMs':1000,'requestWindow':100,'submitConcurrency':1}
 http(58242,'/bench/start',trigger);wait_idle()
 http(58241,'/bench/reset',{})
 scheduling('start')
 perf=spawn(['perf','record','-e','cpu-clock:u','-F','499','-g','--call-graph','fp','-o',str(out/'perf.data'),'-p',f'{source.pid},{target.pid}','--','taskset','-c','6-7,14-15','sleep','5.5'],'perf')
 time.sleep(.12)
 trigger.update(phase='active',durationMs=4000)
 http(58242,'/bench/start',trigger)
 time.sleep(2)
 scheduling('mid')
 result=wait_idle()
 (out/'source-stats.json').write_text(json.dumps(result,indent=2))
 (out/'target-stats.json').write_text(json.dumps(http(58241,'/bench/stats'),indent=2))
 perf.wait(timeout=6)
 (out/'metadata.json').write_text(json.dumps({'stage':stage,'commit':'62e766f88dc6d1feefdff76e6132185019db0bb6','profile_only':True,'elapsed_seconds':time.monotonic()-start,'affinity':sorted(os.sched_getaffinity(0)),'binaries':{str(build/n):hashlib.sha256((build/n).read_bytes()).hexdigest() for n in ['bench_cpp_client','bench_cpp_framework_server']}},indent=2))
 print(out)
finally:
 signal.alarm(0)
 for p in reversed(procs):
  if p.poll() is None:p.terminate()
 deadline=time.monotonic()+1
 for p in reversed(procs):
  try:p.wait(timeout=max(.01,deadline-time.monotonic()))
  except subprocess.TimeoutExpired:p.kill();p.wait()
 for f in files:f.close()
