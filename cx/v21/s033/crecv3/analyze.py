from pathlib import Path
from collections import defaultdict
import csv,json,re,statistics
J=Path(__file__).parent; P=J.parent; median=statistics.median

def cell_data(p):
 c=json.loads((p/'results.json').read_text())['cells'][0]
 assert c['errors']==c['abandoned']==c['header_validation_failures']==0,p
 assert not c.get('contaminated') and not c.get('drain_bound_hit'),p
 assert c['completed']==c['target_stats']['received'],p
 assert c['target_stats']['errors']==c['target_stats']['rejected']==0,p
 samples=json.loads((p/'capture/samples.json').read_text()); b,a=samples[0],samples[-1]
 clock='monotonic' if 'monotonic' in b else 'time'
 n=(a['stats']['target']['received']-b['stats']['target']['received']) if c['pattern']=='send-saturation' else (a['stats']['source']['completed']-b['stats']['source']['completed'])
 assert n>0,p
 row={'path':str(p),'throughput':c['throughput_per_second'],'messages':n,'seconds':a[clock]-b[clock],'endpoints':{}}
 for role in ['source','target']:
  marked=dict((tid,label) for label,tid in re.findall(r'ROLE ([\w-]+) tid=(\d+)',(p/f'{role}.log').read_text()))
  previous=b['processes'][role]['threads'].copy(); threads={}
  for snap in samples[1:]:
   for tid,now in snap['processes'][role]['threads'].items():
    old=previous.get(tid,{'runtime_ns':0}); t=threads.setdefault(tid,{'comm':now['comm'],'cpu_ns':0})
    t['cpu_ns']+=max(0,now['runtime_ns']-old['runtime_ns']);previous[tid]=now
  if role=='source' and 'source-submitter' not in marked.values() and not any(t['comm']=='ladder-caller' for t in threads.values()):
   candidates=[(tid,t) for tid,t in threads.items() if not re.search('ZLINKbg/IO/|ladder-poll|zlink-io',t['comm']) and tid!=str(a['processes'][role]['pid'])]
   if candidates:marked.setdefault(max(candidates,key=lambda x:x[1]['cpu_ns'])[0],'caller')
  groups=defaultdict(list)
  for tid,t in threads.items():
   t['cpu_us']=t['cpu_ns']/1000/n;t['cores']=t['cpu_ns']/1e9/row['seconds'];t['role']=marked.get(tid,t['comm'])
   if t['role']=='source-submitter' or t['comm']=='ladder-caller':t['role']='caller'
   groups[t['role']].append((tid,t))
  by_rank={}
  for name,values in groups.items():
   for rank,(tid,t) in enumerate(sorted(values,key=lambda v:v[1]['cpu_ns'],reverse=True),1):
    t['rank']=rank;by_rank[f'{name} #{rank}']=t['cpu_us']
  row['endpoints'][role]={'cpu_us':sum(t['cpu_us'] for t in threads.values()),'threads':threads,'ranked_cpu_us':by_rank}
 return row

groups={}
for pattern in ['request-backpressure','send-saturation']:
 for impl in ['zlink-cpp','zlink-framework-cpp']:
  for version,root in [('before',P/'crecv2/measure'),('after',J/'measure')]:
   paths=[root/f'run{i}/{impl}-{pattern}-1024' for i in range(1,4)]
   assert all((p/'results.json').exists() for p in paths),paths
   groups[f'csubmit-{pattern}-{impl}-{version}']=[cell_data(p) for p in paths]
summary={}
for label,rows in groups.items():
 row={'throughput':median(r['throughput'] for r in rows),'range':[min(r['throughput'] for r in rows),max(r['throughput'] for r in rows)],'runs':len(rows),'endpoints':{}}
 for role in ['source','target']:
  names=set().union(*(r['endpoints'][role]['ranked_cpu_us'] for r in rows))
  row['endpoints'][role]={'cpu_us':median(r['endpoints'][role]['cpu_us'] for r in rows),'ranked_cpu_us':{name:median(r['endpoints'][role]['ranked_cpu_us'].get(name,0) for r in rows) for name in sorted(names)}}
 summary[label]=row
