import argparse,copy,hashlib,json,os,pathlib,signal,subprocess,sys,time,urllib.request
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/cstage')
p=argparse.ArgumentParser();p.add_argument('--variants',default='main,staged');p.add_argument('--runs',type=int,default=5);p.add_argument('--warmup',type=int,default=30);p.add_argument('--active',type=int,default=5);p.add_argument('--rate',type=int,default=0);p.add_argument('--tag',default='caller');p.add_argument('--cpu',action='store_true');a=p.parse_args()
children=[]
def stop(*args):
 for child in children:
  if child.poll() is None:child.send_signal(signal.SIGTERM)
 for child in children:
  try:child.wait(timeout=15)
  except subprocess.TimeoutExpired:
   child.send_signal(signal.SIGINT);child.wait(timeout=15)
 if args:raise KeyboardInterrupt()
signal.signal(signal.SIGTERM,stop);signal.signal(signal.SIGINT,stop)
def cpu_set(mask):
 result=set()
 for part in mask.split(','):
  ends=part.split('-');result.update(range(int(ends[0]),int(ends[-1])+1))
 return result
def http(port,path='/bench/stats',data=None):
 req=urllib.request.Request(f'http://127.0.0.1:{port}{path}',data=json.dumps(data).encode() if data is not None else None,headers={'Content-Type':'application/json'})
 with urllib.request.urlopen(req,timeout=5) as f:return json.load(f)
def wait_state(predicate,timeout):
 end=time.monotonic()+timeout;last=None
 while time.monotonic()<end:
  for child in children:assert child.poll() is None,('process exited',child.pid,child.returncode)
  try:
   last=predicate()
   if last:return last
  except (OSError,ValueError):pass
  time.sleep(.05)
 raise RuntimeError(('wait timeout',last))
def phase_done():
 s=http(5286)
 if s['phase']=='failed':raise RuntimeError(s)
 return s if s['phase']=='idle' else None
def snapshot(pid):
 d={'monotonic':time.monotonic(),'stats':http(5286),'pid':pid,'threads':{}}
 for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
  try:
   raw=(t/'stat').read_text();rest=raw[raw.rindex(')')+2:].split()
   mask=next(l.split(':',1)[1].strip() for l in (t/'status').read_text().splitlines() if l.startswith('Cpus_allowed_list:'))
   assert cpu_set(mask) and cpu_set(mask)<=set(range(8)),(t.name,mask)
   d['threads'][t.name]={'comm':(t/'comm').read_text().strip(),'affinity':mask,'user_ticks':int(rest[11]),'kernel_ticks':int(rest[12]),'runtime_ns':int((t/'schedstat').read_text().split()[0])}
  except FileNotFoundError:pass
 raw=pathlib.Path(f'/proc/{pid}/stat').read_text();rest=raw[raw.rindex(')')+2:].split();d['process']={'user_ticks':int(rest[11]),'kernel_ticks':int(rest[12])}
 d['maps']=pathlib.Path(f'/proc/{pid}/maps').read_text();d['end_monotonic']=time.monotonic();return d
def verify(result,target):
 c=result['cells'][0]
 assert c['submitted']==c['completed']==target['received'],(c['submitted'],c['completed'],target)
 for k in ['errors','abandoned','header_validation_failures']:assert c[k]==0,(k,c[k])
 assert not c.get('contaminated') and not c.get('drain_bound_hit')
 assert target['errors']==0 and target.get('rejected',0) in (None,0)
 assert c['throughput_per_second']>0
 return c

