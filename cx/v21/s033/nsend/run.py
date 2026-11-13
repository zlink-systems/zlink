import hashlib
import argparse,json,os,pathlib,signal,subprocess,time,urllib.request
ROOT=pathlib.Path('/root/worktree/zlink-nsend'); HERE=ROOT/'framework/bench/grpc/node'; JOB=pathlib.Path(__file__).parent
p=argparse.ArgumentParser();p.add_argument('--out',required=True);p.add_argument('--runs',type=int,default=3);p.add_argument('--start-run',type=int,default=1);p.add_argument('--warmup',type=int,default=10);p.add_argument('--duration',type=int,default=20);p.add_argument('--patterns',default='send-saturation,request-backpressure,request-serial');p.add_argument('--sizes',default='1024,4096');p.add_argument('--impls',default='framework,binding');p.add_argument('--capture',default='none');p.add_argument('--preload');p.add_argument('--rate',type=float,default=0);p.add_argument('--match-framework-rate',action='store_true');args=p.parse_args()
assert 'd961e51e231234e8404d74164d2a27c0cf2fce41' in (HERE/'client/main.js').read_text(), 'Apply nsend/measurement.diff before replay; it disables git metadata commands'
OUT=JOB/args.out;OUT.mkdir(parents=True,exist_ok=True)
from guard import Guard
condition_guard=Guard(OUT)
def mem():
 m=int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')));assert m>=12582912,m;return m
