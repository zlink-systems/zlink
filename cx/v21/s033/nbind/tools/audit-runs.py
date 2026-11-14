import json,pathlib,os
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/nbind');R=J/'raw';rows=[]
for p in sorted(R.iterdir()):
 if not p.is_dir() or not (p.name.startswith('final-audit-') or p.name.startswith('window-') or p.name.startswith('permit-')) or not (p/'events.json').exists():continue
 e=json.loads((p/'events.json').read_text())
 if not any(x['event']=='valid' for x in e):continue
 s=json.loads((p/'active-source.json').read_text());t=json.loads((p/'active-target.json').read_text());w=json.loads((p/'warmup-source.json').read_text());wt=json.loads((p/'warmup-target.json').read_text())
 assert s['errors']==t['errors']==w['errors']==wt['errors']==0
 assert s['submitted']==s['completed']==t['received']
 assert w['submitted']==w['completed']==wt['allPhaseMessages']
 states=[]
 for f in p.glob('threads-*.json'):
  for role,threads in json.loads(f.read_text()).items():
   for x in threads:
    assert x['affinity'] and set(x['affinity'])<={6,7,14,15},(p,role,x)
    state=x['stat'].rsplit(') ',1)[1].split()[0]
    assert state!='T',(p,role,x)
    states.append(dict(role=role,policy=x['policy'],affinity=x['affinity'],state=state))
 exits=[x for x in e if x['event']=='exit'];assert len(exits)==2
 assert all(x['lifetime_s']<=20 for x in exits)
 modules=[]
 for role in ['source','target']:
  d=json.loads((p/f'{role}-instrument.json').read_text());modules+=d['modules']
  assert not any('/root/worktree/' in v and '/root/worktree/zlink-v21-s033/nbind/' not in v for v in d['modules']),[v for v in d['modules'] if '/root/worktree/' in v and '/nbind/' not in v]
 maps='\n'.join((p/f'{role}-maps.txt').read_text() for role in ['source','target'])
 assert not any('/root/worktree/' in v and '/root/worktree/zlink-v21-s033/nbind/' not in v for v in maps.splitlines()),p
 row=dict(label=p.name,active_messages=s['completed'],warmup_messages=w['completed'],errors=0,max_lifetime_s=max(x['lifetime_s'] for x in exits),policies=sorted({x['policy'] for x in states}),affinities=sorted({','.join(map(str,x['affinity'])) for x in states}),loaded_worktree_verified=True)
 rows.append(row)
(R/'profile-audit.json').write_text(json.dumps(rows,indent=2)+'\n')
print(json.dumps(dict(valid_runs=len(rows),bench_processes=len(rows)*2,max_lifetime=max(x['max_lifetime_s'] for x in rows),policies=sorted({v for x in rows for v in x['policies']}),affinities=sorted({v for x in rows for v in x['affinities']})),indent=2))
