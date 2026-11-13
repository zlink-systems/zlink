import json,re,datetime,collections
from pathlib import Path
job=Path('/root/jobs/fx/cx/v21/s033/jl57bp')
root=job/'raw/diag-L5_7/request-backpressure-1024/warm/run1/zlink-framework-java-request-backpressure-1024'
sent={};hist=collections.Counter();examples={};summaries=[];stats=[]
for n,line in enumerate((root/'source.log').open(),1):
 if line.startswith('client_error'): summaries.append({'line':n,'text':line.strip()})
 if 'event_id=zlink.message_flow' not in line: continue
 fields=dict(re.findall(r'(phase|flow|outcome)=([^ ]+)',line.strip()))
 flow=fields.get('flow')
 if not flow: continue
 t=datetime.datetime.fromisoformat(line.split()[0]).timestamp()
 if fields.get('phase')=='sent': sent[flow]=(t,n)
 elif fields.get('phase')=='reply_received' and flow in sent:
  t0,n0=sent.pop(flow); dt=t-t0; outcome=fields.get('outcome');category=(outcome,'>=30s' if dt>=30 else '<30s')
  key=' '.join(category);hist[key]+=1
  examples.setdefault(key,[])
  if len(examples[key])<3: examples[key].append({'flow':flow,'duration_s':round(dt,3),'source_sent_line':n0,'source_terminal_line':n,'terminal':line.strip()})
rows=[json.loads(x) for x in (root/'stats-timeline.jsonl').read_text().splitlines()]
for phase in ['warmup','active']:
 rr=[r for r in rows if r.get('source',{}).get('phase')==phase]
 if not rr:continue
 t0=rr[0]['unix_ns']; mx=max(rr,key=lambda x:x['source'].get('currentInFlight',0));firsterr=next((r for r in rr if r['source'].get('errors',0)),None)
 stats.append({'phase':phase,'sampled_max_in_flight':mx['source']['currentInFlight'],'max_row':mx,'first_error_after_first_phase_sample_s':(firsterr['unix_ns']-t0)/1e9 if firsterr else None,'first_error_row':firsterr,'last_sample':rr[-1]})
result={'source_error_summaries':summaries,'flow_duration_histogram':hist,'examples':examples,'timeline':stats,'unmatched_sent':len(sent),'note':'Trace run changes throughput; intervals are sent log emission to terminal log emission, not exact internal queue residence. Error flow has no errorType/errorMessage; short vs >=30s labels are timing groups, not per-flow typed classification.'}
(job/'raw/trace-analysis.json').write_text(json.dumps(result,indent=2)+'\n')
print(json.dumps({'histogram':hist,'error_summaries':summaries,'timeline':[{k:v for k,v in r.items() if k not in ('max_row','first_error_row','last_sample')} for r in stats]}))
