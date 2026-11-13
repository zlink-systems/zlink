import pathlib, subprocess, os, time, json, urllib.request, signal, sys
J=pathlib.Path(__file__).parent

def http(port,path='/bench/stats',body=None):
 data=None if body is None else json.dumps(body).encode()
 with urllib.request.urlopen(urllib.request.Request(f'http://127.0.0.1:{port}{path}',data=data,headers={'content-type':'application/json'}),timeout=2) as r:return json.load(r)

def wait(port,children,ready=False,idle=False):
 until=time.monotonic()+35
 while time.monotonic()<until:
  if any(p.poll() is not None for p in children):raise RuntimeError('endpoint exited')
  try:
   s=http(port)
   if s.get('phase')=='failed':raise RuntimeError(s)
   if (not ready or s.get('ready')) and (not idle or s.get('phase')=='idle'):return s
  except (OSError,urllib.error.URLError):pass
  time.sleep(.05)
 raise TimeoutError(port)

for n in map(int,sys.argv[1:] or range(1,10)):
 out=J/'smoke'/f'rung{n}';out.mkdir(parents=True,exist_ok=True);children=[];handles=[]
 try:
  full=n==9;impl='zlink-framework-cpp' if full else 'zlink-cpp';tp=5295 if full else 5289;sp=5293 if full else 5286;trigger=5292 if full else 5285;endpoint='tcp://127.0.0.1:'+str(5294 if full else 5287)
  env={**os.environ,'LADDER_RUNG':str(n)}
  for role,args,cpus in [('target',[str(J/'build'/f'rung{n}'/('bench_cpp_framework_server' if full else 'bench_cpp_zlink_server')),'--endpoint',endpoint,'--command-endpoint','tcp://127.0.0.1:5288','--stats-port',str(tp)],'8-15'),('source',[str(J/'build'/f'rung{n}'/'bench_cpp_client'),'--implementation',impl,'--pattern','request-backpressure','--payload-size','1024','--trigger-port',str(trigger),'--stats-port',str(sp),'--target-stats-port',str(tp),'--endpoint',endpoint,'--output-file',str(out/'results.json'),'--warmup-seconds','1','--warmup-segments','1','--request-timeout-ms','30000','--drain-bound-ms','30000'],'0-7')]:
   h=open(out/(role+'.log'),'w');handles.append(h);children.append(subprocess.Popen(['taskset','-c',cpus]+args,env=env,stdout=h,stderr=h,cwd=J));wait(tp if role=='target' else sp,children,role=='source')
  for phase in ['warmup','active']:
   if phase=='active':http(tp,'/bench/reset',{})
   http(trigger,'/bench/start',{'runId':'smoke','cellId':f'rung{n}','pattern':'request-backpressure','payloadBytes':1024,'phase':phase,'durationMs':1000,'requestWindow':100,'sendConcurrency':8})
   state=wait(sp,children,idle=True)
  c=json.loads((out/'results.json').read_text())['cells'][0];target=http(tp)
  assert c['completed']>0 and c['errors']==0 and c['abandoned']==0 and target['errors']==0,(c,target)
  # Allow target metrics snapshot to settle, then prove exact echo count.
  assert c['completed']==target['received'],(c['completed'],target)
  (out/'target.json').write_text(json.dumps(target,indent=2));print(n,c['throughput_per_second'],c['completed'],'OK',flush=True)
 finally:
  for p in children:
   if p.poll() is None:p.send_signal(signal.SIGTERM)
  for p in children:
   try:p.wait(timeout=15)
   except subprocess.TimeoutExpired:raise RuntimeError(f'TERM did not stop {p.pid}')
  for h in handles:h.close()
