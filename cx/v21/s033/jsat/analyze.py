#!/usr/bin/env python3
import json,pathlib,sys
root=pathlib.Path(sys.argv[1])
result_files=list(root.glob('run1/*/results.json'))
if not result_files: result_files=list(root.glob('run*/**/results.json'))
result=json.load(open(result_files[0]))['cells'][0]
start,end=result['active_started_at_ns'],result['active_ended_at_ns']
samples=json.load(open(root/'samples.json'))
pre=min(samples,key=lambda x:abs(x['t']-start))
post=min(samples,key=lambda x:abs(x['t']-end))
wall=(end-start)/1e9
counts={'source':result['completed_at_close'],'target':result['server_received_at_close']}

def threads(sample,group):
 out={}
 for p in sample['procs']:
  if p['group']==group:
   for tid,v in p['threads'].items(): out[int(tid)]=v
 return out

def role(name,group,impl):
 n=name.lower()
 if 'gc ' in n or n.startswith('g1 '): return 'GC'
 if 'compilerthread' in n or 'compiler' in n and ('c1' in n or 'c2' in n): return 'JIT'
 if n.startswith('forkjoinpool.co'): return 'completion/pump'
 if n.startswith('forkjoinpool-1-'): return 'Framework dispatcher/executor'
 if 'zlinkbg/io' in n: return 'binding/Core I/O'
 if n.startswith('bench-phase-act'): return 'caller/submit'
 if n.startswith('bench-raw-reque'): return 'binding receive/request'
 if n.startswith('zlink-jvm-raw-m'): return 'Framework mesh / binding transport'
 if 'zlink-' in n and ('timer' in n or 'timeout' in n): return 'timer'
 if 'virtualthread' in n or 'http-dispatcher' in n: return 'Framework dispatcher/handler carrier'
 return 'JVM/other'
rows=[]
for group in ('source','target'):
 a,b=threads(pre,group),threads(post,group)
 for tid in sorted(set(a)|set(b)):
  av=a.get(tid); bv=b.get(tid)
  if av is None: av=(bv[0],0,0,0,0)
  if bv is None: bv=(av[0],0,0,0,0)
  name=bv[0]
  runtime=max(0,bv[1]-av[1]); wait=max(0,bv[2]-av[2]); vol=max(0,bv[3]-av[3]); invol=max(0,bv[4]-av[4])
  req=counts[group]
  rows.append({'process':group,'tid':tid,'thread':name,'role':role(name,group,result['implementation']),
    'cores':runtime/(end-start),'us_per_req':runtime/req/1000,
    'vol_per_req':vol/req,'invol_per_req':invol/req,'runqueue_us_per_req':wait/req/1000,
    'runqueue_ms':wait/1e6,'runtime_ms':runtime/1e6})
rows.sort(key=lambda x:(x['process'], -x['cores'],x['thread']))
json.dump({'implementation':result['implementation'],'rate':result['throughput_per_second'],'completed':counts['source'],'target_received':counts['target'],'window_seconds':wall,'start_delta_ms':(pre['t']-start)/1e6,'end_delta_ms':(post['t']-end)/1e6,'rows':rows},open(root/'thread-metrics.json','w'),indent=2)
print(root, 'rate',result['throughput_per_second'],'n',counts,'snapshot deltas ms',(pre['t']-start)/1e6,(post['t']-end)/1e6,'rows',len(rows))
for r in rows[:20]: print(r)
