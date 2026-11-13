from pathlib import Path
import json
from wire import parse
J=Path('/root/jobs/fx/cx/v21/s033/coreio');result=[]
for cap in J.glob('out/*/run1/*/capture-strace'):
 for role in ('source','target'):
  path=cap/(role+'.strace');cache=cap/(role+'.wire.json')
  if not path.exists():continue
  if cache.exists():r=json.load(open(cache))
  else:r=parse(path);cache.write_text(json.dumps(r,indent=2))
  result.append({'capture':str(cap),'role':role,'streams':r['streams'],'syscalls':r['syscalls']})
  print(cap.parents[2].name,role,[(s['fd'],s['direction'],s['records'],s['record_hist'][:2]) for s in r['streams'] if any(x['wire_bytes']>1000 for x in s['record_hist'])])
(J/'wire-summary.json').write_text(json.dumps(result,indent=2))
