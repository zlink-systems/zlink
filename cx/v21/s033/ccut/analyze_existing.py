import sys,ast,json,re
from pathlib import Path
from collections import Counter,defaultdict
sys.dont_write_bytecode=True
root=Path('/root/jobs/fx/cx/v21/s033/ccall');out=Path('/root/jobs/fx/cx/v21/s033/ccut')
sys.path.insert(0,str(root/'scripts'))
tree=ast.parse((root/'scripts/summarize.py').read_text());tree.body=tree.body[:next(i for i,n in enumerate(tree.body) if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='results' for t in n.targets))]
ns={'__file__':str(root/'scripts/summarize.py')};exec(compile(tree,'existing_summarize','exec'),ns)
summary=json.loads((root/'profile-summary.json').read_text()); report={}
for impl, data in summary.items():
 ss=json.loads((root/f'out-profile/run1/{impl}-request-backpressure-1024/capture/source.samples.json').read_text())
 ss=[s for s in ss if s['tid']==data['critical_tid']];factor=data['critical']['cpu_us']/sum(s['period'] for s in ss)*(17.48/data['critical']['cpu_us'] if impl=='zlink-framework-cpp' else 1)
 groups=defaultdict(Counter);stacks=defaultdict(Counter)
 for s in ss:
  fr=s['frames']; stage=ns['stage'](fr);w=s['period']*factor
  syms=[s for s,d in fr]
  leaf=syms[0] if syms else 'EMPTY';groups[stage][leaf]+=w
  # first binding/Core frame and its nearest non-standard higher frame
  if stage=='binding submit':
   first=next((sym for sym,dso in fr if ns['BINDING'].search(sym) or '/libzlink.so' in dso),'unresolved')
   stacks[first]['\n'.join(syms[:15])]+=w
 report[impl]={'cpu_us':sum(sum(c.values()) for c in groups.values()),'stages':{stage:dict(c.most_common()) for stage,c in groups.items()},'binding_stacks':{owner:dict(c.most_common()) for owner,c in stacks.items()}}
(out/'attribution.json').write_text(json.dumps(report,indent=2))
for impl,r in report.items():
 print('\nIMPLEMENTATION',impl,r['cpu_us'])
 for stage, entries in r['stages'].items():
  print('\nSTAGE',stage,round(sum(entries.values()),4))
  for s,v in list(entries.items())[:22]:print(f'{v:.4f}',s[:180])
 print('\nBINDING OWNERS')
 for s,c in sorted(r['binding_stacks'].items(),key=lambda it:-sum(it[1].values())):print(f'{sum(c.values()):.4f}',s[:180])
