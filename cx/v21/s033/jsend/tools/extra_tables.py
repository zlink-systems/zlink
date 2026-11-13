from pathlib import Path
import json,collections
p=Path('/root/jobs/fx/cx/v21/s033/jsend');ns={};s=(p/'tools/summarize_ss.py').read_text();exec(s[:s.index('out={}')],ns);parse=ns['parse'];out={}
for name in ('main-50k-combined','prototype-50k-combined','binding-50k-combined','direct-50k-combined','nagle-50k-combined'):
 run=p/'runs'/name
 if not (run/'warmup-ss.json').exists():continue
 seq=json.load(open(run/'warmup-ss.json'));active=json.load(open(run/'capture/ss.json')) if (run/'capture/ss.json').exists() else [];r={}
 for row in seq+active:
  for key,d in parse(row['ss']).items():
   key='->'.join(map(str,key));r.setdefault(key,[]).append(d['rcvbuf'])
 out[name]={k:{'first':v[0],'last':v[-1],'min':min(v),'max':max(v),'samples':len(v)} for k,v in r.items()}
(p/'rcvbuf-summary.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
