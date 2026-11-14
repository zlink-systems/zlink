from pathlib import Path
import json,hashlib,subprocess,struct,time
J=Path('/root/jobs/fx/cx/v21/s033/c2prof');R=J/'raw';W=Path('/root/worktree/zlink-v21-s033/cfx');out={};total=0
for d in sorted(R.iterdir()):
 if not d.is_dir():continue
 included=d.name.startswith(('perf-final-','alloc-v2-','alloc-stacks-v2-','threadstat-','trace-'))
 if not(d/'results.json').exists():
  if (d/'failure.json').exists():out[d.name]={'included':False,'failure':json.loads((d/'failure.json').read_text())}
  continue
 c=json.loads((d/'results.json').read_text())['cells'][0];tar=json.loads((d/'target-final.json').read_text()) if(d/'target-final.json').exists() else {};reason=''
 if d.name.startswith('perf-') and not d.name.startswith('perf-final-'):reason='preliminary/default perf clock or startup trial; excluded from final ranking'
 if d.name.startswith('alloc-') and '-v2-' not in d.name:reason='preload entered taskset before binary exec; empty counter; excluded from allocation results'
 if d.name.startswith('stat-'):reason='per-process exploratory stat; final per-thread runs supersede'
 status={'included':included,'reason':reason,'submitted':c['submitted'],'completed':c['completed'],'received':tar.get('received'),'errors':c['errors'],'target_errors':tar.get('errors'),'header_validation_failures':c.get('header_validation_failures',0),'abandoned':c.get('abandoned',0),'active_ms':c['trigger']['durationMs'],'warmup_ms':c['trigger'].get('warmup'),'affinities':[],'paused_threads':[]}
 for sn in ['before.json','after.json','timeline.json']:
  if not(d/sn).exists():continue
  snapshots=json.loads((d/sn).read_text());snapshots=snapshots if isinstance(snapshots,list) else [snapshots]
  for snap in snapshots:
   for role,proc in snap['processes'].items():
    for tid,t in proc['threads'].items():
     if t['affinity'] not in status['affinities']:status['affinities'].append(t['affinity'])
     if t['state'].startswith('T'):status['paused_threads'].append([role,tid,t])
 if included:
  assert status['submitted']==status['completed']==status['received']
  assert status['errors']==status['target_errors']==status['header_validation_failures']==status['abandoned']==0
  assert status['active_ms']==5000 and status['warmup_ms']==1000
  assert not status['paused_threads']
  assert status['affinities']==['6-7,14-15'],status['affinities']
  if d.name.startswith('alloc'):
   for side in ['source','target']:
    buf=(d/(side+'.counter')).read_bytes();assert struct.unpack_from('<I',buf,4)[0]>0
  total+=1
 out[d.name]=status
initial=json.loads((R/'binary-provenance.json').read_text());final={}
for p,t in initial['binaries'].items():
 final[p]=hashlib.sha256(Path(p).read_bytes()).hexdigest();assert final[p]==t['sha256']
status=subprocess.check_output(['git','status','--short'],cwd=W,text=True);head=subprocess.check_output(['git','rev-parse','HEAD'],cwd=W,text=True).strip();assert status=='' and head==initial['head']
summary={'utc':time.time(),'included_runs':total,'run_details':out,'head':head,'worktree_status':status,'unchanged_binary_hashes':final,'profile_settings':'cpu-clock 2999 Hz, dwarf 8192, CLOCK_MONOTONIC, source/target separate perf files captured concurrently','guard_pauses_observed':0,'SIGCONT_used':False,'measure_lock_used':False}
(R/'run-audit.json').write_text(json.dumps(summary,indent=2));print('included',total,'all errors0 submitted=completed=received, affinity exact, worktree clean, binary hashes unchanged')
