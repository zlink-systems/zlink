import argparse,json,os,pathlib,signal,subprocess,time,urllib.request,concurrent.futures
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/jsend'); J=pathlib.Path('/root/jobs/fx/cx/v21/s033/jenv'); W=pathlib.Path('/root/worktree/zlink-jsend')
p=argparse.ArgumentParser();p.add_argument('case');p.add_argument('--rate',type=int,default=0);p.add_argument('--smoke',action='store_true');p.add_argument('--collect',default='none');a=p.parse_args()
raw=a.case.startswith('binding'); impl='zlink-java' if raw else 'zlink-framework-java'
build=P/'prototype-build' if a.case.startswith('original') else J/'main-build' if a.case.startswith(('main','binding')) else W/'framework/bench/grpc/java'
out=P/'runs'/a.case;out.mkdir(parents=True,exist_ok=True)
base=5245 if raw else 5252
su=f'http://127.0.0.1:{base+1}';tu=f'http://127.0.0.1:{base+4 if raw else base+3}';trigger=f'http://127.0.0.1:{base}'
endpoint=f'tcp://127.0.0.1:{base+2}'
env=os.environ.copy();env['JAVA_HOME']='/usr/lib/jvm/java-25-openjdk-amd64';env['PATH']=str(J/'runner/bin')+':'+env['JAVA_HOME']+'/bin:'+env['PATH']
env['ZLINK_JAVA_BENCH_REQUEST_RATE']=str(a.rate) if a.rate else ''
if not a.rate:env.pop('ZLINK_JAVA_BENCH_REQUEST_RATE',None)
children=[];handles=[];mems=[]
def mem():
 n=int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')));mems.append(n);assert n>=12582912,n

def http(url,data=None):
 req=urllib.request.Request(url,None if data is None else json.dumps(data).encode(),headers={'content-type':'application/json'})
 with urllib.request.urlopen(req,timeout=300 if data is not None else 8) as r:return json.load(r)
def stats(url):return http(url+'/bench/stats')
def wait(url,pred,seconds=300):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  mem()
  try:
   s=stats(url)
   if pred(s):return s
   if s.get('phase')=='failed':raise RuntimeError(s)
  except OSError:pass
  for ch in children:
   if ch.poll() is not None:raise RuntimeError(('child exited',ch.args,ch.returncode))
  time.sleep(.2)
 raise TimeoutError(url)
def start(cmd,name):
 (out/(name+'.command.json')).write_text(json.dumps(cmd));h=open(out/(name+'.log'),'w');handles.append(h)
 ch=subprocess.Popen(cmd,env=env,stdout=h,stderr=h,start_new_session=True);children.append(ch);return ch

def phase(name,seconds):
 r=http(trigger+'/bench/start',{'runId':'jsend-'+a.case,'cellId':impl+'-request-backpressure-1024','pattern':'request-backpressure','payloadBytes':1024,'phase':name,'durationMs':seconds*1000,'requestWindow':100,'sendConcurrency':8});print(name,r,flush=True)

def collect(mode):
 cmd=['python3',str(P/'tools/collect.py'),'--mode',mode,'--source',str(source.pid),'--target',str(target.pid),'--source-url',su,'--target-url',tu,'--out',str(out/'capture')]
 subprocess.run(cmd,env=env,check=True)

