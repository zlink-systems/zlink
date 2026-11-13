import json,statistics
from pathlib import Path
J=Path(__file__).parent;results={}
for p in J.glob('out-profile/run1/*/capture/*.samples.json'):
 ss=json.loads(p.read_text());cap=p.parent;b=json.loads((cap/'before.json').read_text());a=json.loads((cap/'after.json').read_text());wt=sum(s['period'] for s in ss)
 def frac(pred):return sum(s['period'] for s in ss if pred(s))/wt if wt else 0
 results[str(p.relative_to(J))]={'sample_time_min':min(s['time'] for s in ss),'sample_time_max':max(s['time'] for s in ss),'sample_span_seconds':max(s['time'] for s in ss)-min(s['time'] for s in ss),'samples':len(ss),'empty':frac(lambda s:not s['frames']),'unknown_leaf':frac(lambda s:s['frames'] and s['frames'][0][0]=='[unknown]'),'fully_unknown':frac(lambda s:s['frames'] and all(f[0]=='[unknown]' for f in s['frames'])),'median_frames':statistics.median(len(s['frames']) for s in ss),'max_frames':max(len(s['frames']) for s in ss)}
(J/'quality.json').write_text(json.dumps(results,indent=2));print(json.dumps(results,indent=2))
