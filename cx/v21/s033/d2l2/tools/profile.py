import sys,os,time,json,pathlib,subprocess,urllib.request,signal,threading,hashlib
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/d2l2')
bench=pathlib.Path('/root/worktree/zlink-v21-s033/d2l2-scratch/framework/bench/grpc/dotnet')
label,level,pattern,size=sys.argv[1:]; size=int(size)
if label.startswith('before-'): bench=pathlib.Path('/root/worktree/zlink-v21-s033/d2l2-scratch/before-bin')
out=job/'raw'/label;out.mkdir(parents=True,exist_ok=False)
env=os.environ.copy();env.update(ZLINK_DOTNET_LADDER_LEVEL=level,ZLINK_DOTNET_LADDER_CLIENT='raw',ZLINK_DOTNET_RAW_TARGET='framework')
procs=[];logs=[]
def spawn(cmd,name,trace=False):
 f=open(out/(name+'.log'),'w');logs.append(f)
 p=subprocess.Popen(cmd,stdout=f,stderr=subprocess.STDOUT,env=env,start_new_session=True);procs.append(p)
 if not trace:
  threading.Timer(19,lambda: kill(p)).start()
 return p
def kill(p):
 if p.poll() is None:
  try:os.killpg(p.pid,signal.SIGKILL)
  except ProcessLookupError:pass
def get(port,path='/bench/stats'):
 with urllib.request.urlopen('http://127.0.0.1:'+str(port)+path,timeout=.4) as r:return json.load(r)
def waitready(port,deadline):
 while time.monotonic()<deadline:
  try:
   s=get(port)
   if s.get('ready',True):return s
  except Exception:pass
  time.sleep(.05)
 raise RuntimeError('not ready '+str(port))
