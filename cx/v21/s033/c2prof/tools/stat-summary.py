from pathlib import Path
from collections import defaultdict,Counter
import json,csv,re
from analyze import stat
J=Path('/root/jobs/fx/cx/v21/s033/c2prof');R=J/'raw';out={}
for d in sorted(R.glob('threadstat-*')):
 if not (d/'validated.json').exists():continue
 c=json.loads((d/'validated.json').read_text());b=json.loads((d/'before.json').read_text());a=json.loads((d/'after.json').read_text());rr={}
 for side in ['source','target']:
  msgs=c['window_source_completed' if side=='source' else 'window_target_received'];ts=stat(d/(side+'.syscalls.csv'));names=defaultdict(Counter)
  for tid,t in ts.items():
   for key in ['us','switches','migrations']:names[t['comm']][key]+=t[key]/msgs
  rr[side]={'messages':msgs,'threads':ts,'by_name':dict(names),'cpu_us_per_message':sum(t['us'] for t in ts.values())/msgs,'context_switches_per_message':sum(t['switches'] for t in ts.values())/msgs,'migrations_per_message':sum(t['migrations'] for t in ts.values())/msgs}
 out[d.name]={'roles':rr,'counts':c,'pattern':json.loads((d/'launch.json').read_text())['pattern'],'level':json.loads((d/'launch.json').read_text())['level']}
(R/'threadstat-summary.json').write_text(json.dumps(out,indent=2))
for name,r in out.items():
 print(name)
 for side,t in r['roles'].items():print(side,{k:round(t[k],3) for k in ['cpu_us_per_message','context_switches_per_message','migrations_per_message']})
traces={}
for d in sorted(R.glob('trace-*')):
 if not(d/'validated.json').exists():continue
 c=json.loads((d/'validated.json').read_text());rr={}
 for side in ['source','target']:
  m=c['window_source_completed' if side=='source' else 'window_target_received'];p=d/(side+'.syscalls.csv')
  if p.exists():
   events={}
   for l in p.read_text().splitlines():
    t=l.split(';')
    if len(t)>3:
     try:events[t[2]]=float(t[0])/m*(1000 if t[1]=='msec' else 1)
     except ValueError:pass
   rr[side]={'messages':m,'events_per_message':events}
  else:
   p=d/(side+'.trace.futex.txt');t=p.read_text();current='';ts={}
   for l in t.splitlines():
    h=re.match(r' (.*?) \((\d+)\),',l)
    if h:current=h[2];ts[current]={'comm':h[1]}
    h=re.match(r'\s+futex\s+(\d+)\s+(\d+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)\s+([\d.]+)',l)
    if h:ts[current].update(calls=int(h[1]),errors=int(h[2]),total_ms=float(h[3]),avg_ms=float(h[5]),max_ms=float(h[6]))
   rr[side]={'messages':m,'futex_threads':ts,'futex_per_message':sum(v.get('calls',0) for v in ts.values())/m,'blocked_wall_us_per_message':sum(v.get('total_ms',0) for v in ts.values())*1000/m}
 traces[d.name]={'roles':rr}
(R/'syscalls-summary.json').write_text(json.dumps(traces,indent=2))
for name,r in traces.items():print(name,{side:t.get('events_per_message',t.get('futex_per_message')) for side,t in r['roles'].items()})