def req(url,data=None):
 r=urllib.request.Request(url,data=None if data is None else json.dumps(data).encode(),headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(r,timeout=6) as f:return json.load(f)
def wait(url,fn,children,timeout=300):
 end=time.monotonic()+timeout
 while time.monotonic()<end:
  for c in children:assert c.poll() is None,('child exited',c.args,c.returncode)
  try:
   d=req(url+'/bench/stats')
   assert d.get('phase')!='failed',d
   if fn(d):return d
  except (OSError,ValueError):pass
  time.sleep(.1)
 raise TimeoutError(url)
def snapshot(pids,urls):
 d={'monotonic':time.monotonic(),'stats':{r:req(u+'/bench/stats') for r,u in urls.items()},'processes':{},'memavailable_kb':mem()}
 for r,pid in pids.items():
  threads={}
  for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
   try:
    s=(t/'stat').read_text();f=s[s.rindex(')')+2:].split();threads[t.name]={'comm':(t/'comm').read_text().strip(),'runtime_ns':int((t/'schedstat').read_text().split()[0]),'utime_ticks':int(f[11]),'stime_ticks':int(f[12])}
   except FileNotFoundError:pass
  d['processes'][r]={'pid':pid,'threads':threads,'affinity':sorted(os.sched_getaffinity(pid))}
 return d
def save(path,data):path.write_text(json.dumps(data,indent=2)+'\n')
def settle(urls,children,key='received'):
 prior=None;stable=0;end=time.monotonic()+30
 while time.monotonic()<end:
  s=req(urls['source']+'/bench/stats');t=req(urls['target']+'/bench/stats');counts=(s.get('completed'),s.get('inFlight'),t.get(key),t.get('errors'),t.get('rejected'))
  stable=stable+1 if counts==prior else 0
  if stable>=2:return t
  prior=counts;time.sleep(.1)
 raise TimeoutError('settle')
for run in range(args.start_run,args.start_run+args.runs):
 for pattern in args.patterns.split(','):
  for size in map(int,args.sizes.split(',')):
   for impl in args.impls.split(','):
    mem();cell=f'zlink-{"framework-" if impl=="framework" else ""}node-{pattern}-{size}';out=OUT/f'run{run}'/cell;out.mkdir(parents=True,exist_ok=True)
    base=5232 if impl=='framework' else 5225;urls={'source':f'http://127.0.0.1:{base+1}','target':f'http://127.0.0.1:{base+3 if impl=="framework" else base+4}'};trigger=f'http://127.0.0.1:{base}'
    ep=f'tcp://127.0.0.1:{base+2}';cmd_ep=f'tcp://127.0.0.1:{base+3}'
    target=['node',f'zlink-{impl if impl=="framework" else "raw"}-server/main.js','--endpoint',ep,'--metrics-url',urls['target']]
    if impl=='binding':target+=['--command-endpoint',cmd_ep]
    source=['node','client/main.js','--implementation',f'zlink-{"framework-" if impl=="framework" else ""}node','--scenario',pattern,'--payload-size',str(size),'--request-window','100','--send-concurrency','8','--latency-sample-limit','200000','--warmup-seconds',str(args.warmup),'--drain-bound-ms','30000','--request-timeout-ms','30000','--route-ready-ms','30000','--trigger-url',trigger,'--stats-url',urls['source'],'--target-endpoint',ep,'--target-stats-url',urls['target'],'--raw-socket','router','--output',str(out),'--report-file','report.txt']
    if impl=='binding':source+=['--target-command-endpoint',cmd_ep]
    effective_rate=args.rate
    if args.match_framework_rate:
     effective_rate=0
     if impl=='binding':
      ref=OUT/f'run{run}'/f'zlink-framework-node-{pattern}-{size}'
      rb=json.loads((ref/'before.json').read_text());ra=json.loads((ref/'after.json').read_text())
      effective_rate=(ra['stats']['target']['received']-rb['stats']['target']['received'])/(ra['monotonic']-rb['monotonic'])
    env=dict(os.environ);env['NODE_PATH']=str(ROOT/'framework/languages/node/node_modules');env['NSEND_RATE']=str(effective_rate)
    if args.preload:env['NODE_OPTIONS']=env.get('NODE_OPTIONS','')+' --require='+str(JOB/args.preload)
    if args.capture in ('cpu','heap','diag'):env['NODE_OPTIONS']=env.get('NODE_OPTIONS','')+' --inspect=127.0.0.1:0 --perf-basic-prof'
    if args.capture=='profile':env['NODE_OPTIONS']=env.get('NODE_OPTIONS','')+' --perf-basic-prof'
    children=[];handles=[]
    print('START',out,'MemAvailableKiB',mem(),time.strftime('%FT%T%z'),flush=True)
    try:
     for role,cmd,cpus in [('target',target,'8-15'),('source',source,'0-7')]:
      e=dict(env);e['NSEND_ROLE']=role;e['NSEND_DIAG_FILE']=str(out/(role+'-diag.json'));h=open(out/(role+'.log'),'w');handles.append(h);c=subprocess.Popen(['taskset','-c',cpus]+cmd,cwd=HERE,env=e,stdout=h,stderr=h,start_new_session=True);children.append(c);wait(urls[role],lambda d:role=='target' or d.get('ready') is True,children,45)
     pids={'target':children[0].pid,'source':children[1].pid};save(out/'commands.json',{'target':target,'source':source,'pids':pids,'affinity':{r:sorted(os.sched_getaffinity(pid)) for r,pid in pids.items()},'loaded_native':{r:[line.split()[-1] for line in pathlib.Path(f'/proc/{pid}/maps').read_text().splitlines() if 'zlink.node' in line or 'libzlink.so' in line] for r,pid in pids.items()},'raw_port_sha256':hashlib.sha256((ROOT/'framework/languages/node/packages/framework/dist/runtime/backend/node/node-raw-binding-port.js').read_bytes()).hexdigest(),'capture':args.capture,'rate':effective_rate,'match_framework_rate':args.match_framework_rate})
     for phase,seconds in [('warmup',args.warmup),('active',args.duration)]:
      req(trigger+'/bench/start',{'runId':f'nsend-{args.out}-{run}','cellId':cell,'pattern':pattern,'payloadBytes':size,'phase':phase,'durationMs':seconds*1000,'requestWindow':100,'sendConcurrency':8})
      if phase=='active':
       wait(urls['source'],lambda d:d.get('phase')=='active',children,10)
       if args.capture!='none':
        subprocess.run(['python3',str(JOB/'collect.py'),'--mode',args.capture,'--source',str(pids['source']),'--target',str(pids['target']),'--source-url',urls['source'],'--target-url',urls['target'],'--out',str(out/'capture')],check=True)
       else:
        time.sleep(5);before=snapshot(pids,urls);time.sleep(max(1,args.duration-6));after=snapshot(pids,urls);save(out/'before.json',before);save(out/'after.json',after)
      s=wait(urls['source'],lambda d:d.get('phase')=='idle',children)
      assert s.get('currentInFlight',s.get('inFlight',0))==0,s
      t=settle(urls,children,'allPhaseMessages' if phase=='warmup' else 'received')
      if phase=='active':
       save(out/'target-stats.json',t);result=json.loads((out/'results.json').read_text());c=result['cells'][0];c['target_stats']=t;save(out/'results.json',result)
       assert c['errors']==c['abandoned']==t['errors']==0,c
       assert c['submitted']==c['completed']+c['errors']+c['abandoned'],c
       if pattern=='send-saturation' and 'rejected' in t:assert t['rejected'] is not None,t
       assert c['completed']==t['received']+(t.get('rejected') or 0),c
       assert not c.get('drain_bound_hit',False),c
       print('END',cell,'run',run,'msg/s',c['throughput_per_second'],'completed',c['completed'],'received',t['received'],flush=True)
    finally:
     for c in children:
      if c.poll() is None:os.killpg(c.pid,signal.SIGTERM)
     for c in children:
      try:c.wait(timeout=15)
      except subprocess.TimeoutExpired:
       os.killpg(c.pid,signal.SIGINT);c.wait(timeout=15)
     for h in handles:h.close()

# Resolve native and JIT stacks while retaining the job's exclusive measurement lock.
if args.capture=='profile':
 for cap in sorted(OUT.glob('run*/*/capture')):
  for role in ('source','target'):
   with open(cap/f'{role}.perf.script','w') as handle:
    subprocess.run(['perf','script','-i',str(cap/f'{role}.perf.data'),'-F','comm,pid,tid,time,event,ip,sym,dso'],stdout=handle,check=True)
