from pathlib import Path
import json,subprocess
job=Path('/root/jobs/fx/cx/v21/s033/cthin');checks=[]
for pattern in ('request-backpressure','send-saturation'):
 for impl in ('zlink-cpp','zlink-framework-cpp'):
  p=job/'none/run1'/f'{impl}-{pattern}-1024/results.json'
  original=json.loads(p.read_text());bad=json.loads(p.read_text());bad['cells'][0]['target_stats']['received']+=1
  neg=job/f'negative-{impl}-{pattern}.json';neg.write_text(json.dumps(bad))
  for name,file,expected in [('positive',p,True),('negative',neg,False)]:
   r=subprocess.run(['bash','-c','source "$1"; verify_request_counts "$2"','validator',str(job/'runner-common.sh'),str(file)],text=True,capture_output=True)
   passed=(r.returncode==0)==expected
   checks.append({'case':name,'path':str(file),'expected_acceptance':expected,'exit_code':r.returncode,'check_passed':passed,'output':(r.stdout+r.stderr).strip()})
   assert passed,checks[-1]
(job/'count-validator-check.json').write_text(json.dumps(checks,indent=2));print('Count validator:',len(checks),'positive/negative checks passed')
