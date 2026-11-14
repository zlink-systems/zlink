from pathlib import Path
from collections import defaultdict,Counter
import json,csv
J=Path('/root/jobs/fx/cx/v21/s033/c2prof');R=J/'raw';data=json.loads((R/'profiles-summary.json').read_text())

def mean(xs):return sum(xs)/len(xs) if xs else 0
comp={};csvrows=[];text=['# Thread별 top 25 symbol','', '각 thread name의 두 반복을 합쳤다. Self/inclusive는 해당 thread의 cpu-clock period 대비 백분율이다. 같은 이름을 가진 thread는 합쳤으며 개별 TID는 profiles-summary.json에 보존했다. Δ CPU는 각 반복의 task-clock ÷ 해당 side 완료 수를 평균한 L2−L1이다. Inclusive 행은 중첩되므로 합하지 않는다.','']
for pattern in ['request-backpressure','send-saturation']:
 comp[pattern]={}
 for side in ['source','target']:
  levels={}
  for level in [1,2]:
   runs=[r for r in data.values() if r['pattern']==pattern and r['level']==level]
   stages={s:mean([r['roles'][side]['stages'].get(s,0) for r in runs]) for s in set(s for r in runs for s in r['roles'][side]['stages'])}
   weights=Counter();total=0;names={};allsyms={};cpunames=defaultdict(Counter)
   for run in runs:
    rr=run['roles'][side];d=Path(run['path']);agg=json.loads((d/(side+'.perf.aggregates-v2.json')).read_text())
    for g in agg['groups'].values():weights.update(g['stages']);total+=g['weight']
    for tid,t in rr['threads'].items():
     name=t['comm'];g=names.setdefault(name,{'weight':0,'self':Counter(),'inclusive':Counter(),'samples':0});samp=agg['groups'].get(tid,{});g['weight']+=samp.get('weight',0);g['samples']+=samp.get('samples',0);g['self'].update(samp.get('self',{}));g['inclusive'].update(samp.get('inclusive',{}))
     c=cpunames[name];c['cpu_us_per_message']+=t['cpu_us_per_message']/len(runs);c['context_switches_per_message']+=t['context_switches_per_message']/len(runs);c['migrations_per_message']+=t['migrations_per_message']/len(runs)
     for sym,v in t['symbols'].items():
      ss=allsyms.setdefault(sym,Counter());ss['self_us']+=v['self_us']/len(runs);ss['inclusive_us']+=v['inclusive_us']/len(runs)
      nn=g.setdefault('cpu_symbols',defaultdict(Counter));nn[sym]['self_us']+=v['self_us']/len(runs);nn[sym]['inclusive_us']+=v['inclusive_us']/len(runs)
   levels[level]={'cpu_us_per_message':mean([r['roles'][side]['cpu_us_per_message'] for r in runs]),'context_switches_per_message':mean([r['roles'][side]['context_switches_per_message'] for r in runs]),'migrations_per_message':mean([r['roles'][side]['migrations_per_message'] for r in runs]),'stages':stages,'stage_sample_share':{k:v/total*100 for k,v in weights.items()},'by_name':dict(cpunames),'symbols':allsyms,'threads':names,'paths':[r['path'] for r in runs]}
  comp[pattern][side]=levels
  for name in sorted(set(levels[1]['threads'])|set(levels[2]['threads'])):
   text += [f'## {pattern} · {side} · {name}','']
   for kind in ['self','inclusive']:
    symbols=set(levels[1]['threads'].get(name,{}).get(kind,{}))|set(levels[2]['threads'].get(name,{}).get(kind,{}))
    def share(level,s,k):
     t=levels[level]['threads'].get(name,{});return t.get(k,{}).get(s,0)/t.get('weight',1)*100 if t.get('weight',0) else 0
    symbols=sorted(symbols,key=lambda s:max(share(1,s,kind),share(2,s,kind)),reverse=True)[:25]
    text += [f'### {kind} top 25','', '| 순위 | symbol | self % L1 → L2 | inclusive % L1 → L2 | Δ self µs/msg | Δ inclusive µs/msg |','|---:|---|---:|---:|---:|---:|']
    for rank,sym in enumerate(symbols,1):
     cs=[levels[l]['threads'].get(name,{}).get('cpu_symbols',{}).get(sym,{}) for l in [1,2]]
     row={'pattern':pattern,'side':side,'thread_name':name,'sort':kind,'rank':rank,'symbol':sym,'L1_self_share':share(1,sym,'self'),'L2_self_share':share(2,sym,'self'),'L1_inclusive_share':share(1,sym,'inclusive'),'L2_inclusive_share':share(2,sym,'inclusive'),'delta_self_us':cs[1].get('self_us',0)-cs[0].get('self_us',0),'delta_inclusive_us':cs[1].get('inclusive_us',0)-cs[0].get('inclusive_us',0)};csvrows.append(row)
     text.append(f"|{rank}|{sym.replace('|','&#124;')}|{row['L1_self_share']:.3f} → {row['L2_self_share']:.3f}|{row['L1_inclusive_share']:.3f} → {row['L2_inclusive_share']:.3f}|{row['delta_self_us']:+.4f}|{row['delta_inclusive_us']:+.4f}|")
    text+=['']
(R/'comparison.json').write_text(json.dumps(comp,indent=2));(R/'thread-top25.md').write_text('\n'.join(text))
with (R/'thread-top25.csv').open('w') as f:
 w=csv.DictWriter(f,fieldnames=list(csvrows[0]));w.writeheader();w.writerows(csvrows)
for pattern,sides in comp.items():
 print(pattern)
 for side,levels in sides.items():
  print(side)
  for level,r in levels.items():print(' L',level,'cpu',round(r['cpu_us_per_message'],4),'cs',round(r['context_switches_per_message'],4),'stage_us',{k:round(v,4) for k,v in r['stages'].items()},'stage_shares',{k:round(v,3) for k,v in r['stage_sample_share'].items()})
  for name in sorted(set(levels[1]['by_name'])|set(levels[2]['by_name'])):
   a=levels[1]['by_name'].get(name,{});b=levels[2]['by_name'].get(name,{})
   print(' ',name,round(a.get('cpu_us_per_message',0),4),round(b.get('cpu_us_per_message',0),4),round(a.get('context_switches_per_message',0),4),round(b.get('context_switches_per_message',0),4))
