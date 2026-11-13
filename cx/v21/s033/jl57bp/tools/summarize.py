import json,sys
from pathlib import Path
out=Path(sys.argv[1]); p=next(out.rglob('results.json')); c=json.loads(p.read_text())['cells'][0]
w=json.loads((p.parent/'results.warmup.json').read_text())['cells'][0]
s=next(out.rglob('scheduling.txt')).read_text()
assert 'scheduling_invalid=' not in s
for phase,cpus in [('source_start','0-5'),('target_start','8-13'),('mid_active_source','0-5'),('mid_active_target','8-13')]:
 lines=[x for x in s.splitlines() if x.startswith(phase+'_task_affinity')]
 assert lines and all(x.endswith('cpus='+cpus) for x in lines),(phase,lines)
row={'warmup':{k:w.get(k) for k in ['submitted','completed','errors','client_error_summary','peak_in_flight','abandoned']}}
row.update({k:c.get(k) for k in ['submitted','completed','errors','client_error_summary','peak_in_flight','abandoned','drain_ms','drain_bound_hit','target_stats']})
with Path('/root/jobs/fx/cx/v21/s033/jl57bp/progress.md').open('a') as f: f.write('- 결과 '+str(p)+': '+json.dumps(row,ensure_ascii=False)+'\n')
print(json.dumps({'raw':str(p),**row},ensure_ascii=False))
