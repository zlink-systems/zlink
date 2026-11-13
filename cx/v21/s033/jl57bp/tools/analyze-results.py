import json
from pathlib import Path
job=Path('/root/jobs/fx/cx/v21/s033/jl57bp')
rows=[]
for p in sorted(job.glob('raw/*/request-backpressure-*/*/run1/*/results*.json')):
 data=json.loads(p.read_text());c=data['cells'][0]
 if c.get('contaminated'):continue
 cap=sum(e['count'] for e in c.get('client_error_summary',[]) if 'capacity is unavailable' in e['message'])
 timeout=sum(e['count'] for e in c.get('client_error_summary',[]) if 'timed out' in e['message'])
 rows.append({'mode':p.parts[-6], 'size':c['payload_size'], 'kind':p.parts[-4], 'phase':'warmup' if p.name=='results.warmup.json' else 'active', 'raw':str(p.relative_to(job)), **{k:c.get(k) for k in ['submitted','completed','errors','abandoned','peak_in_flight']},'capacity_unavailable':cap,'timeouts':timeout,'received':(c.get('target_stats') or {}).get('received')})
(job/'raw/results-summary.json').write_text(json.dumps(rows,indent=2)+'\n')
print(json.dumps(rows,indent=2))
timelines=[]
for p in sorted(job.glob('raw/*/request-backpressure-*/*/run1/*/stats-timeline.jsonl')):
 rr=[json.loads(x) for x in p.read_text().splitlines()]
 for phase in ('warmup','active'):
  selected=[r for r in rr if r.get('source',{}).get('phase')==phase]
  if not selected:continue
  row=max(selected,key=lambda r:r['source'].get('currentInFlight',0))
  latency=max(selected,key=lambda r:r.get('target',{}).get('p99Micros',0))
  timelines.append({'raw':str(p.relative_to(job)),'phase':phase,'max_current_in_flight':row['source']['currentInFlight'],'sample':row, 'max_observed_target_p99_ms':latency.get('target',{}).get('p99Micros',0)/1000,'latency_sample':latency})
(job/'raw/timeline-peaks.json').write_text(json.dumps(timelines,indent=2)+'\n')
