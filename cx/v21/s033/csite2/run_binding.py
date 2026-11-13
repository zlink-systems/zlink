import os,sys,json,time,struct,pathlib as P,subprocess as S,urllib.request as U,signal,fcntl
j=P.Path('/root/jobs/fx/cx/v21/s033/csite2');base=P.Path('/root/jobs/fx/cx/v21/s033/crecv3/build')
def mem():
 m=int(next(x.split()[1] for x in P.Path('/proc/meminfo').read_text().splitlines() if x.startswith('MemAvailable:')));assert m>=12582912,m;return m
mem(); env=dict(os.environ,LD_PRELOAD=str(j/'libchand.so'),TMPDIR=str(j));children=[];logs=[]
def get(port,path='/bench/stats',data=None):
 req=U.Request(f'http://127.0.0.1:{port}'+path,data=None if data is None else json.dumps(data).encode(),headers={'Content-Type':'application/json'})
 with U.urlopen(req,timeout=5) as f:return json.load(f)
def wait(port,pred,seconds=60):
 end=time.monotonic()+seconds
 while time.monotonic()<end:
  mem()
  try:
   d=get(port)
   if pred(d):return d
  except (OSError,ValueError):pass
  for ch in children:
   if ch.poll() is not None:raise RuntimeError(f'endpoint exited {ch.returncode}')
  time.sleep(.1)
 raise RuntimeError(f'wait {port}')
def launch(name,cpus,cmd):
 f=open(j/(name+'.log'),'w');logs.append(f);ch=S.Popen(['taskset','-c',cpus]+list(map(str,cmd)),env=env,stdout=f,stderr=f);children.append(ch);return ch
names=['malloc','calloc','realloc','aligned','mutex_lock','cond_signal','rotation_start','rotation_end','completion_post','completion_run','cond_wait','role_mark','wait_return','trylock_ok','cond_broadcast','unused']
def snapshot(ch):
 raw=(j/f'count-{ch.pid}.bin').read_bytes();magic,n=struct.unpack_from('QQ',raw);assert magic==0x4348414e44;d={}
 for i in range(n):
  vals=struct.unpack_from('18Q',raw,4096+i*256);d[str(vals[0])]={'role':vals[1],**dict(zip(names,vals[2:]))}
 return {'threads':d,'sample_count':struct.unpack_from('Q',raw,16)[0]}
try:
 target=launch('target','8-15',[base/'bench_cpp_zlink_server','--endpoint','tcp://127.0.0.1:5294','--stats-port','5295']);wait(5295,lambda x:True)
 source=launch('source','0-7',[j/'bench_cpp_client','--implementation','zlink-cpp','--pattern','request-backpressure','--payload-size','1024','--trigger-port','5292','--stats-port','5293','--target-stats-port','5295','--endpoint','tcp://127.0.0.1:5294','--output-file',j/'binding-results.json','--warmup-seconds','30','--warmup-segments','10','--request-timeout-ms','30000','--drain-bound-ms','30000']);wait(5293,lambda x:x['ready'])
 common={'runId':'csite-binding-count','cellId':'zlink-cpp-request-backpressure-1024','pattern':'request-backpressure','payloadBytes':1024,'requestWindow':100,'sendConcurrency':8}
 get(5295,'/bench/reset',{});print('warmup',flush=True);get(5292,'/bench/start',{**common,'phase':'warmup','durationMs':30000});wait(5293,lambda x:x['phase']=='idle',70);time.sleep(.3)
 # Counts cover the complete phase and its drained completions, not sampled time.
 before={'source':snapshot(source),'target':snapshot(target)};sb=get(5293);tb=get(5295)
 print('active counts',flush=True);get(5292,'/bench/start',{**common,'phase':'active','durationMs':5000});sa=wait(5293,lambda x:x['phase']=='idle',45);time.sleep(.4);ta=get(5295)
 after={'source':snapshot(source),'target':snapshot(target)}
 (j/'counts-raw.json').write_text(json.dumps({'before':before,'after':after,'stats_before':{'source':sb,'target':tb},'stats_after':{'source':sa,'target':ta},'memavailable_kb':mem()},indent=2))
 print('done',sa,flush=True)
 for ch in children: (j/f'maps-{ch.pid}.txt').write_text(P.Path(f'/proc/{ch.pid}/maps').read_text())
finally:
 for ch in reversed(children):
  if ch.poll() is None:ch.send_signal(signal.SIGTERM)
 for ch in children:
  try:ch.wait(timeout=15)
  except S.TimeoutExpired:ch.send_signal(signal.SIGINT);ch.wait(timeout=15)
 for f in logs:f.close()
