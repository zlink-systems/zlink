from pathlib import Path
import subprocess,json,sys
r=Path('/root/jobs/fx/cx/v21/s033/cbclose2');results=[]
for name,expected in [('sizeof-main',0),('sizeof-after',0),('wait-proof-firstpass',1),('wait-proof-after',0)]:
 p=subprocess.run([str(r/name)],capture_output=True,text=True)
 (r/(name+'.log')).write_text(p.stdout+p.stderr)
 results.append({'program':name,'expected_exit':expected,'exit':p.returncode,'output':p.stdout.strip()})
 print(name,'exit='+str(p.returncode),p.stdout.strip(),flush=True)
(r/'extra-results.json').write_text(json.dumps(results,indent=2))
sys.exit(0 if all(x['exit']==x['expected_exit'] for x in results) else 1)
