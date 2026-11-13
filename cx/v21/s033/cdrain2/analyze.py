import pathlib,json,re,statistics,math
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/cdrain2')
records=[]
def celldir(run,pattern,variant,impl):return job/'measure'/str(run)/pattern/variant/'run1'/f'{impl}-{pattern if pattern!="pm" else "send-saturation"}-1024'
def profile(directory):
 samples=json.loads((directory/'capture/samples.json').read_text());first,last=samples[0],samples[-1]
 n=last['stats']['target']['received']-first['stats']['target']['received'];assert n>0
 elapsed=last['time']-first['time'];result={}
 for role in ('source','target'):
  previous=dict(first['processes'][role]['threads']);totals={}
  for sample in samples[1:]:
   for tid,now in sample['processes'][role]['threads'].items():
    before=previous.get(tid,{k:0 for k in ('runtime_ns','voluntary','involuntary')})
    total=totals.setdefault(tid,{'runtime_ns':0,'voluntary':0,'involuntary':0})
    for key in total:total[key]+=max(0,now[key]-before[key])
    previous[tid]=now
  for total in totals.values():
   total['us_per_message']=total['runtime_ns']/1000/n
   total['cores']=total['runtime_ns']/1e9/elapsed
  result[role]={'threads':totals,'switches_per_message':sum(v['voluntary']+v['involuntary'] for v in totals.values())/n}
 roles={}
 for role,tid in re.findall(r'ROLE (\S+) tid=(\d+)',(directory/'target.log').read_text()):roles.setdefault(role,set()).add(tid)
 result['role_threads']={k:sorted(v) for k,v in roles.items()};result['messages']=n;result['elapsed']=elapsed
 return result
for pattern in ('request-backpressure','send-saturation','pm'):
 for run in (1,2,3):
  for variant in ('main','branch'):
   row={'pattern':pattern,'run':run,'variant':variant}
   for impl,key in [('zlink-framework-cpp','framework'),('zlink-cpp','binding')]:
    if pattern=='pm' and key=='binding':continue
    directory=celldir(run,pattern,variant,impl)
    c=json.loads((directory/'results.json').read_text())['cells'][0]
    assert not c.get('contaminated',False) and not c.get('drain_bound_hit',False) and c['errors']==0 and c['abandoned']==0,(directory,c)
    row[key]={'throughput':c['throughput_per_second'],'cell':c,'directory':str(directory),'profile':profile(directory)}
   if pattern=='pm':
    probes=json.loads((celldir(run,pattern,variant,'zlink-framework-cpp')/'capture/pm-probes.json').read_text());assert len(probes)>=150
    lat=sorted(p['spotLatencyUs'] for p in probes)
    row['spot']={'count':len(lat),'p50_us':lat[math.ceil(.5*len(lat))-1],'p99_us':lat[math.ceil(.99*len(lat))-1],'maximum_hot_handlers':max(p['maximumHotRunning'] for p in probes),'median_hot_handlers':statistics.median(p['hotRunning'] for p in probes)}
   records.append(row)
(job/'summary.json').write_text(json.dumps(records,indent=2))
lines=['## Measurements','', 'Release / `-O3 -DNDEBUG`, identical Core/binding packages and dependency versions, test creation probes disabled and install enabled for both builds. Condition B pins source 0–7 and target 8–15. Each cell uses 30 s warm-up, followed by 5 s active time (PM: 30 s). Three runs per cell and variant. An exclusive measure.lock excludes builds/tests. TIDs are marked once on first handler/receive entry; no per-record logging is used. CPU and switches come from `/proc/<pid>/task/*/{schedstat,status}`, sampled every ~100 ms during a 3 s window (PM: 25 s). CPU µs/msg and switches/msg use target messages received in that same window as denominator. A retiring thread may have at most one sampling interval of unobserved final counters. Unique TIDs across a window can exceed the concurrent worker bound because idle workers retire and are replaced.','', '| Cell | Run | Revision | Binding msg/s | Framework msg/s | Framework / binding |','|---|---:|---|---:|---:|---:|']
for r in records:
 if r['pattern']=='pm':continue
 b,f=r['binding']['throughput'],r['framework']['throughput']
 lines.append(f"| {r['pattern']} 1K | {r['run']} | {r['variant']} | {b:,.1f} | {f:,.1f} | {f/b:.4f} |")
