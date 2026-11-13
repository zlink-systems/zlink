#!/usr/bin/env python3
import json, pathlib, statistics, sys
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/lad-node-l1')
base=job/'raw/session'
variants=('binding','L0','L0R','L1')
impl={'binding':'zlink-node','L0':'zlink-framework-node','L0R':'zlink-framework-node','L1':'zlink-framework-node'}
patterns=('send-saturation','request-serial','request-backpressure')
payloads=(1024,4096)
def load(variant,pattern,payload):
  paths=[]
  for phase,count in (('first3',3),('repeat2',2)):
    directory=base/phase/variant
    if not directory.exists(): continue
    for run in range(1,count+1):
      path=directory/f'run{run}'/f'{impl[variant]}-{pattern}-{payload}'/'results.json'
      if path.exists(): paths.append(path)
  vals=[]
  for path in paths:
    cell=json.loads(path.read_text())['cells'][0]
    if cell['errors'] or cell['server_errors'] or cell['target_stats']['errors'] or cell['submitted']!=cell['completed'] or cell['submitted']!=cell['target_stats']['received']:
      raise SystemExit(f'broken result: {path}')
    vals.append({'throughput':cell['throughput_per_second'],'path':path.relative_to(job).as_posix(),'errors':cell['errors'],'submitted':cell['submitted'],'completed':cell['completed'],'received':cell['target_stats']['received']})
  if len(vals) not in (3,5): raise SystemExit(f'expected 3 or 5 valid runs: {variant} {pattern}-{payload}, got {len(vals)}')
  speeds=[v['throughput'] for v in vals]
  median=statistics.median(speeds); spread=max(abs(v-median) for v in speeds)/median
  return {'runs':vals,'run_count':len(vals),'median':median,'spread':spread,'g5':'pass' if spread<=.10 else ('판정 불가' if len(vals)==5 else 'repeat-required')}
rows=[]
for pattern in patterns:
  for payload in payloads:
    cells={v:load(v,pattern,payload) for v in variants}
    b,l,r,n=(cells[v]['median'] for v in variants)
    l0ratio=l/b
    step_l1=1-n/l
    step_l0r=1-n/r
    module_cost=1-r/l
    l0_verdict='통과' if l0ratio>=.95 and cells['binding']['g5']=='pass' and cells['L0']['g5']=='pass' else ('미달' if l0ratio<.95 else '판정 불가')
    l1_verdict='통과' if step_l1<=.07 and all(cells[v]['g5']=='pass' for v in ('binding','L0','L1')) else ('미달' if step_l1>.07 else '판정 불가')
    rows.append({'pattern':pattern,'payload':payload,'cells':cells,'l0_binding_ratio':l0ratio,'module_cost_l0r_vs_l0':module_cost,'l1_loss_l1_vs_l0r':step_l0r,'l1_loss_l1_vs_l0':step_l1,'l1_binding_ratio':n/b,'l0_verdict':l0_verdict,'l1_verdict':l1_verdict})
result={'built_commit':__import__('subprocess').check_output(['git','-C','/root/worktree/zlink-v21-s033/lad-node','rev-parse','HEAD'],text=True).strip(),'rows':rows}
out=job/'raw/aggregates/session.json'; out.parent.mkdir(parents=True,exist_ok=True); out.write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n')
print(out)
for row in rows:
  print(row['pattern'],row['payload'],f"L0/b={row['l0_binding_ratio']:.4f}",f"L0R-loss={row['module_cost_l0r_vs_l0']:.4%}",f"L1/L0R-loss={row['l1_loss_l1_vs_l0r']:.4%}",f"L1/L0-loss={row['l1_loss_l1_vs_l0']:.4%}",f"G5={','.join(row['cells'][v]['g5'] for v in variants)}",f"verdict={row['l1_verdict']}")
