# Adapted from jsend/runner/run-instrument.py and cstage/tools/measure.py.
import argparse,copy,hashlib,json,os,pathlib,signal,subprocess,time,urllib.request
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/creply');W=pathlib.Path('/root/worktree/zlink-creply')
p=argparse.ArgumentParser();p.add_argument('case');p.add_argument('--variant',choices=['main','direct','binding'],default='main');p.add_argument('--rate',type=int,default=0);p.add_argument('--warmup',type=int,default=30);p.add_argument('--active',type=int,default=60);p.add_argument('--payload',type=int,default=1024);p.add_argument('--pattern',default='request-backpressure');p.add_argument('--collect',default='none');p.add_argument('--mutex-stat',action='store_true');a=p.parse_args()
raw=a.variant=='binding';impl='zlink-cpp' if raw else 'zlink-framework-cpp';build=W/'framework/bench/grpc/cpp/build';binaries=P/('bin-main' if a.variant=='binding' else 'bin-'+a.variant)
out=P/'runs'/a.case;out.mkdir(parents=True,exist_ok=True)
base=5245 if raw else 5252;sp=base+1;tp=base+4 if raw else base+3;port=base+2
su=f'http://127.0.0.1:{sp}';tu=f'http://127.0.0.1:{tp}';trigger=f'http://127.0.0.1:{base}';endpoint=f'tcp://127.0.0.1:{port}'
env=os.environ.copy();env.pop('ZLINK_CPP_BENCH_REQUEST_RATE',None)
env['LD_LIBRARY_PATH']='/root/.cache/zlink/core/1.23.0/linux-x64/lib:'+env.get('LD_LIBRARY_PATH','')
if a.rate:env['ZLINK_CPP_BENCH_REQUEST_RATE']=str(a.rate)
children=[];handles=[];mems=[]
def mem():
 n=int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')));mems.append(n);assert n>=12582912,n

def http(url,data=None):
 req=urllib.request.Request(url,None if data is None else json.dumps(data).encode(),headers={'content-type':'application/json'})
 with urllib.request.urlopen(req,timeout=8) as r:return json.load(r)
def stats(url):return http(url+'/bench/stats')
def wait(url,pred,seconds=120):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  mem()
  for ch in children:
   if ch.poll() is not None:raise RuntimeError(('child exited',ch.args,ch.returncode))
  try:
   s=stats(url)
   if s.get('phase')=='failed':raise RuntimeError(s)
   if pred(s):return s
  except OSError:pass
  time.sleep(.1)
 raise TimeoutError(url)
def start(cmd,name):
 (out/(name+'.command.json')).write_text(json.dumps(cmd));h=open(out/(name+'.log'),'w');handles.append(h)
 childenv=env.copy()
 if name=='target' and a.mutex_stat:
  sym='_ZN5zlink9framework6detail7backend16raw_route_port_t5replyERKNS2_14raw_received_tESt6vectorINS_9message_tESaIS8_EE'
  row=next(l.split() for l in subprocess.check_output(['nm','-S',str(targetbin)],text=True).splitlines() if l.endswith(' '+sym))
  lo,size=int(row[0],16),int(row[1],16)
  childenv.update(LD_PRELOAD=str(P/'tools/creply_mutex.so'),CREPLY_LOCK_RANGE=f'{lo:x}-{lo+size:x}',CREPLY_LOCK_OUTPUT=str(out/'mutex-stats.json'))
 ch=subprocess.Popen(cmd,env=childenv,stdout=h,stderr=h,start_new_session=True);children.append(ch);return ch

def mutex_snapshot():
 lib=P/'tools/creply_mutex.so'
 symbols={}
 for line in subprocess.check_output(['nm','-S',str(lib)],text=True).splitlines():
  v=line.split()
  if len(v)==4 and v[3] in ('calls','contended','wait_ns','errors'):symbols[v[3]]=int(v[0],16)
 maps=pathlib.Path(f'/proc/{target.pid}/maps').read_text().splitlines()
 base=int(next(l.split()[0].split('-')[0] for l in maps if str(lib) in l and l.split()[2]=='00000000'),16)
 fd=os.open(f'/proc/{target.pid}/mem',os.O_RDONLY)
 try:
  return {**{k:int.from_bytes(os.pread(fd,8,base+address),'little') for k,address in symbols.items()},'time':time.monotonic(),'stats':stats(tu)}
 finally:os.close(fd)

def phase(name,seconds):
 r=http(trigger+'/bench/start',{'runId':'creply-'+a.case,'cellId':impl+'-'+a.pattern+'-'+str(a.payload),'pattern':a.pattern,'payloadBytes':a.payload,'phase':name,'durationMs':seconds*1000,'requestWindow':100,'sendConcurrency':8});print(name,r,flush=True)
 wait(su,lambda s:s.get('phase')=='active' if name=='active' else s.get('phase')=='warmup',5)
def verify(r,t):
 c=r['cells'][0];assert c['submitted']==c['completed']==t['received'],(c,t)
 assert c['errors']==c['abandoned']==c['header_validation_failures']==t['errors']==0,(c,t)
 assert not c.get('contaminated') and not c.get('drain_bound_hit')
 assert t.get('rejected',0) in (0,None)
 return c

