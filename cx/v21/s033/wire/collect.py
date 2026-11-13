import json,os,pathlib,struct,sys,time,urllib.request
source,target,outdir=map(str,sys.argv[1:]); out=pathlib.Path(outdir);out.mkdir(parents=True,exist_ok=True)
def stats(port):
 with urllib.request.urlopen(f'http://127.0.0.1:{port}/bench/stats',timeout=2) as r:return json.load(r)
def snap(pid):
 threads={}
 for t in pathlib.Path(f'/proc/{pid}/task').iterdir():
  try:threads[t.name]={'cpu_ns':int((t/'schedstat').read_text().split()[0]),'comm':(t/'comm').read_text().strip(),'live':True}
  except FileNotFoundError:pass
 data=(pathlib.Path(os.environ['PROF_ALLOC_DIR'])/f'alloc-{pid}.bin').read_bytes();magic,version,n=struct.unpack_from('QQQ',data)
 assert magic==0x435050414c4c4f43 and version==1 and n<=4096
 for i in range(n):
  tid,m,c,r,f=struct.unpack_from('QQQQQ',data,4096+i*64)
  threads.setdefault(str(tid),{'cpu_ns':0,'comm':'<exited>','live':False}).update(malloc=m,calloc=c,realloc=r,free=f)
 return threads
def snapshot():
 mem=int(next(l.split()[1] for l in pathlib.Path('/proc/meminfo').read_text().splitlines() if l.startswith('MemAvailable:')));assert mem>=12582912,mem
 return {'time':time.monotonic(),'memavailable_kb':mem,'source_stats':stats(5293),'target_stats':stats(5295),'source':snap(source),'target':snap(target)}
# Inner active window excludes phase transitions and final drain.
time.sleep(.3);before=snapshot();assert before['source_stats']['phase']=='active',before['source_stats']
time.sleep(3);after=snapshot();assert after['source_stats']['phase']=='active',after['source_stats']
preactive=set((out.parent/'source-preactive-tids.txt').read_text().split())
new_threads={tid for tid,t in before['source'].items() if t['live']}-preactive
def heap_delta(tid):
 return sum(after['source'].get(tid,{}).get(k,0)-before['source'].get(tid,{}).get(k,0) for k in ['malloc','calloc','realloc'])
candidates=sorted(new_threads,key=heap_delta,reverse=True)
assert candidates and heap_delta(candidates[0])>0,('active worker not observed',new_threads)
assert len(candidates)==1 or heap_delta(candidates[0])>10*heap_delta(candidates[1]),('ambiguous active worker',[(t,heap_delta(t)) for t in candidates])
submitter_tid=candidates[0]
candidate_counts={t:{'heap_delta':heap_delta(t),'cpu_ns_delta':after['source'].get(t,{}).get('cpu_ns',0)-before['source'].get(t,{}).get('cpu_ns',0)} for t in candidates}
(out/'capture.json').write_text(json.dumps({'before':before,'after':after,'submitter_tid':submitter_tid,'candidate_counts':candidate_counts},indent=2))