def run(variant,index):
 out=J/'runs'/a.tag/variant/f'run{index}';out.mkdir(parents=True,exist_ok=True)
 env=os.environ.copy();env.pop('ZLINK_CPP_BENCH_STAGED',None);env.pop('ZLINK_CPP_BENCH_REQUEST_RATE',None)
 if variant=='staged':env['ZLINK_CPP_BENCH_STAGED']='1'
 if a.rate:env['ZLINK_CPP_BENCH_REQUEST_RATE']=str(a.rate)
 package=J/f'package-{variant}'/'core';env['LD_LIBRARY_PATH']=str(package/'lib')+':'+env.get('LD_LIBRARY_PATH','')
 binary=J/f'bin-{variant}'
 target=[str(J/'bin-main/bench_cpp_zlink_server'),'--endpoint','tcp://127.0.0.1:5287','--command-endpoint','tcp://127.0.0.1:5288','--stats-port','5289']
 source=[str(binary/'bench_cpp_client'),'--implementation','zlink-cpp','--pattern','request-backpressure','--payload-size','1024','--trigger-port','5285','--stats-port','5286','--target-stats-port','5289','--endpoint','tcp://127.0.0.1:5287','--command-endpoint','tcp://127.0.0.1:5288','--output-file',str(out/'results.json'),'--warmup-seconds',str(a.warmup),'--warmup-segments','10','--request-timeout-ms','30000','--drain-bound-ms','30000']
 meta={'variant':variant,'run':index,'rate':a.rate,'active':a.active,'warmup':a.warmup,'source_command':['taskset','-c','0-7']+source,'target_command':['taskset','-c','8-15']+target,'target_core':str(J/'package-main/core/lib/libzlink.so'),'sha256':{str(path):hashlib.sha256(path.read_bytes()).hexdigest() for path in [binary/'bench_cpp_client',J/'bin-main/bench_cpp_zlink_server',package/'lib/libzlink.so',J/'package-main/core/lib/libzlink.so']}}
 handles=[]
 try:
  for name,cpus,command in [('target','8-15',target),('source','0-7',source)]:
   child_env=env.copy()
   if name=='target':
    child_env['LD_LIBRARY_PATH']=str(J/'package-main/core/lib')+':'+os.environ.get('LD_LIBRARY_PATH','')
    child_env.pop('ZLINK_CPP_BENCH_STAGED',None);child_env.pop('ZLINK_CPP_BENCH_REQUEST_RATE',None)
   h=(out/f'{name}.log').open('w');handles.append(h);child=subprocess.Popen(['taskset','-c',cpus]+command,env=child_env,stdout=h,stderr=h,start_new_session=True);children.append(child);meta[name+'_pid']=child.pid
   if name=='target':wait_state(lambda:http(5289),30)
   else:wait_state(lambda:http(5286).get('ready'),60)
  for role in ['source','target']:
   pid=meta[role+'_pid'];maps=pathlib.Path(f'/proc/{pid}/maps').read_text();loaded=sorted(set(l.split()[-1] for l in maps.splitlines() if '/libzlink.so' in l))
   expected=(package/'lib/libzlink.so') if role=='source' else J/'package-main/core/lib/libzlink.so'
   assert loaded and all(pathlib.Path(f).resolve()==expected.resolve() for f in loaded),(role,loaded,expected)
   meta[role+'_loaded_Core']=loaded
   affinity=next(l.split(':',1)[1].strip() for l in pathlib.Path(f'/proc/{pid}/status').read_text().splitlines() if l.startswith('Cpus_allowed_list:'))
   assert affinity==('0-7' if role=='source' else '8-15'),(role,affinity)
   meta[role+'_affinity']=affinity
   meta[role+'_thread_affinities']={}
   for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
    try:
     mask=next(l.split(':',1)[1].strip() for l in (t/'status').read_text().splitlines() if l.startswith('Cpus_allowed_list:'))
     assert cpu_set(mask) and cpu_set(mask)<=cpu_set(affinity),(role,t.name,mask,affinity)
     meta[role+'_thread_affinities'][t.name]=mask
    except FileNotFoundError:pass
  (out/'meta.json').write_text(json.dumps(meta,indent=2))
  trigger={'runId':f'cstage-{a.tag}-{variant}-{index}','cellId':'zlink-cpp-request-backpressure-1024','pattern':'request-backpressure','payloadBytes':1024,'requestWindow':100,'sendConcurrency':8}
  http(5285,'/bench/start',dict(trigger,phase='warmup',durationMs=a.warmup*1000));wait_state(phase_done,a.warmup+65)
  s=http(5286);assert s['currentInFlight']==0 and s['errors']==0,s
  thread_names={t.name:(t/'comm').read_text().strip() for t in pathlib.Path(f"/proc/{meta['source_pid']}/task").iterdir()}
  assert sum(n=='cstage-body' for n in thread_names.values())==(1 if variant=='staged' else 0),thread_names
  meta['source_threads_after_warmup']=thread_names
  (out/'meta.json').write_text(json.dumps(meta,indent=2))
  time.sleep(.3)
  http(5289,'/bench/reset',{})
  http(5285,'/bench/start',dict(trigger,phase='active',durationMs=a.active*1000))
  wait_state(lambda:http(5286)['phase']=='active',5)
  active_start=time.monotonic()
  if a.cpu:
   assert a.active>=60
   time.sleep(1);before=snapshot(meta['source_pid']);(out/'cpu-before.json').write_text(json.dumps(before,indent=2))
   while time.monotonic()-active_start<a.active-1:time.sleep(max(0,min(.25,a.active-1-(time.monotonic()-active_start))))
   after=snapshot(meta['source_pid']);(out/'cpu-after.json').write_text(json.dumps(after,indent=2));assert after['stats']['phase']=='active'
  wait_state(phase_done,a.active+65)
  last=None;stable=0
  for _ in range(100):
   t=http(5289)
   sig=(t['received'],t['errors'],t.get('rejected'))
   stable=stable+1 if last==sig else 0;last=sig
   if stable>=2:break
   time.sleep(.1)
  else:raise RuntimeError('target did not settle')
  (out/'target-stats.json').write_text(json.dumps(t,indent=2))
  result=json.loads((out/'results.json').read_text());c=verify(result,t)
  c['target_stats']={k:t[k] for k in ['received','errors','rejected'] if k in t};(out/'results.json').write_text(json.dumps(result,indent=2))
  print(json.dumps({'tag':a.tag,'variant':variant,'run':index,'throughput':c['throughput_per_second'],'caller_us':c['submit_thread_cores']*1e6/c['throughput_per_second'],'client_us':c['client_cores']*1e6/c['throughput_per_second'],'completed':c['completed']}),flush=True)
 finally:
  stop();children.clear()
  for h in handles:h.close()
  time.sleep(.3)

for index in range(1,a.runs+1):
 for variant in a.variants.split(','):run(variant,index)
# Check that validation rejects deliberately invalid count/error/contamination inputs.
test=json.loads(next((J/'runs'/a.tag).rglob('results.json')).read_text());target=copy.deepcopy(test['cells'][0]['target_stats']);verify(test,target)
for label,mutate in [('error',lambda c:c.update(errors=1)),('count',lambda c:c.update(submitted=c['submitted']+1)),('contamination',lambda c:c.update(contaminated=True)),('drain',lambda c:c.update(drain_bound_hit=True))]:
 bad=copy.deepcopy(test);mutate(bad['cells'][0])
 try:verify(bad,target)
 except AssertionError:print('negative_check_rejected='+label,flush=True)
 else:raise RuntimeError('validator accepted '+label)
