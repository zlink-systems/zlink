import json,pathlib,statistics,collections
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/wire');rows=[]
for result in sorted((job/'measure').glob('*/*/*/results.json')):
 cell=json.loads(result.read_text())['cells'][0]
 assert not cell.get('contaminated') and cell['errors']==0 and cell['abandoned']==0 and cell['header_validation_failures']==0 and not cell['drain_bound_hit'],result
 cap=json.loads((result.parent/'capture/capture.json').read_text());b=cap['before'];a=cap['after'];n=a['source_stats']['completed']-b['source_stats']['completed'];assert n>0
 tid=cap['submitter_tid'];rates={}
 for role in ['source','target']:
  alloc=cpu=0
  for t,after in a[role].items():
   before=b[role].get(t,{})
   alloc+=sum(after.get(k,0)-before.get(k,0) for k in ['malloc','calloc','realloc'])
   if after.get('live') and before.get('live'):cpu+=after['cpu_ns']-before['cpu_ns']
  rates[role+'_heap']=alloc/n;rates[role+'_cpu_us']=cpu/1000/n
 submit_heap=sum(a['source'][tid].get(k,0)-b['source'][tid].get(k,0) for k in ['malloc','calloc','realloc'])/n
 stamp=result.parents[2].name;round_,label,pattern=stamp.split('-',2)
 rows.append({'round':int(round_),'build':label,'pattern':pattern,'throughput':cell['throughput_per_second'],'submit_cpu_us':cell['submit_thread_cores']*1e6/cell['throughput_per_second'],'submit_capture_cpu_us':(a['source'][tid]['cpu_ns']-b['source'][tid]['cpu_ns'])/1000/n,'submit_heap':submit_heap,'submitter_tid':tid,'capture_messages':n,'minimum_capture_mem_kb':min(a['memavailable_kb'],b['memavailable_kb']),**rates})
assert len(rows)==16,len(rows)
medians=[]
for pattern in ['request-backpressure','send-saturation']:
 for label in ['W','main']:
  subset=[r for r in rows if r['pattern']==pattern and r['build']==label];assert len(subset)==4
  medians.append({'pattern':pattern,'build':label,**{key:statistics.median(r[key] for r in subset) for key in ['throughput','submit_cpu_us','submit_heap','source_heap','target_heap','source_cpu_us','target_cpu_us','submit_capture_cpu_us']}})
(job/'measure-summary.json').write_text(json.dumps({'rounds':rows,'medians':medians},indent=2))
lines=['| Pattern | Round | Build | msg/s | Submit CPU µs/msg | Submit heap calls/msg | Source heap/msg | Target heap/msg |','|---|---:|---|---:|---:|---:|---:|---:|']
for r in sorted(rows,key=lambda r:(r['pattern'],r['round'],r['build']=='main')):
 lines.append(f"| {r['pattern']} | {r['round']} | {r['build']} | {r['throughput']:,.1f} | {r['submit_cpu_us']:.3f} | {r['submit_heap']:.3f} | {r['source_heap']:.3f} | {r['target_heap']:.3f} |")
lines+=['','| Pattern | Median build | msg/s | Submit CPU µs/msg | Submit heap calls/msg | Source heap/msg | Target heap/msg |','|---|---|---:|---:|---:|---:|---:|']
for r in medians:lines.append(f"| {r['pattern']} | {r['build']} | {r['throughput']:,.1f} | {r['submit_cpu_us']:.3f} | {r['submit_heap']:.3f} | {r['source_heap']:.3f} | {r['target_heap']:.3f} |")
(job/'measure-table.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines[-7:]))
