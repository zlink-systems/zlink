import json,pathlib,statistics,math,csv,hashlib,subprocess
j=pathlib.Path(__file__).parent
rows=json.loads((j/'raw/observed-analysis.json').read_text());summaries=[json.loads(l) for l in (j/'summary.jsonl').read_text().splitlines()];look={(r['label'],r['run']):r for r in summaries}

def windows(label,run,bounds):
 ss=[json.loads(l) for l in (j/f'raw/{label}-{run:02d}.proc.jsonl').read_text().splitlines()];out=[]
 for a,b in bounds:
  vals=[x for x in ss if a<=x['t']<=b and x.get('memory')];m=[x['memory'] for x in vals]
  if len(m)<2:continue
  n=m[-1]['latency_count']-m[0]['latency_count'];out.append({'window':[a,b],'samples':len(m),'received':n,'mean_ms':(m[-1]['sum_ns']-m[0]['sum_ns'])/n/1e6 if n else None,'backlog_mean':statistics.mean((v['backlog_lo']+v['backlog_hi'])/2 for v in m),'backlog_max':max(v['backlog_hi'] for v in m)})
 return out

valid=[];missing=[];violations=[];states=[];all_meta=[]
for r in rows:
 key=r['label'],r['run'];base=pathlib.Path(r['raw']);meta=json.loads(base.with_suffix('.json').read_text());ss=[json.loads(x) for x in base.with_suffix('.proc.jsonl').read_text().splitlines()];all_meta.append(meta)
 mem=[x['memory'] for x in ss if x.get('memory')]
 for m in mem:
  if not 0<=m['received']-m['latency_count']<=2:violations.append([key,m])
 if r['label'] in ('natural','natural-extra'):
  (valid if mem else missing).append(key)
 if mem:
  last=mem[-1];expected=r['throughput']*5;states.append({'label':r['label'],'run':r['run'],'last_count':last['latency_count'],'reported_count':expected,'last_count_fraction':last['latency_count']/expected})
 before=json.loads(base.with_suffix('.before.json').read_text());after=json.loads(base.with_suffix('.after.json').read_text());b=before['threads'];a=after['threads'];union=b.keys()|a.keys();s=look[key]
 assert union==s['deltas_tid_union'].keys()
 for tid in union:
  assert s['deltas_tid_union'][tid]['runtime_ns']==max(0,a.get(tid,{}).get('schedstat',[0])[0]-b.get(tid,{}).get('schedstat',[0])[0])
 dt=s['window_after_s']-s['window_before_s'];r['role_cpu_fraction']={role:s['deltas_tid_union'][str(tid)]['runtime_ns']/1e9/dt for role,tid in s['roles'].items()}
 r['role_frequency_ghz']={}
 for role,tid in s['roles'].items():
  freqs=[x['frequency_khz'].get(str(x['threads'][str(tid)]['cpu'])) for x in ss if x['t']>.2 and str(tid) in x['threads']];freqs=[v for v in freqs if v];r['role_frequency_ghz'][role]=statistics.mean(freqs)/1e6 if freqs else None
 if mem:r['state_addr']=hex(mem[0]['state_addr']);r['payload_vector_addr']=hex(mem[0]['payload_vector_addr'])

natural=[r for r in rows if r['label']=='natural'];groups={}
for group,predicate in [('low_lt_0.2ms',lambda x:x<.2),('middle_0.2_to_0.9ms',lambda x:.2<=x<.9),('high_ge_0.9ms',lambda x:x>=.9)]:
 x=[r for r in natural if predicate(r['latency'])];depth=[r['backlog_avg'] for r in x if r['backlog_avg'] is not None];groups[group]={'n':len(x),'mean_ms_range':[min(r['latency'] for r in x),max(r['latency'] for r in x)],'backlog_mean_range':[min(depth),max(depth)],'runs':[r['run'] for r in x]}
pts=[(r['latency'],r['backlog_avg']/r['throughput']*1000) for r in rows if r['label'] in ('natural','natural-extra') and r['backlog_avg'] is not None]
x,y=map(list,zip(*pts));mx,my=statistics.mean(x),statistics.mean(y);corr=sum((a-mx)*(b-my) for a,b in pts)/math.sqrt(sum((a-mx)**2 for a in x)*sum((b-my)**2 for b in y))
result={'same_binary_sha256':sorted({m['binary_sha256'] for m in all_meta}),'returncodes':[r['rc'] for r in summaries],'natural20_groups':groups,'natural20_throughput_range':[min(r['throughput'] for r in natural),max(r['throughput'] for r in natural)],'valid_queue_natural_and_extra':valid,'missing_queue_observation':missing,'memory_counter_violations':len(violations),'last_counter_checks':states,'queue_over_rate_vs_reported_mean_correlation':corr,'queue_cycle_windows':windows('queue-cycle',1,[(.2,.8),(1.15,1.9),(2,2.9),(3.2,4.8)]),'cap2M_windows':windows('cap2M',1,[(.2,.8),(1.5,2.5),(3,4.8)]),'rows':rows}
(j/'raw/final-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
with (j/'raw/runs.tsv').open('w') as f:
 writer=csv.writer(f,delimiter='\t');writer.writerow(['label','run','mean_ms','p95_ms','throughput','backlog_avg','receiver_cpu_fraction','sender_cpu_fraction','raw'])
 for r in rows:writer.writerow([r['label'],r['run'],r['latency'],r['latency_p95'],r['throughput'],r['backlog_avg'],r['role_cpu_fraction']['receiver'],r['role_cpu_fraction']['sender'],r['raw']])
print(json.dumps({k:v for k,v in result.items() if k not in ('rows','last_counter_checks','valid_queue_natural_and_extra')},indent=2))
for r in rows:
 if r['label'].startswith('cap'):print(r['label'],r['latency'],r['backlog_avg'],r['role_cpu_fraction'],r.get('state_addr'),r.get('payload_vector_addr'))