def onterm(*args):raise KeyboardInterrupt()
signal.signal(signal.SIGTERM,onterm);signal.signal(signal.SIGINT,onterm)
try:
 mem()
 expected=json.loads((P/'inputs.json').read_text())
 for f in ('/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so','/root/.cache/zlink/zlink-cpp/1.23.0/lib/libzlink_cpp.a'):
  assert hashlib.sha256(pathlib.Path(f).read_bytes()).hexdigest()==expected[f],f
 targetbin=binaries/('bench_cpp_zlink_server' if raw else 'bench_cpp_framework_server')
 args=['--endpoint',endpoint,'--stats-port',str(tp)]
 if raw:args+=['--command-endpoint',f'tcp://127.0.0.1:{base+3}']
 target=start(['taskset','-c','8-15',str(targetbin)]+args,'target');wait(tu,lambda s:True,40)
 args=['--implementation',impl,'--pattern',a.pattern,'--payload-size',str(a.payload),'--warmup-seconds',str(a.warmup),'--warmup-segments','10','--drain-bound-ms','30000','--request-timeout-ms','30000','--trigger-port',str(base),'--stats-port',str(sp),'--target-stats-port',str(tp),'--endpoint',endpoint,'--output-file',str(out/'results.json')]
 if raw:args+=['--command-endpoint',f'tcp://127.0.0.1:{base+3}']
 # Keep source main in every Framework case: isolate target reply intervention.
 sourcebin=P/'bin-main/bench_cpp_client'
 source=start(['taskset','-c','0-7',str(sourcebin)]+args,'source');wait(su,lambda s:s.get('ready')==True,60)
 meta={'arguments':vars(a),'pids':{'source':source.pid,'target':target.pid},'sha256':{str(f):hashlib.sha256(f.read_bytes()).hexdigest() for f in (sourcebin,targetbin)}}
 for role,ch in [('source',source),('target',target)]:
  maps=pathlib.Path(f'/proc/{ch.pid}/maps').read_text();meta[role+'_maps']=maps
  loaded=sorted(set(l.split()[-1] for l in maps.splitlines() if '/libzlink.so' in l))
  assert loaded and all(pathlib.Path(f).resolve()==pathlib.Path('/root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so').resolve() for f in loaded),loaded
  meta[role+'_loaded_core']={f:hashlib.sha256(pathlib.Path(f).read_bytes()).hexdigest() for f in loaded}
  meta[role+'_thread_affinities']={}
  for t in pathlib.Path(f'/proc/{ch.pid}/task').iterdir():
   mask=next(l.split(':',1)[1].strip() for l in (t/'status').read_text().splitlines() if l.startswith('Cpus_allowed_list:'))
   selected=set()
   for span in mask.split(','):
    ends=span.split('-');selected.update(range(int(ends[0]),int(ends[-1])+1))
   assert selected and selected<=set(range(8) if role=='source' else range(8,16)),(role,t.name,mask)
   meta[role+'_thread_affinities'][t.name]=mask
 (out/'meta.json').write_text(json.dumps(meta,indent=2))
 print('PIDS',source.pid,target.pid,flush=True)
 phase('warmup',a.warmup)
 cap=start(['tcpdump','-i','lo','-nn','-s','0','-U','-c','200','-w',str(out/'wire.pcap'),'tcp','port',str(port)],'tcpdump')
 cap.wait(timeout=15);children.remove(cap)
 wait(su,lambda s:s.get('phase')=='idle',a.warmup+65)
 warm=stats(su)
 assert warm['currentInFlight']==0 and warm['errors']==0,warm
 assert stats(tu)['errors']==0,stats(tu)
 (out/'warmup-target-stats.json').write_text(json.dumps(stats(tu),indent=2))
 (out/'warmup-source-stats.json').write_text(json.dumps(stats(su),indent=2))
 http(tu+'/bench/reset',{})
 mutex_before=mutex_snapshot() if a.mutex_stat else None
 phase('active',a.active)
 if a.collect!='none':
  collector=start(['python3',str(P/'tools/collect.py'),'--mode',a.collect,'--source',str(source.pid),'--target',str(target.pid),'--source-url',su,'--target-url',tu,'--out',str(out/'capture')],'collector')
  while collector.poll() is None:mem();time.sleep(.2)
  assert collector.returncode==0,collector.returncode
  children.remove(collector)
 wait(su,lambda s:s.get('phase')=='idle',a.active+65)
 if a.mutex_stat:
  (out/'mutex-window.json').write_text(json.dumps({'before':mutex_before,'after':mutex_snapshot()},indent=2))
 t=stats(tu);s=stats(su);(out/'target-stats.json').write_text(json.dumps(t,indent=2));(out/'source-stats.json').write_text(json.dumps(s,indent=2))
 r=json.loads((out/'results.json').read_text());c=verify(r,t);c['target_stats']=t;(out/'results.json').write_text(json.dumps(r,indent=2))
 print('RESULT',a.case,c['throughput_per_second'],'completed',c['completed'],'caller_us',c['submit_thread_cores']*1e6/c['throughput_per_second'],flush=True)
 for field,value in [('completed',c['completed']-1),('errors',1),('abandoned',1),('contaminated',True),('drain_bound_hit',True)]:
  bad=copy.deepcopy(r);bad['cells'][0][field]=value
  try:verify(bad,t)
  except AssertionError:pass
  else:raise RuntimeError('validator accepted '+field)
finally:
 for ch in children:
  if ch.poll() is None:os.killpg(ch.pid,signal.SIGTERM)
 for ch in children:
  try:ch.wait(timeout=30)
  except subprocess.TimeoutExpired:os.killpg(ch.pid,signal.SIGINT);ch.wait(timeout=30)
 for h in handles:h.close()
 (out/'runner-meta.json').write_text(json.dumps({'case':a.case,'rate':a.rate,'min_memavailable_kb':min(mems) if mems else None,'exit_codes':[ch.returncode for ch in children]},indent=2))
