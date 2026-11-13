from pathlib import Path
import json,math,re
job=Path('/root/jobs/fx/cx/v21/s033/cthin');a=json.loads((job/'analysis.json').read_text());out={}
for cell,row in a.items():
 p=Path(row['directory']);cap=p/'capture';result=json.loads((p/'results.json').read_text())['cells'][0]
 assert result['errors']==result['abandoned']==result['header_validation_failures']==0,(cell,result)
 assert not result.get('contaminated',False) and not result.get('drain_bound_hit',False)
 assert result['completed']==result['target_stats']['received'] and result['target_stats']['errors']==0
 before=json.loads((cap/'before.json').read_text());after=json.loads((cap/'after.json').read_text())
 timeline=json.loads((cap/'samples.json').read_text()) if (cap/'samples.json').exists() else [before,after]
 o={'valid_counts':True,'roles':{},'profile_phase_rate':result['throughput_per_second'],'completed':result['completed']}
 for role,rr in row['roles'].items():
  samples=json.loads((cap/f'{role}.samples.json').read_text());assert samples,(cell,role)
  assert math.isclose(sum(rr['stage_us'].values()),rr['cpu_us'],abs_tol=1e-8)
  for tid,t in rr['threads'].items():assert math.isclose(sum(t['stage_us'].values()),t['cpu_us'],abs_tol=1e-8)
  allowed=set(range(8)) if role=='source' else set(range(8,16))
  for snap in timeline:
   for tid,t in snap['processes'][role]['threads'].items():assert set(t['affinity'])<=allowed and t['affinity'],(cell,role,tid,t['affinity'])
  empty=sum(not x['frames'] for x in samples);unknown=sum(not x['frames'] or x['frames'][0][0]=='[unknown]' for x in samples)
  unresolved_all=sum(not x['frames'] or all(s=='[unknown]' for s,d in x['frames']) for x in samples)
  err=(cap/f'{role}.perf.stderr').read_text()
  no_lost_report=not re.search(r'\b(lost|losing)\b',err,re.I)
  all_tids=set().union(*(set(x['processes'][role]['threads']) for x in timeline))
  retired=all_tids-set(after['processes'][role]['threads'])
  assert retired<=set(rr['threads']),(cell,role,'unaccounted retired',retired-set(rr['threads']))
  o['roles'][role]={'timeline_snapshots':len(timeline),'max_snapshot_gap_seconds':max(y['monotonic']-x['monotonic'] for x,y in zip(timeline,timeline[1:])),'retired_tids_accounted':sorted(retired),'retired_cpu_us_counted':sum(rr['threads'][tid]['cpu_us'] for tid in retired),'samples':len(samples),'empty_stacks':empty,'unknown_leaves':unknown,'wholly_unknown_stacks':unresolved_all,'no_lost_report':no_lost_report,'retired_tids':sorted(set(before['processes'][role]['threads'])-set(after['processes'][role]['threads'])),'bytes':(cap/f'{role}.perf.data').stat().st_size,'affinity_valid':True,'cpu_stage_sum_valid':True}
 out[cell]=o
(job/'quality.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
