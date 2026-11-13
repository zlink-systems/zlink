import json
from pathlib import Path
job=Path('/root/jobs/fx/cx/v21/s033/jl57bp');rows=[]
for p in sorted(job.glob('raw/*/request-backpressure-*/*/run1/*/results.json')):
 c=json.loads(p.read_text())['cells'][0];d=p.parent;s=(d/'scheduling.txt').read_text()
 assert not c.get('contaminated'),p
 assert 'scheduling_invalid=' not in s,p
 policies=[line.split(': ',1)[1] for line in s.splitlines() if 'current scheduling policy:' in line]
 assert policies and set(policies)=={'SCHED_OTHER'},p
 masks={}
 for phase,cpus in [('source_start','0-5'),('target_start','8-13'),('mid_active_source','0-5'),('mid_active_target','8-13')]:
  masks[phase]=[line.rsplit('cpus=',1)[1] for line in s.splitlines() if line.startswith(phase+'_task_affinity')]
  assert masks[phase] and set(masks[phase])=={cpus},(p,phase,masks[phase])
 for cpu in (0,8):
  for state in (2,3):assert f'cpu{cpu}.state{state}.disable_at_run_start=1' in s,p
 assert c['submitted']==c['completed']+c['errors']+c['abandoned'],p
 assert c['errors']==sum(x['count'] for x in c.get('client_error_summary',[]))+c.get('client_error_other_count',0),p
 assert c['target_stats']['errors']==c.get('server_errors',0)==0,p
 assert c['completed']<=c['target_stats']['received']<=c['completed']+c['errors']+c['abandoned'],p
 w=json.loads((d/'results.warmup.json').read_text())['cells'][0]
 assert w['submitted']==w['completed']+w['errors']+w['abandoned'],p
 if 'baseline-main' in str(p) or 'fixed-L5_7' in str(p):
  assert c['errors']==w['errors']==c['abandoned']==w['abandoned']==0,p
  assert c['submitted']==c['completed']==c['target_stats']['received'],p
 rows.append({'raw':str(p.relative_to(job)),'policy_samples':len(policies),'thread_masks_per_phase':{phase:len(values) for phase,values in masks.items()},'all_SCHED_OTHER':True,'all_source_masks_0_5':True,'all_target_masks_8_13':True,'C2_C3_off_at_start':True,'source_terminal_counts_match':True,'target_accounting_valid':True,'active_errors':c['errors'],'warmup_errors':w['errors']})
(job/'raw/verification.json').write_text(json.dumps(rows,indent=2)+'\n')
print(f'validated {len(rows)} complete runs')
