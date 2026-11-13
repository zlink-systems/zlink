#!/usr/bin/env python3
import json, pathlib, statistics, subprocess
job=pathlib.Path('/root/jobs/fx/cx/v21/s033/lad-node-m2')
base=job/'raw/session'
variants=('binding','L0','L0R','L1')
impl={'binding':'zlink-node','L0':'zlink-framework-node','L0R':'zlink-framework-node','L1':'zlink-framework-node'}
patterns=('send-saturation','request-serial','request-backpressure')
payloads=(1024,4096)
def load(v,pattern,payload):
  p=base/v/f'run1/{impl[v]}-{pattern}-{payload}/results.json'
  if not p.exists(): raise SystemExit(f'missing {v} {pattern}-{payload}')
  c=json.loads(p.read_text())['cells'][0]
  if c['errors'] or c['server_errors'] or c['target_stats']['errors'] or c['submitted']!=c['completed'] or c['submitted']!=c['target_stats']['received']:
    raise SystemExit(f'broken result: {p}')
  return {'runs':[{'throughput':c['throughput_per_second'],'path':p.relative_to(job).as_posix(),'errors':c['errors'],'submitted':c['submitted'],'completed':c['completed'],'received':c['target_stats']['received']}],'run_count':1,'median':c['throughput_per_second'],'spread':None,'g5':'not-applicable-1-run'}

rows=[]
for pattern in patterns:
  for payload in payloads:
    cells={v:load(v,pattern,payload) for v in variants}
    b,l,r,n=(cells[v]['median'] for v in variants)
    module=1-r/l
    l1_l0r=1-n/r
    l1_l0=1-n/l
    rows.append({'pattern':pattern,'payload':payload,'cells':cells,'l0_binding_ratio':l/b,'module_cost_l0r_vs_l0':module,'l1_loss_l1_vs_l0r':l1_l0r,'l1_loss_l1_vs_l0':l1_l0,'l1_binding_ratio':n/b,'l0_verdict':'통과' if l/b>=.95 else '미달','l1_verdict':'통과' if l1_l0<=.07 else '미달','confirm_pairs':[p for p,x in (('L0-L0R',module),('L0R-L1',l1_l0r),('L0-L1',l1_l0)) if .05<=x<=.09]})
result={'built_commit':subprocess.check_output(['git','-C','/root/worktree/zlink-v21-s033/lad-node','rev-parse','HEAD'],text=True).strip(),'rows':rows}
out=job/'raw/aggregates/session.json'; out.parent.mkdir(parents=True,exist_ok=True); out.write_text(json.dumps(result,indent=2,ensure_ascii=False)+'\n')
print(out)
for x in rows:
  print(x['pattern'],x['payload'],f"L0/b={x['l0_binding_ratio']:.4f}",f"L0R-loss={x['module_cost_l0r_vs_l0']:.4%}",f"L1/L0R-loss={x['l1_loss_l1_vs_l0r']:.4%}",f"L1/L0-loss={x['l1_loss_l1_vs_l0']:.4%}",f"confirm={','.join(x['confirm_pairs']) or '-'}")
