import json,pathlib,statistics,os
j=pathlib.Path('/root/jobs/fx/cx/v21/s033/cstage');out={'caller':{},'cpu':{}}
fingerprints={}
for variant in ['main','staged']:
 for tag,count,active in [('caller',5,5),('cpu',1,60)]:
  metas=[json.loads(p.read_text()) for p in sorted((j/'runs'/tag/variant).glob('run*/meta.json'))]
  assert len(metas)==count,(variant,tag,len(metas))
  for m in metas:
   assert m['warmup']==30 and m['active']==active,m
   assert m['source_affinity']=='0-7' and m['target_affinity']=='8-15'
   assert ('cstage-body' in m['source_threads_after_warmup'].values())==(variant=='staged')
   if tag=='caller':assert m['rate']==0
   for path,digest in m['sha256'].items():
    if path in fingerprints:assert fingerprints[path]==digest,(path,'artifact changed')
    fingerprints[path]=digest
out['artifact_sha256']=fingerprints
for variant in ['main','staged']:
 rows=[]
 for p in sorted((j/'runs/caller'/variant).glob('run*/results.json')):
  c=json.loads(p.read_text())['cells'][0];rate=c['throughput_per_second']
  assert c['errors']==c['abandoned']==c['header_validation_failures']==0 and not c['contaminated'] and not c['drain_bound_hit']
  assert c['completed']==c['submitted']==c['target_stats']['received']
  rows.append({'run':p.parent.name,'caller_us':c['submit_thread_cores']*1e6/rate,'throughput':rate,'completed':c['completed'],'source_cpu_us_at_own_rate':c['client_cores']*1e6/rate})
 if rows:
  out['caller'][variant]={'runs':rows,'caller_median':statistics.median(r['caller_us'] for r in rows),'caller_range':[min(r['caller_us'] for r in rows),max(r['caller_us'] for r in rows)],'throughput_median':statistics.median(r['throughput'] for r in rows),'throughput_range':[min(r['throughput'] for r in rows),max(r['throughput'] for r in rows)]}
 for p in sorted((j/'runs/cpu'/variant).glob('run*/cpu-after.json')):
  after=json.loads(p.read_text());before=json.loads((p.parent/'cpu-before.json').read_text());hz=os.sysconf('SC_CLK_TCK');n=after['stats']['completed']-before['stats']['completed'];seconds=after['monotonic']-before['monotonic'];threads=[];groups={g:0.0 for g in ['caller','worker','Core_IO','other']}
  candidates=[tid for tid,t in after['threads'].items() if t['comm']=='cstage-caller']
  assert candidates
  caller_tid=max(candidates,key=lambda tid:sum(after['threads'][tid][k]-before['threads'].get(tid,{}).get(k,0) for k in ['user_ticks','kernel_ticks']))
  assert caller_tid!=str(after['pid'])
  for tid,t in after['threads'].items():
   b=before['threads'].get(tid,{'user_ticks':0,'kernel_ticks':0,'runtime_ns':0});user=(t['user_ticks']-b['user_ticks'])/hz*1e6/n;kernel=(t['kernel_ticks']-b['kernel_ticks'])/hz*1e6/n
   name=t['comm'];group='caller' if tid==caller_tid else 'worker' if name=='cstage-body' else 'Core_IO' if name.startswith('ZLINKbg/IO') else 'other';groups[group]+=user+kernel
   threads.append({'tid':tid,'name':name,'group':group,'user_us':user,'kernel_us':kernel,'total_us':user+kernel,'schedstat_us':(t['runtime_ns']-b['runtime_ns'])/1000/n})
  user=(after['process']['user_ticks']-before['process']['user_ticks'])/hz*1e6/n;kernel=(after['process']['kernel_ticks']-before['process']['kernel_ticks'])/hz*1e6/n
  c=json.loads((p.parent/'results.json').read_text())['cells'][0]
  out['cpu'][variant]={'caller_tid':caller_tid,'sample_seconds':seconds,'completed':n,'rate':n/seconds,'process_user_us':user,'process_kernel_us':kernel,'process_total_us':user+kernel,'groups':groups,'threads':sorted(threads,key=lambda t:-t['total_us']),'process_minus_thread_sum_us':user+kernel-sum(groups.values()),'full_60s_rate':c['throughput_per_second'],'full_60s_process_us':c['client_cores']*1e6/c['throughput_per_second']}
if all(v in out['caller'] for v in ['main','staged']):
 a=out['caller']['main']['caller_median'];b=out['caller']['staged']['caller_median'];out['caller_target']={'staged_at_most_5_831':b<=5.831,'staged_at_most_main':b<=a,'difference_us':b-a}
if all(v in out['cpu'] for v in ['main','staged']):
 a=out['cpu']['main'];b=out['cpu']['staged'];out['same_rate']={'relative_rate_difference':abs(a['rate']-b['rate'])/max(a['rate'],b['rate']),'staged_cpu_at_most_main':b['process_total_us']<=a['process_total_us'],'difference_us':b['process_total_us']-a['process_total_us'],'full_60s_staged_cpu_at_most_main':b['full_60s_process_us']<=a['full_60s_process_us'],'full_60s_difference_us':b['full_60s_process_us']-a['full_60s_process_us']}
(j/'summary.json').write_text(json.dumps(out,indent=2));print(json.dumps({k:v for k,v in out.items() if k not in ['caller','cpu']},indent=2))
for v,x in out['caller'].items():print(v,'caller_us',x['caller_median'],x['caller_range'],'rate',x['throughput_median'])
for v,x in out['cpu'].items():print(v,'process_us',x['process_total_us'],'rate',x['rate'],'groups',x['groups'])
