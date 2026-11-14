from pathlib import Path
from collections import defaultdict,Counter
import csv,json
R=Path('/root/jobs/fx/cx/v21/s033/c2prof/raw');runs=json.loads((R/'profiles-summary.json').read_text());groups=defaultdict(lambda:defaultdict(lambda:{'self':Counter(),'inclusive':Counter(),'weight':0,'cpu':defaultdict(Counter),'tids':[]}));rawrows=[];threads=[]
for runname,r in runs.items():
 for side,rr in r['roles'].items():
  a=json.loads((Path(r['path'])/(side+'.perf.aggregates-v2.json')).read_text());ordinals=Counter()
  for tid,t in sorted(rr['threads'].items(),key=lambda kv:int(kv[0])):
   ordinals[t['comm']]+=1;label=t['comm']+' #'+str(ordinals[t['comm']]);key=(r['pattern'],side,label);g=groups[key][r['level']];g['tids'].append(tid);s=a['groups'].get(tid,{});g['weight']+=s.get('weight',0);g['self'].update(s.get('self',{}));g['inclusive'].update(s.get('inclusive',{}))
   threads.append({'run':runname,'level':r['level'],'pattern':r['pattern'],'side':side,'tid':tid,'comm':t['comm'],'ordinal':ordinals[t['comm']],'cpu_us_per_message':t['cpu_us_per_message'],'context_switches_per_message':t['context_switches_per_message'],'samples':t['samples']})
   for sym,v in t['symbols'].items():
    for k in ['self_us','inclusive_us']:g['cpu'][sym][k]+=v[k]/2
   for mode in ['self','inclusive']:
    for rank,(sym,v) in enumerate(sorted(t['symbols'].items(),key=lambda kv:-kv[1][mode+'_us'])[:25],1):rawrows.append({'run':runname,'level':r['level'],'pattern':r['pattern'],'side':side,'tid':tid,'comm':t['comm'],'sort':mode,'rank':rank,'symbol':sym,**v})
for name,rows in [('thread-top25-by-tid.csv',rawrows),('thread-cpu-by-tid.csv',threads)]:
 with (R/name).open('w') as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
rows=[]
for (pattern,side,label),levels in sorted(groups.items()):
 def share(level,s,mode):
  g=levels[level];return g[mode][s]/g['weight']*100 if g['weight'] else 0
 for mode in ['self','inclusive']:
  syms=set(levels[1][mode])|set(levels[2][mode]);syms=sorted(syms,key=lambda s:max(share(1,s,mode),share(2,s,mode)),reverse=True)[:25]
  for rank,sym in enumerate(syms,1):
   row={'pattern':pattern,'side':side,'thread_name_and_ordinal':label,'L1_tids':','.join(levels[1]['tids']),'L2_tids':','.join(levels[2]['tids']),'sort':mode,'rank':rank,'symbol':sym}
   for l in [1,2]:
    for m in ['self','inclusive']:row[f'L{l}_{m}_share']=share(l,sym,m)
   for m in ['self','inclusive']:row[f'delta_{m}_us_per_message']=levels[2]['cpu'][sym][m+'_us']-levels[1]['cpu'][sym][m+'_us']
   rows.append(row)
with (R/'thread-top25-by-slot.csv').open('w') as f:w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
print('raw top25 rows',len(rawrows),'individual threads',len(threads),'side-by-side rows',len(rows))
