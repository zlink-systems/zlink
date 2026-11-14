#!/usr/bin/env python3
"""Exercise the validator with valid and intentionally invalid counts."""
import ast,copy,json,subprocess,sys
from pathlib import Path
JOB=Path('/root/jobs/fx/cx/v21/s033/lfx-cpp')
for p in (JOB/'tools').glob('*.py'):ast.parse(p.read_text(),filename=str(p))
for p in (JOB/'tools').rglob('*'):
 if p.is_file() and (p.suffix=='.sh' or p.name=='curl'):subprocess.run(['bash','-n',str(p)],check=True)
c={'implementation':'fixture','pattern':'fixture','payload_size':1024,'throughput_per_second':1,
 'submitted':1,'completed':1,'errors':0,'target_stats':{'received':1,'errors':0},'trigger':{'durationMs':5000,'warmup':3000}}
for kind in ('valid','bad-count','bad-warmup','bad-errors'):
 value={'cells':[copy.deepcopy(c)]};cell=value['cells'][0]
 if kind=='bad-count':cell['completed']=0
 if kind=='bad-warmup':cell['trigger']['warmup']=30000
 if kind=='bad-errors':cell['target_stats']['errors']=1
 p=JOB/'raw/tool-validation'/f'{kind}.json';p.parent.mkdir(parents=True,exist_ok=True);p.write_text(json.dumps(value)+'\n')
 r=subprocess.run([sys.executable,str(JOB/'tools/check-result.py'),str(p),'--verify-only'],capture_output=True,text=True)
 (p.with_suffix('.log')).write_text(r.stdout+r.stderr)
 assert (r.returncode==0)==(kind=='valid'),kind
 print(kind,'ACCEPT' if r.returncode==0 else 'REJECT')
print('PASS: Python syntax, shell syntax, positive and three negative checks')