def onterm(*args):raise KeyboardInterrupt()
signal.signal(signal.SIGTERM,onterm)
try:
 mem()
 module='zlink-raw-server' if raw else 'zlink-framework-server';name='bench-zlink-raw-server' if raw else 'bench-zlink-framework-server'
 args=['--endpoint',endpoint,'--metrics-url',tu]
 if raw:args+=['--command-endpoint',f'tcp://127.0.0.1:{base+3}']
 target=start(['taskset','-c','8-15',str(build/module/'build/install'/name/'bin'/name)]+args,'target');wait(tu,lambda s:True,40)
 args=['--implementation',impl,'--scenario','request-backpressure','--payload-size','1024','--request-window','100','--send-concurrency','8','--latency-sample-limit','200000','--warmup-seconds','30','--drain-bound-ms','30000','--request-timeout-ms','30000','--route-ready-ms','30000','--trigger-url',trigger,'--stats-url',su,'--target-endpoint',endpoint,'--target-stats-url',tu,'--run-id','jsend-'+a.case,'--cell-id',impl+'-request-backpressure-1024','--raw-socket','router','--output',str(out),'--report-file','report.txt']
 if raw:args+=['--target-command-endpoint',f'tcp://127.0.0.1:{base+3}']
 source=start(['taskset','-c','0-7',str((J/'main-counter-build' if a.case.startswith('main') and a.rate else build)/'client/build/install/bench-client/bin/bench-client')]+args,'source');wait(su,lambda s:s.get('ready')==True,45)
 print('pids',source.pid,target.pid,flush=True)
 if os.environ.get('JSEND_ISOLATE_IO')=='1':
  pins={}
  for role,pid in [('source',source.pid),('target',target.pid)]:
   cpus=[0,2] if role=='source' else [8,10];app=set(range(4,8)) if role=='source' else set(range(12,16))
   for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
    comm=(t/'comm').read_text().strip();aff=app
    if 'IO/' in comm:
     i=int(comm.rsplit('/',1)[1]);aff={cpus[0 if i in (0,3) else 1]}
    os.sched_setaffinity(int(t.name),aff);pins[t.name]={'role':role,'comm':comm,'cpus':sorted(aff)}
  (out/'pins.json').write_text(json.dumps(pins,indent=2))
 pool=concurrent.futures.ThreadPoolExecutor(max_workers=1)
 warmup=pool.submit(phase,'warmup',5 if a.smoke else 30)
 # A loopback capture during warmup avoids perturbing the active throughput.
 cap=start(['tcpdump','-i','lo','-nn','-s','0','-U','-c','300','-w',str(out/'wire.pcap'),'tcp','port',str(base+2)],'tcpdump')
 cap.wait(timeout=15);children.remove(cap)
 warmss=[];lastss=0
 while not warmup.done():
  mem()
  if time.monotonic()-lastss>2:
   lastss=time.monotonic()
   warmss.append({'time':lastss,'ss':subprocess.check_output(['ss','-tinmeo','( sport = :'+str(base+2)+' or dport = :'+str(base+2)+' )'],text=True)})
  time.sleep(.2)
 (out/'warmup-ss.json').write_text(json.dumps(warmss,indent=2))
 warmup.result()
 wait(su,lambda s:s.get('phase')=='idle')
 assert stats(su).get('currentInFlight',stats(su).get('inFlight',0))==0
 time.sleep(.5)
 if a.collect=='handoff':
  subprocess.run([env['JAVA_HOME']+'/bin/jcmd',str(target.pid),'JFR.start','name=jenv','settings=profile','duration=60s','filename='+str(out/'target.jfr')],env=env,check=True)
 active=pool.submit(phase,'active',5 if a.smoke else 60)
 wait(su,lambda s:s.get('phase')=='active',5)
 if a.collect != 'none':
  if a.collect=='handoff':
   with open(out/'target-threads.txt','w') as f:subprocess.run([env['JAVA_HOME']+'/bin/jcmd',str(target.pid),'Thread.print'],stdout=f,stderr=f,check=True)
  collect(a.collect)
 while not active.done():mem();time.sleep(.2)
 active.result()
 pool.shutdown()
 wait(su,lambda s:s.get('phase')=='idle')
 if not a.smoke:time.sleep(1)
 t=stats(tu);s=stats(su);(out/'target-stats.json').write_text(json.dumps(t,indent=2));(out/'source-stats.json').write_text(json.dumps(s,indent=2))
 r=json.load(open(out/'results.json'));c=r['cells'][0];c['target_stats']={'received':t['received'],'errors':t['errors']}
 assert c['submitted']==c['completed']==t['received'],(c,t)
 assert c['errors']==c['abandoned']==t['errors']==0,(c,t)
 (out/'results.json').write_text(json.dumps(r,indent=2));print('RESULT',a.case,c['throughput_per_second'],'completed',c['completed'],'caller_us',c['jvm_thread_cores']*1e6/c['throughput_per_second'],flush=True)
finally:
 for ch in children:
  if ch.poll() is None:os.killpg(ch.pid,signal.SIGTERM)
 for ch in children:
  try:ch.wait(timeout=30)
  except subprocess.TimeoutExpired:
   os.killpg(ch.pid,signal.SIGINT);ch.wait(timeout=30)
 for h in handles:h.close()
 (out/'runner-meta.json').write_text(json.dumps({'case':a.case,'rate':a.rate,'min_memavailable_kb':min(mems),'exit_codes':[ch.returncode for ch in children]},indent=2))
