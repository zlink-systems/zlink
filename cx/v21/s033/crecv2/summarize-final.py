from pathlib import Path
import json,re,statistics
j=Path(__file__).parent
m=json.loads((j/'measurements.json').read_text());s=m['summary'];median=statistics.median
cohorts=[('cladder rung 9','ladder-rung9'),('cladder unlimited Framework','ladder-rung9-unbounded'),('csubmit request, binding','csubmit-request-backpressure-zlink-cpp'),('csubmit request, Framework','csubmit-request-backpressure-zlink-framework-cpp'),('csubmit send, binding','csubmit-send-saturation-zlink-cpp'),('csubmit send, Framework','csubmit-send-saturation-zlink-framework-cpp')]
lines=['## Final before/after measurements','', f'Median of three runs; throughput in messages/s, CPU in µs/completed message. csubmit uses condition B and paired binding medians. Ladder denominators are its unchanged before references (rung 0: {s["ladder-rung9-before"]["binding_denominator"]:,.2f}; unlimited binding: {s["ladder-rung9-unbounded-before"]["binding_denominator"]:,.2f} messages/s), so ladder F/B is not a newly paired binding measurement. Full per-run values, ranges, and actual thread IDs are in `measurements.json` and `threads.csv`.','', '| Cell | Before msg/s | After msg/s | Change | Before F/B | After F/B | Caller CPU before → after | Source CPU before → after | Target CPU before → after |', '|---|---:|---:|---:|---:|---:|---:|---:|---:|']
for name,key in cohorts:
 b,a=s[key+'-before'],s[key+'-after'];assert b['runs']==a['runs']==3
 def ratio(x):return f"{x['F/B']:.3f}" if 'F/B' in x else '—'
 def cpu(role):return f"{b['endpoints'][role]['cpu_us']:.3f} → {a['endpoints'][role]['cpu_us']:.3f}"
 def caller(x):return x['endpoints']['source']['ranked_cpu_us'].get('caller #1',0)
 lines.append(f"| {name} | {b['throughput']:,.1f} | {a['throughput']:,.1f} | {(a['throughput']/b['throughput']-1)*100:+.1f}% | {ratio(b)} | {ratio(a)} | {caller(b):.3f} → {caller(a):.3f} | {cpu('source')} | {cpu('target')} |")
lines+=['','Raw paths (each directory contains `results.json`, source/target logs, and `capture/` snapshots):','', '| Cell | Before | After |','|---|---|---|']
for name,key in cohorts:
 b=m['raw'][key+'-before'];a=m['raw'][key+'-after'];lines.append(f"| {name} | "+'<br>'.join(f"`{r['path']}`" for r in b)+' | '+'<br>'.join(f"`{r['path']}`" for r in a)+' |')
lines+=['','### Per-thread CPU','', 'Names below are the recorded thread role or Linux comm plus descending CPU rank within that role/comm in each run, then the three-run median. They are not stable thread IDs across processes. Every thread with at least 0.5 µs/message in either cohort is listed; all smaller threads and each actual TID remain in `threads.csv` and `measurements.json`. Unnamed `bench_cpp_*` workers are intentionally not assigned an inferred execution role.','', '| Cell | Endpoint | Recorded thread/rank | Before µs/msg | After µs/msg |','|---|---|---|---:|---:|']
for name,key in cohorts:
 b,a=s[key+'-before'],s[key+'-after']
 for role in ['source','target']:
  bv,av=b['endpoints'][role]['ranked_cpu_us'],a['endpoints'][role]['ranked_cpu_us']
  names=sorted(set(bv)|set(av),key=lambda n:max(bv.get(n,0),av.get(n,0)),reverse=True)
  for n in names:
   if max(bv.get(n,0),av.get(n,0))>=.5:lines.append(f"| {name} | {role} | `{n}` | {bv.get(n,0):.3f} | {av.get(n,0):.3f} |")
(j/'measurement-tables.md').write_text('\n'.join(lines)+'\n')
# The allocation census uses the same interposer and eight-second counter window.
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
before=census(j.parent/'csubmit/census/branch/run1/zlink-framework-cpp-request-backpressure-1024')
after=census(j/'census/after/run1/zlink-framework-cpp-request-backpressure-1024')
(j/'allocation-census.json').write_text(json.dumps({'before':before,'after':after},indent=2))
print(json.dumps({'throughput':{k:v['throughput'] for k,v in s.items()},'caller_heap_calls':{k:v['caller_heap_calls_per_request'] for k,v in [('before',before),('after',after)]}},indent=2))
