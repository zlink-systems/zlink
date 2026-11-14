#!/usr/bin/env python3
"""Independent verification of the saved raw measurements and chosen run counts."""
import datetime,hashlib,json,statistics
from pathlib import Path
JOB=Path('/root/jobs/fx/cx/v21/s033/lfx-cpp');RAW=JOB/'raw'
def main():
 summary=json.loads((RAW/'summary.json').read_text());checked=[]
 for path in sorted(RAW.glob('matrix/*/*/run*/*/results.json')):
  assert (path.parent/'validated.json').exists(),f'unvalidated result: {path}'
  r=json.loads(path.read_text());c=r['cells'][0]
  assert hashlib.sha256(path.read_bytes()).hexdigest()==json.loads((path.parent/'validated.json').read_text())['result_sha256']
  assert c['errors']==c['target_stats']['errors']==0
  assert c['submitted']==c['completed']==c['target_stats']['received']
  assert not any(c.get(k) for k in ('contaminated','drain_bound_hit','abandoned','header_validation_failures'))
  assert c['trigger']['warmup']==3000 and c['trigger']['durationMs']==5000
  assert r['metadata']['sourceCpus']=='0-7' and r['metadata']['targetCpus']=='8-15'
  v=path.relative_to(RAW).parts[1]
  assert r['metadata']['ablationLadderLevel']==(1 if v=='L1' else 0)
  assert (path.parent/'reply-parity.md').is_file()
  p=json.loads((path.parent/'binary-provenance.json').read_text());s=json.loads((RAW/'build-provenance.json').read_text())
  assert p['commit']==s['commit'] and p['binaries']==s['binaries'] and not p['dirty']
  checked.append(str(path))
 for c in summary['cells']:
  for name,v in c['variants'].items():
   if not v:continue
   assert len(v['runs']) in (1,3)
   vals=[json.loads(Path(r['path']).read_text())['cells'][0]['throughput_per_second'] for r in v['runs']]
   m=statistics.median(vals);assert m==v['median']
   if len(vals)==1:assert v['g5'] is None and v['spread'] is None
   else:assert v['g5']==(max(abs(x-m)/m for x in vals)<=.10)
  if 'step_loss' in c:
   n=3 if .05<=c['initial_step_loss']<=.09 else 1
   a=[r['msg_s'] for r in c['variants']['L0']['runs'][:n]];b=[r['msg_s'] for r in c['variants']['L1']['runs'][:n]]
   assert len(a)==len(b)==n
   assert abs(c['step_loss']-(1-statistics.median(b)/statistics.median(a)))<1e-12
   valid=n==1 or all(max(abs(x-statistics.median(v))/statistics.median(v) for x in v)<=.10 for v in (a,b))
   assert c['L1_verdict']==('판정 불가' if not valid else '통과' if c['step_loss']<=.07 else '미달')
 value={'utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'audit':'pass','checked_results':checked,'cells':len(summary['cells'])}
 (RAW/'final-audit.json').write_text(json.dumps(value,indent=2)+'\n');print(f'PASS {len(checked)} checked raw results')
if __name__=='__main__':main()
