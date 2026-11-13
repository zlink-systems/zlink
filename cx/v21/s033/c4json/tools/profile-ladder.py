import os,sys,json,time,signal,subprocess,urllib.request,hashlib,socket,random
from pathlib import Path
job=Path('/root/jobs/fx/cx/v21/s033/c4json')
build=Path('/root/worktree/zlink-v21-s033/c4json/build-c4json-ladder')
stage,level,pattern,size=sys.argv[1],sys.argv[2],sys.argv[3],int(sys.argv[4])
out=job/'raw'/f'{stage}-L{level}-{pattern}-{size}'
out.mkdir(exist_ok=False)
procs=[]
files=[]
metadata_start=time.monotonic()
scratch=build.parent.with_name('c4json-scratch')
source_time=max(p.stat().st_mtime for top in ['framework/bench/grpc/cpp','framework/languages/cpp','bindings/cpp'] for p in (scratch/top).rglob('*') if p.is_file() and p.suffix in ['.hpp','.cpp','.h'])
artifacts={str(build/n):{'sha256':hashlib.sha256((build/n).read_bytes()).hexdigest(),'newer_than_sources':(build/n).stat().st_mtime>source_time} for n in ['bench_cpp_client','bench_cpp_framework_server']}
assert all(v['newer_than_sources'] for v in artifacts.values()),artifacts
with (job/'progress.md').open('a') as f:
 f.write('\n- BEFORE '+str(out)+': scratch commit 62e766f88d, production base df42a4df40; profile only, call-shape.md; artifacts='+json.dumps(artifacts)+'\n')
for attempt in range(100):
 reservations=[]
 try:
  base_port=random.randrange(20000,30000,4)
  for offset in range(4):
   reservation=socket.socket();reservations.append(reservation)
   reservation.bind(('127.0.0.1',base_port+offset))
  break
 except OSError:
  pass
 finally:
  for reservation in reservations:reservation.close()
else:raise RuntimeError('no free profile port block')
target_stats_port=base_port+1;trigger_port=base_port+2;source_stats_port=base_port+3
(out/'before.json').write_text(json.dumps({'artifacts':artifacts,'scratch_commit':'62e766f88d','worktree_base':'df42a4df40','call_shape':'call-shape.md','cpus':'6-7,14-15','port_base':base_port},indent=2))
start=time.monotonic()
events=[];readiness=[]
def expired(*args):raise TimeoutError('19 second profile run limit')
signal.signal(signal.SIGALRM,expired);signal.alarm(19)
def spawn(args,name,env=None):
 f=(out/(name+'.log')).open('w');files.append(f)
 p=subprocess.Popen(['taskset','-c','6-7,14-15']+args,stdout=f,stderr=subprocess.STDOUT,env=env)
 procs.append(p);events.append({'event':'spawn '+name,'seconds':time.monotonic()-start,'pid':p.pid});return p
def http(port,path,data=None):
 req=urllib.request.Request(f'http://127.0.0.1:{port}{path}',data=None if data is None else json.dumps(data).encode(),headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=.5) as r:return json.load(r)
def wait_ready(port,process):
 while True:
  if process.poll() is not None:raise RuntimeError(f'process for {port} exited {process.returncode}')
  try:
   r=http(port,'/ready')
   readiness.append({'port':port,'seconds':time.monotonic()-start,'response':r})
   if r.get('ready'):
    events.append({'event':'ready '+str(port),'seconds':time.monotonic()-start});return r
  except (OSError,ValueError):pass
  if time.monotonic()-start>7:raise TimeoutError('startup >7 s')
  time.sleep(.04)
def wait_idle():
 while True:
  r=http(source_stats_port,'/bench/stats')
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
 target=spawn([str(build/'bench_cpp_framework_server'),'--endpoint',f'tcp://127.0.0.1:{base_port}','--stats-port',str(target_stats_port)],'target',env)
 wait_ready(target_stats_port,target)
 source=spawn([str(build/'bench_cpp_client'),'--implementation','zlink-framework-cpp','--endpoint',f'tcp://127.0.0.1:{base_port}','--target-stats-port',str(target_stats_port),'--trigger-port',str(trigger_port),'--stats-port',str(source_stats_port),'--pattern',pattern,'--payload-size',str(size),'--warmup-seconds','1','--warmup-segments','1','--output-file',str(out/'cells.json')],'source',env)
 wait_ready(trigger_port,source)
 trigger={'runId':'profile-'+stage,'cellId':f'L{level}-{pattern}-{size}','pattern':pattern,'payloadBytes':size,'phase':'warmup','durationMs':1000,'requestWindow':100,'submitConcurrency':1}
 http(trigger_port,'/bench/start',trigger);wait_idle()
 http(target_stats_port,'/bench/reset',{})
 trigger.update(phase='active',durationMs=4000)
 http(trigger_port,'/bench/start',trigger)
 while http(source_stats_port,'/bench/stats')['submitted']==0:time.sleep(.01)
 scheduling('start')
 task_map={str(p.pid):[int(t.name) for t in Path(f'/proc/{p.pid}/task').iterdir()] for p in [source,target]}
 (out/'perf-task-map.json').write_text(json.dumps(task_map,indent=2))
 tids=','.join(str(tid) for ts in task_map.values() for tid in ts)
 perf=spawn(['perf','record','-e','cpu-clock:u','-F','499','-g','--call-graph','fp','-o',str(out/'perf.data'),'-t',tids,'--','taskset','-c','6-7,14-15','sleep','3.5'],'perf')
 time.sleep(2)
 scheduling('mid')
 result=wait_idle()
 target_result=http(target_stats_port,'/bench/stats')
 while target_result['received'] < result['completed']:
  time.sleep(.04)
  target_result=http(target_stats_port,'/bench/stats')
 (out/'source-stats.json').write_text(json.dumps(result,indent=2))
 (out/'target-stats.json').write_text(json.dumps(target_result,indent=2))
 perf.wait(timeout=6)
 (out/'metadata.json').write_text(json.dumps({'stage':stage,'commit':'62e766f88dc6d1feefdff76e6132185019db0bb6','profile_only':True,'elapsed_seconds':time.monotonic()-start,'affinity':sorted(os.sched_getaffinity(0)),'binaries':{str(build/n):hashlib.sha256((build/n).read_bytes()).hexdigest() for n in ['bench_cpp_client','bench_cpp_framework_server']}},indent=2))
 print(out)
finally:
 (out/'startup-events.json').write_text(json.dumps({'metadata_seconds':start-metadata_start,'events':events,'readiness':readiness,'port_base':base_port,'seconds':time.monotonic()-start,'statuses':[(p.pid,p.poll()) for p in procs]},indent=2))
 signal.alarm(0)
 for p in reversed(procs):
  if p.poll() is None:p.terminate()
 deadline=time.monotonic()+1
 for p in reversed(procs):
  try:p.wait(timeout=max(.01,deadline-time.monotonic()))
  except subprocess.TimeoutExpired:p.kill();p.wait()
 for f in files:f.close()