def start(phase,seconds):
 data=dict(runId=label,cellId=label,pattern=pattern,payloadBytes=size,phase=phase,durationMs=seconds*1000,requestWindow=100,submitConcurrency=8)
 q=urllib.request.Request('http://127.0.0.1:5325/bench/start',data=json.dumps(data).encode(),headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(q,timeout=1) as r:return json.load(r)
def done(deadline):
 while time.monotonic()<deadline:
  s=get(5326)
  if s['phase'] in ('completed','idle','failed'):
   if s['phase']=='failed':raise RuntimeError(s)
   return s
  time.sleep(.05)
 raise RuntimeError('phase timeout')
def snapshot(label):
 rows=[]
 for role,p in [('source',source),('target',target)]:
  for t in pathlib.Path(f'/proc/{p.pid}/task').iterdir():
   try:
    status=t.joinpath('status').read_text();aff=[l.split(':')[1].strip() for l in status.splitlines() if l.startswith('Cpus_allowed_list:')][0]
    rows.append(dict(role=role,pid=p.pid,tid=int(t.name),affinity=aff,policy=os.sched_getscheduler(int(t.name))))
   except FileNotFoundError:pass
 (out/(label+'-threads.json')).write_text(json.dumps(rows,indent=2))
 assert all(set(os.sched_getaffinity(r['tid'])) <= {6,7,14,15} and r['policy']==0 for r in rows), 'affinity/policy outside profile contract'
artifacts=[]
for project in ('Client','ZLinkServer'):
 for name in ('WithGrpcBench.'+project+'.dll','Zlink.Framework.dll','Systems.Zlink.dll'):
  p=(bench/project/name if label.startswith('before-') else bench/project/'bin/Release/net8.0'/name)
  artifacts.append(dict(path=str(p),mtime=p.stat().st_mtime,sha256=hashlib.sha256(p.read_bytes()).hexdigest()))
  if name=='Zlink.Framework.dll' and not label.startswith('before-'):
   assert p.stat().st_mtime >= pathlib.Path('/root/worktree/zlink-v21-s033/d2l2/framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs').stat().st_mtime
 native=(bench/project if label.startswith('before-') else bench/project/'bin/Release/net8.0')/'runtimes/linux-x64/native/libzlink.so.1.25.0'
 artifacts.append(dict(path=str(native),sha256=hashlib.sha256(native.read_bytes()).hexdigest()))
head=subprocess.check_output(['git','rev-parse','HEAD'],text=True).strip();base=subprocess.check_output(['git','merge-base','HEAD','origin/main'],text=True).strip();dirty=subprocess.check_output(['git','status','--porcelain'],text=True).strip()
(out/'fingerprint.json').write_text(json.dumps(dict(head=head,base=base,dirty=dirty,artifacts=artifacts,builtManifest='raw/before-built-manifest.json' if label.startswith('before-') else None),indent=2))
with (job/'progress.md').open('a') as f:
 f.write('\n- profile '+label+': HEAD='+head+' dirty='+str(bool(dirty))+' base='+base+'. Binary hashes: raw/'+label+'/fingerprint.json. Receive/reply parity: call-shape.md; L1=permit+raw header, L2=같은 경로+요청/응답 header. Body encode/copy·wait·metrics 동일.\n')
starttime=time.monotonic()
try:
 target=spawn(['dotnet',str((bench/'ZLinkServer/WithGrpcBench.ZLinkServer.dll') if label.startswith('before-') else bench/'ZLinkServer/bin/Release/net8.0/WithGrpcBench.ZLinkServer.dll'),'--endpoint','tcp://127.0.0.1:5334','--metrics-url','http://127.0.0.1:5335'],'target')
 waitready(5335,starttime+8)
 source=spawn(['dotnet',str((bench/'Client/WithGrpcBench.Client.dll') if label.startswith('before-') else bench/'Client/bin/Release/net8.0/WithGrpcBench.Client.dll'),'--implementation','zlink-dotnet','--scenario',pattern,'--payload-size',str(size),'--request-window','100','--submit-concurrency','8','--warmup-seconds','3','--trigger-url','http://127.0.0.1:5325','--stats-url','http://127.0.0.1:5326','--target-endpoint','tcp://127.0.0.1:5334','--target-command-endpoint','tcp://127.0.0.1:5334','--target-stats-url','http://127.0.0.1:5335','--raw-socket','router','--output',str(out),'--configuration','Release','--timeout-seconds','30','--drain-bound-ms','2000'],'source')
 waitready(5326,starttime+10)
 (out/'pids.json').write_text(json.dumps(dict(source=source.pid,target=target.pid)));snapshot('start')
 if 'warm' not in label:
  for role,p in [('source',source),('target',target)]:
   spawn([str(job/'tools/dotnet8/dotnet-trace'),'collect','--process-id',str(p.pid),'--profile','cpu-sampling','--providers','Microsoft-Windows-DotNETRuntime:0x1:5','--duration','00:00:07','--output',str(out/(role+'.nettrace'))],role+'-trace',True)
 (out/'warmup-trigger.json').write_text(json.dumps(start('warmup',3)))
 warm=done(starttime+16);(out/'warmup-stats.json').write_text(json.dumps(warm,indent=2))
 phaseStart=int(time.time()*1000)
 ack=start('active',4)
 (out/'active-trigger.json').write_text(json.dumps(ack,indent=2))
 (out/'window.json').write_text(json.dumps(dict(startUnixMs=phaseStart+100,endUnixMs=phaseStart+4000),indent=2))
 time.sleep(1);snapshot('mid-active');time.sleep(3.2)
 previous=None;stable=0
 while time.monotonic()<starttime+18.3:
  active=get(5326)
  if active['phase']=='failed':raise RuntimeError(active)
  key=(active['submitted'],active['completed'],active['errors'],active['currentInFlight'])
  stable=stable+1 if key==previous and active['currentInFlight']==0 and active['submitted']==active['completed'] else 0
  if active['phase']=='idle' or stable>=2:break
  previous=key;time.sleep(.1)
 else:raise RuntimeError('active counters did not settle within profile cap')
 (out/'active-stats.json').write_text(json.dumps(active,indent=2));(out/'target-stats.json').write_text(json.dumps(get(5335),indent=2))
 assert active['errors']==0 and active['submitted']==active['completed']==get(5335)['received'] and get(5335)['errors']==0
 for p in procs[2:]:
  try:p.wait(timeout=max(.1,18.5-(time.monotonic()-starttime)))
  except subprocess.TimeoutExpired:kill(p)
 if any(p.poll()!=0 for p in procs[2:]):raise RuntimeError('trace did not finish within profile cap')
finally:
 for p in procs:kill(p)
 for p in procs:p.wait()
 for f in logs:f.close()
 (out/'lifecycle.json').write_text(json.dumps(dict(elapsed=time.monotonic()-starttime,returncodes=[p.returncode for p in procs]),indent=2))
