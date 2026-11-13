from pathlib import Path
import json,hashlib,subprocess,csv,collections,math,re
j=Path(__file__).parent;r=Path('/root/worktree/zlink-1639c')
old=json.loads((j/'original-state.json').read_text())
now={'branch':subprocess.check_output(['git','branch','--show-current'],cwd=r,text=True).strip(),'head':subprocess.check_output(['git','rev-parse','HEAD'],cwd=r,text=True).strip(),'status':subprocess.check_output(['git','status','--short'],cwd=r,text=True),'sha256':{f:hashlib.sha256((r/f).read_bytes()).hexdigest() for f in old['sha256']}}
now['matches_initial']={k:now[k]==old[k] for k in ('branch','head','status','sha256')}
(j/'preservation.json').write_text(json.dumps(now,indent=2));assert all(now['matches_initial'].values()),now['matches_initial']
quality=[]
for root in ('perf-cpu','pm-final'):
 for p in sorted((j/root).glob('*/*/run1/*/results.json')) if root=='perf-cpu' else sorted((j/root).glob('*/run1/*/results.json')):
  d=json.loads(p.read_text())['cells'][0];target=json.loads((p.parent/'target-stats.json').read_text())
  q={'directory':str(p.parent),'errors':d['errors'],'abandoned':d['abandoned'],'contaminated':d.get('contaminated',False),'drain_bound_hit':d.get('drain_bound_hit',False),'source_completed':d['completed'],'target_received':target['received'],'target_rejected':d.get('server_rejected_count',0)}
  assert q['errors']==q['abandoned']==0 and not q['contaminated'] and not q['drain_bound_hit'],q
  assert q['source_completed']==q['target_received']+q['target_rejected'],q
  capture=p.parent/'capture';before=json.loads((capture/'before.json').read_text());after=json.loads((capture/'after.json').read_text())
  assert after['stats']['target']['received']>before['stats']['target']['received']
  if root=='perf-cpu':
   assert (capture/'perf.data').stat().st_size>1000 and (capture/'perf.script').stat().st_size>1000
   q['profiler_exit']=(capture/'perf-exit.txt').read_text();assert q['profiler_exit'] in ('0','-2','130')
  else:
   probes=json.loads((capture/'pm-probes.json').read_text());assert len(probes)==250
   q['probes']=len(probes)
   assert (capture/'sched.data').stat().st_size>1000 and (capture/'sched.script').stat().st_size>1000
  quality.append(q)
assert len(quality)==6
for v in ('main','branch'):
 groups=collections.defaultdict(list)
 for row in csv.reader((j/f'pm-final-{v}.csv').open()):groups[int(row[0])].append(row)
 assert len(groups)==251 and sum(len(z) for z in groups.values())<65536
 assert all(z[0][3]=='request_start' and z[-1][3]=='request_complete' for z in groups.values())
 for z in groups.values():assert all(int(z[i][1])<=int(z[i+1][1]) for i in range(len(z)-1))
(j/'quality.json').write_text(json.dumps({'accepted_cells':quality,'target_worktree_preserved':True,'trace_groups_per_variant':251,'notes':'Profiler exit -2 is the intentional process-group SIGINT stop after perf flushes its data. PM group 1 is cold setup, excluded; 250 active probes per variant. Stage intervals are monotonic.'},indent=2))
print('6 accepted cells: errors=0, abandoned=0, count agreement; 250 PM probes/variant; original branch, HEAD, status and all ten changed-file hashes match')