lines+=['','| Cell | Revision | Binding 3-run median | Framework 3-run median | Ratio of medians |','|---|---|---:|---:|---:|']
for pattern in ('request-backpressure','send-saturation'):
 for variant in ('main','branch'):
  rs=[r for r in records if r['pattern']==pattern and r['variant']==variant]
  b=statistics.median(r['binding']['throughput'] for r in rs);f=statistics.median(r['framework']['throughput'] for r in rs)
  lines.append(f'| {pattern} 1K | {variant} | {b:,.1f} | {f:,.1f} | {f/b:.4f} |')
lines+=['', 'Target per-thread CPU (each entry is `TID: µs/msg`). Receive-role and worker TIDs are identified by the first-entry role markers. Context switches include voluntary and involuntary switches for all threads in the corresponding process.','', '| Cell | Run | Revision | Target application workers | Target receive role | Target switches/msg | Source switches/msg |','|---|---:|---|---|---|---:|---:|']
for r in records:
 p=r['framework']['profile'];threads=p['target']['threads']
 def fmt(role):return '; '.join(f"{tid}: {threads[tid]['us_per_message']:.3f}" for tid in p['role_threads'].get(role,[]) if tid in threads and threads[tid]['runtime_ns']>0) or 'none observed'
 lines.append(f"| {r['pattern']} | {r['run']} | {r['variant']} | {fmt('app-worker')} | {fmt('receive')} | {p['target']['switches_per_message']:.4f} | {p['source']['switches_per_message']:.4f} |")
lines+=['', 'Binding comparison: the receive role executes its handler inline; there is no separate application-worker stage.', '', '| Cell | Run | Revision | Binding receive + handler TID: µs/msg | Target switches/msg | Source switches/msg |', '|---|---:|---|---|---:|---:|']
for r in records:
 if r['pattern']=='pm':continue
 p=r['binding']['profile'];threads=p['target']['threads']
 receive='; '.join(f"{tid}: {threads[tid]['us_per_message']:.3f}" for tid in p['role_threads'].get('receive',[]) if tid in threads and threads[tid]['runtime_ns']>0) or 'none observed'
 lines.append(f"| {r['pattern']} | {r['run']} | {r['variant']} | {receive} | {p['target']['switches_per_message']:.4f} | {p['source']['switches_per_message']:.4f} |")
lines+=['','PM cell: one hot Node owner runs 100 µs synchronous CPU handlers; a public HTTP trigger issues an independent typed request to a gated Instance Spot on an independent 100 ms cadence while the main collector samples /proc. The reported latency measures only the public `request_to_spot(...).instance_spot(...).async<...>()` interval inside the trigger, excluding HTTP transport time. Cold activation completes before hot traffic starts. The same target hosts the hot Node and Spot. The HTTP-triggered public Spot call uses same-host local enqueue through the ordinary application mailbox/executor, gate and permit paths (public_host_runtime.cpp:9615,9345,8992; mesh_node_host_service.cpp:3331). Its latency includes local admission, scheduling, execution and completion; it excludes HTTP and remote network transport. The hot traffic source is pinned to 0–7 and the target including the Spot to 8–15. In-memory Stores are setup dependencies.','', '| Run | Revision | Hot msg/s | Spot samples | Spot p50 µs | Spot p99 µs | Peak concurrent hot handlers | Median hot handlers at probe completion |','|---:|---|---:|---:|---:|---:|---:|---:|']
for r in records:
 if r['pattern']!='pm':continue
 s=r['spot'];lines.append(f"| {r['run']} | {r['variant']} | {r['framework']['throughput']:,.1f} | {s['count']} | {s['p50_us']:.3f} | {s['p99_us']:.3f} | {s['maximum_hot_handlers']} | {s['median_hot_handlers']} |")
lines+=['','| Revision | Median run p50 µs | Median run p99 µs |','|---|---:|---:|']
for variant in ('main','branch'):
 rs=[r['spot'] for r in records if r['pattern']=='pm' and r['variant']==variant]
 lines.append(f"| {variant} | {statistics.median(r['p50_us'] for r in rs):.3f} | {statistics.median(r['p99_us'] for r in rs):.3f} |")
lines+=['',f'Raw cells: `{job}/measure/`; per-thread counters and summaries: `{job}/summary.json`. Reproduction scripts: `configure.sh`, `configure-main.sh`, `measure.sh`, `run-B.sh`, `runner-common.sh`, `collect.py`, and `bench/` in the same job directory.','']
p=job/'report.md';s=p.read_text();start=s.index('## Measurements');end=s.index('## Cross-language comparison',start);s=s[:start]+'\n'.join(lines)+'\n'+s[end:];p.write_text(s)
print('\n'.join(lines[:25]))
