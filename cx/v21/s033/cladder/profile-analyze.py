from pathlib import Path
from collections import Counter,defaultdict
import json,sys,importlib.util,os
J=Path(__file__).parent
os.environ['DEBUGINFOD_URLS']=''
os.environ['DEBUGINFOD_CACHE_PATH']=str(J/'debuginfod')
support=(J.parent/'cthin/analyze.py').read_text().split('\ndef main():')[0].replace("ROOT=Path('/root/jobs/fx/cx/v21/s033/cthin')",'ROOT=Path(__file__).parent')
support=support.replace("['perf','script'","['perf','--buildid-dir',str(ROOT/'perf-buildid'),'script','--no-inline'")
(J/'perf-support.py').write_text(support)
module=importlib.util.spec_from_file_location('perf_support',J/'perf-support.py');m=importlib.util.module_from_spec(module);module.loader.exec_module(m)
summary=json.loads((J/'summary.json').read_text());out={}
for name,row in summary.items():
 if not name.startswith('perf-'):continue
 cell=row['runs'][0];p=Path(cell['path']);cap=p/'capture';b=json.loads((cap/'before.json').read_text());a=json.loads((cap/'after.json').read_text());count=cell['messages'];roles={}
 for role in ['source','target']:
  samples=m.parse_perf(cap/(role+'.perf.data'),cap/(role+'.stacks.json'))
  groups=defaultdict(list)
  for sample in samples:
   groups[sample['tid']].append(sample)
  threads={};role_stages=Counter();role_symbols=defaultdict(lambda:Counter());unknown=0;total=0
  for tid,t in cell['roles'][role]['threads'].items():
   group=groups.get(tid,[]);weight=sum(x['period'] for x in group);factor=t['us']/weight if weight else 0
   stages=Counter();inclusive=Counter();leaf=Counter()
   for sample in group:
    fr=sample['frames'];period=sample['period'];stages[m.classify(fr)]+=period;total+=1
    if not fr or fr[0][0]=='[unknown]':unknown+=1
    if fr:
     leaf[fr[0][0]]+=period
     for symbol in set(x[0] for x in fr):inclusive[symbol]+=period
   if not weight:role_stages['unsampled']+=t['us']
   for stage,v in stages.items():role_stages[stage]+=v*factor
   symbols=[{'symbol':s,'self_us':leaf[s]*factor,'inclusive_us':v*factor} for s,v in inclusive.items()]
   for s in symbols:
    role_symbols[s['symbol']]['self_us']+=s['self_us'];role_symbols[s['symbol']]['inclusive_us']+=s['inclusive_us']
   threads[tid]={**t,'samples':len(group),'stage_us':{k:v*factor for k,v in stages.items()},'top_self':sorted(symbols,key=lambda x:-x['self_us'])[:35],'top_inclusive':sorted(symbols,key=lambda x:-x['inclusive_us'])[:60]}
  roles[role]={'threads':threads,'stage_us':dict(role_stages),'sample_count':total,'unknown_leaf_count':unknown,'symbols':sorted([{'symbol':s,**v} for s,v in role_symbols.items()],key=lambda x:-x['self_us'])}
 out[name]={'path':str(p),'messages':count,'throughput':cell['throughput'],'window_rate':cell['window_rate'],'roles':roles}
 print(name,cell['throughput'])
 for role,rr in roles.items():
  print(role,'stages',json.dumps(rr['stage_us']))
  for tid,t in sorted(rr['threads'].items(),key=lambda kv:-kv[1]['us'])[:4]:
   print('thread',tid,t['role'],round(t['us'],3),'samples',t['samples'])
   for x in t['top_self'][:5]:print(' ',round(x['self_us'],3),x['symbol'][:200])
(J/'profiles.json').write_text(json.dumps(out,indent=2))
