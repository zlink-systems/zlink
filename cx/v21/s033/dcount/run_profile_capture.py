import json, os, pathlib, signal, subprocess, time, urllib.request
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/dcount')
repo=pathlib.Path('/root/worktree/zlink-1639d')
runner=job/'overlay3/framework/bench/grpc/dotnet/run_local.sh'
trace='/root/jobs/fx/cx/v21/p032net/tools/dotnet-trace8/dotnet-trace'
counters='/root/jobs/fx/cx/v21/p032net/tools/dotnet-counters'
for impl,port_src,port_tgt in [('zlink-framework-dotnet',5213,5215),('zlink-dotnet',5206,5209)]:
 tag='profile-'+impl; out=job/'profile-out'/tag; raw=job/'profile-raw'/tag
 out.mkdir(parents=True,exist_ok=True);raw.mkdir(parents=True,exist_ok=True)
 env=os.environ.copy(); env.update(DOTNET_PerfMapEnabled='1',SKIP_BUILD='1',RUNS='1',IMPLEMENTATIONS=impl,PATTERNS='request-backpressure',PAYLOAD_SIZES='1024',WARMUP_SECONDS='30',DURATION_SECONDS='5',RUN_STAMP=tag,OUTPUT_DIR=str(out))
 cmd=['flock','-o','/root/jobs/fx/cx/measure.lock','env']+[f'{k}={env[k]}' for k in ('DOTNET_PerfMapEnabled','SKIP_BUILD','RUNS','IMPLEMENTATIONS','PATTERNS','PAYLOAD_SIZES','WARMUP_SECONDS','DURATION_SECONDS','RUN_STAMP','OUTPUT_DIR')]+['bash',str(runner)]
 with (raw/'runner.log').open('w') as log:
  proc=subprocess.Popen(cmd,cwd=repo,env=env,stdout=log,stderr=subprocess.STDOUT,start_new_session=True)
  def stats(port):
   try:
    with urllib.request.urlopen(f'http://127.0.0.1:{port}/bench/stats',timeout=1) as f:return json.load(f)
   except Exception:return {}
  deadline=time.monotonic()+180
  while time.monotonic()<deadline:
   s=stats(port_src)
   if s.get('phase')=='active':break
   if proc.poll() is not None:raise RuntimeError(f'{tag} runner exited before active: {proc.returncode}')
   time.sleep(.02)
  else:raise RuntimeError(f'{tag} never active')
  def pid(port):
   return int(subprocess.check_output(['lsof','-nP','-t',f'-iTCP:{port}','-sTCP:LISTEN'],text=True).splitlines()[-1])
  pids={'source':pid(port_src),'target':pid(port_tgt)}
  (raw/'pids.json').write_text(json.dumps({'pids':pids,'phase':s},indent=2)+'\n')
  collectors=[]
  for role,p in pids.items():
   f=(raw/f'{role}.nettrace').open('wb')
   c=subprocess.Popen([trace,'collect','-p',str(p),'--profile','cpu-sampling','--providers','Microsoft-Windows-DotNETRuntime:0x4001:5','--duration','00:00:00:08','-o',str(raw/f'{role}.nettrace')],cwd=repo,stdout=(raw/f'{role}.trace.log').open('w'),stderr=subprocess.STDOUT)
   collectors.append((role,c))
   cc=subprocess.Popen([counters,'collect','-p',str(p),'--counters','System.Runtime','--refresh-interval','1','--format','json','--duration','00:00:00:08','-o',str(raw/f'{role}.counters.json')],cwd=repo,stdout=(raw/f'{role}.counters.log').open('w'),stderr=subprocess.STDOUT)
   collectors.append((role+'-counters',cc))
   perf=subprocess.Popen(['perf','stat','-p',str(p),'-e','task-clock,context-switches,raw_syscalls:sys_enter,syscalls:sys_enter_futex','-x,','-o',str(raw/f'{role}.perf.csv')],cwd=repo,stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL)
   collectors.append((role+'-perf',perf))
  ticks={role:{} for role in pids}; started=time.monotonic()
  def snap(p):
   d={}
   for q in pathlib.Path(f'/proc/{p}/task').glob('*/stat'):
    try:
     s=q.read_text();i=s.rfind(')');v=s[i+2:].split();tid=q.parent.name
     name=pathlib.Path(f'/proc/{p}/task/{tid}/comm').read_text().strip();d[tid]=(int(v[11])+int(v[12]),name)
    except Exception:pass
   return d
  first={r:snap(p) for r,p in pids.items()}; end=started
  while stats(port_src).get('phase')=='active' and proc.poll() is None:
   time.sleep(.02)
  end=time.monotonic(); last={r:snap(p) for r,p in pids.items()}
  (raw/'thread-cpu.json').write_text(json.dumps({'elapsed_seconds':end-started,'pids':pids,'ticks_per_second':os.sysconf('SC_CLK_TCK'),'start':first,'end':last},indent=2)+'\n')
  proc.wait()
  for role,c in collectors:
   if role.endswith('-perf') and c.poll() is None:c.send_signal(signal.SIGINT)
   try:c.wait(timeout=20)
   except subprocess.TimeoutExpired:c.send_signal(signal.SIGTERM);c.wait(timeout=10)
  if proc.returncode:raise RuntimeError(f'{tag} runner failed {proc.returncode}')
  print(f'DONE {tag} pids={pids} trace_dir={raw}',flush=True)
