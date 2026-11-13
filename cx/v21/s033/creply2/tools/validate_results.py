from pathlib import Path
import json,copy
p=Path('/root/jobs/fx/cx/v21/s033/creply2')
def check(r):
 c=r['cells'][0];assert c['submitted']==c['completed']==c['target_stats']['received'],('counts',c['submitted'],c['completed'],c['target_stats']['received']);assert c['errors']==c['abandoned']==c['target_stats']['errors']==0
valid=[]
for f in sorted((p/'runs').glob('*/results.json')):
 if 'preflight' in str(f):continue
 r=json.load(open(f));check(r);valid.append(f.parent.name)
r=json.load(open(p/'runs/main-unpaced-1/results.json'));caught=[]
for fault in ('count','error','abandoned'):
 bad=copy.deepcopy(r);c=bad['cells'][0]
 if fault=='count':c['target_stats']['received']-=1
 elif fault=='error':c['errors']=1
 else:c['abandoned']=1
 try:check(bad)
 except AssertionError:caught.append(fault)
 else:raise AssertionError(('validator missed',fault))
mins=[json.load(open(f))['min_memavailable_kb'] for f in (p/'runs').glob('*/runner-meta.json')]
assert min(mins)>=12582912
out={'valid_runs':valid,'valid_run_count':len(valid),'negative_fixtures_caught':caught,'min_memavailable_kb':min(mins)};(p/'validation.json').write_text(json.dumps(out,indent=2));print(json.dumps(out,indent=2))
