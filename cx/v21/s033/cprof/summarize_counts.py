from pathlib import Path
from collections import Counter
import json,csv,re
J=Path(__file__).parent
out={}
for cell in (J/'out-fastcounts/run1').glob('*'):
 cap=cell/'capture'
 if not (cap/'after.json').exists():continue
 b=json.loads((cap/'before.json').read_text());a=json.loads((cap/'after.json').read_text())
 n=a['stats']['source']['completed']-b['stats']['source']['completed'];sec=a['monotonic']-b['monotonic'];res={'messages':n,'seconds':sec,'rate':n/sec,'roles':{}}
 for role in ['source','target']:
  totals=Counter();threads={}
  for row in csv.reader((cap/f'{role}.stat.csv').open()):
   if len(row)<6 or row[0].startswith('#'):continue
   if not re.fullmatch(r'[0-9.]+',row[1].strip()):continue
   tid=row[0].rsplit('-',1)[-1];value=float(row[1]);event=row[3]
   threads.setdefault(tid,Counter())[event]+=value;totals[event]+=value
  bt=b['processes'][role]['threads'];at=a['processes'][role]['threads']
  crit=max(at,key=lambda tid:at[tid]['runtime_ns']-bt.get(tid,{}).get('runtime_ns',0))
  for tid,after in a['processes'][role]['alloc'].items():
   before=b['processes'][role]['alloc'].get(tid,{})
   for event in ['malloc','calloc','realloc','free']:
    value=after[event]-before.get(event,0);threads.setdefault(tid,Counter())[event]+=value;totals[event]+=value
  res['roles'][role]={'critical_tid':crit,'total':dict(totals),'per_message':{k:v/n for k,v in totals.items()},'threads':{k:dict(v) for k,v in threads.items()},'critical_per_message':{k:v/n for k,v in threads[crit].items()}}
 out[cell.name]=res
(J/'counts-summary.json').write_text(json.dumps(out,indent=2))
for cell,r in out.items():
 print(cell,'rate',round(r['rate']))
 for role,rr in r['roles'].items():
  print(role,{k:round(v,3) for k,v in rr['per_message'].items()},'critical',{k:round(v,3) for k,v in rr['critical_per_message'].items()})
