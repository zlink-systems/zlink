from pathlib import Path
from collections import defaultdict,Counter
import json,sys
import perf_support as m
J=Path(__file__).parent
p=J/sys.argv[1]/'run1/zlink-framework-cpp-request-backpressure-1024'
a=json.loads((p/'capture/after.json').read_text());b=json.loads((p/'capture/before.json').read_text());n=a['stats']['source']['completed']-b['stats']['source']['completed']
samples=m.parse_perf(p/'capture/source.perf.data',p/'capture/source.stacks.json')
groups=defaultdict(list)
for s in samples:groups[s['tid']].append(s)
rows={}
for tid,t in a['processes']['source']['threads'].items():
 old=b['processes']['source']['threads'].get(tid,{'runtime_ns':0});cpu=(t['runtime_ns']-old['runtime_ns'])/1000/n
 g=groups[tid];w=sum(s['period'] for s in g);inclusive=Counter();leaf=Counter()
 for s in g:
  if s['frames']:leaf[s['frames'][0][0]]+=s['period']
  for f in set(f[0] for f in s['frames']):inclusive[f]+=s['period']
 rows[tid]={'comm':t['comm'],'cpu_us':cpu,'samples':len(g),'inclusive':{k:v*cpu/w for k,v in inclusive.items()} if w else {},'self':{k:v*cpu/w for k,v in leaf.items()} if w else {}}
(J/(sys.argv[1]+'.json')).write_text(json.dumps(rows,indent=2))
for tid,t in sorted(rows.items(),key=lambda p:-p[1]['cpu_us'])[:12]:
 print(tid,t['comm'],round(t['cpu_us'],3),'samples',t['samples'])
 if any('raw_route_port_t::poll' in s for s in t['inclusive']):
  for k,v in sorted(t['inclusive'].items(),key=lambda p:-p[1]):
   if any(x in k for x in ['raw_route_port_t::poll','post_completions','dispatch_completion(','scheduler::post_immediate_completion','operation_registry_state_t::complete','pthread_cond_signal']):print('  inclusive',round(v,3),k[:200])
  for k,v in sorted(t['self'].items(),key=lambda p:-p[1])[:12]:print('  self',round(v,3),k[:160])