for pattern in ['request-backpressure','send-saturation']:
 for version in ['before','after']:
  f=summary.get(f'csubmit-{pattern}-zlink-framework-cpp-{version}');b=summary.get(f'csubmit-{pattern}-zlink-cpp-{version}')
  if f and b:f['F/B']=f['throughput']/b['throughput']
(J/'measurements.json').write_text(json.dumps({'summary':summary,'raw':groups},indent=2))
with (J/'threads.csv').open('w') as f:
 w=csv.writer(f);w.writerow(['cohort','run','endpoint','tid','comm','role','rank','cpu_us_per_message','cores','raw_path'])
 for label,rows in groups.items():
  for run,row in enumerate(rows,1):
   for role,endpoint in row['endpoints'].items():
    for tid,t in endpoint['threads'].items():w.writerow([label,run,role,tid,t['comm'],t['role'],t['rank'],t['cpu_us'],t['cores'],row['path']])
print(json.dumps(summary,indent=2))

def census(d):
 b=json.loads((d/'capture/before.json').read_text());a=json.loads((d/'capture/after.json').read_text());n=a['stats']['source']['completed']-b['stats']['source']['completed'];assert n>0
 result={'raw':str(d),'completed':n,'seconds':a['monotonic']-b['monotonic'],'endpoints':{}}
 for role in ['source','target']:
  old={str(x['tid']):x for x in b['processes'][role]['alloc']['slots'] if x['tid']};new={str(x['tid']):x for x in a['processes'][role]['alloc']['slots'] if x['tid']}
  calls={tid:sum(x[k]-old.get(tid,{}).get(k,0) for k in ['malloc','calloc','realloc']) for tid,x in new.items()}
  result['endpoints'][role]={'heap_calls_per_request':sum(calls.values())/n,'per_thread':{t:v/n for t,v in calls.items()}}
  if role=='source':
   marked=set(re.findall(r'ROLE source-submitter tid=(\d+)',(d/'source.log').read_text()));active=[t for t in marked if t in a['processes'][role]['threads'] and t in b['processes'][role]['threads'] and a['processes'][role]['threads'][t]['runtime_ns']-b['processes'][role]['threads'][t]['runtime_ns']>1000000];assert len(active)==1,active;result['caller_heap_calls_per_request']=calls[active[0]]/n
 return result
before=census(P/'crecv2/census/after/run1/zlink-framework-cpp-request-backpressure-1024')
after=census(J/'census/after/run1/zlink-framework-cpp-request-backpressure-1024')
(J/'allocation-census.json').write_text(json.dumps({'before':before,'after':after},indent=2))
lines=['| Pattern / implementation | Before msg/s | After msg/s | Change | F/B before → after | Caller µs/msg before → after | Source µs/msg before → after | Target µs/msg before → after |', '|---|---:|---:|---:|---:|---:|---:|---:|']
for pattern in ['request-backpressure','send-saturation']:
 for impl in ['zlink-cpp','zlink-framework-cpp']:
  b,a=(summary[f'csubmit-{pattern}-{impl}-{version}'] for version in ['before','after'])
  ratio=f"{b['F/B']:.3f} → {a['F/B']:.3f}" if 'F/B' in b else '—'
  caller=lambda x:x['endpoints']['source']['ranked_cpu_us'].get('caller #1',0)
  cpu=lambda role:f"{b['endpoints'][role]['cpu_us']:.3f} → {a['endpoints'][role]['cpu_us']:.3f}"
  lines.append(f"| {pattern} / {impl} | {b['throughput']:,.1f} | {a['throughput']:,.1f} | {(a['throughput']/b['throughput']-1)*100:+.1f}% | {ratio} | {caller(b):.3f} → {caller(a):.3f} | {cpu('source')} | {cpu('target')} |")
lines+=['', '| Allocation scope | Before | After |', '|---|---:|---:|',f"| Source caller heap calls/request | {before['caller_heap_calls_per_request']:.6f} | {after['caller_heap_calls_per_request']:.6f} |"]
for role in ['source','target']:
 lines.append(f"| Entire {role} heap calls/request | {before['endpoints'][role]['heap_calls_per_request']:.6f} | {after['endpoints'][role]['heap_calls_per_request']:.6f} |")
(J/'measurement-tables.md').write_text('\n'.join(lines)+'\n')
