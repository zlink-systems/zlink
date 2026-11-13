import collections,json,subprocess,sys
from pathlib import Path
base=Path(sys.argv[1]); phases=json.loads((base/'phases.json').read_text())
active=next(p['epochMs'] for p in phases if p['phase']=='active')
java='/usr/lib/jvm/java-25-openjdk-amd64/bin/java'
converter='/root/jobs/fx/cx/v21/s033/l2bp/tools/jfr-converter-4.6.jar'
result={}
for role in ['source','target']:
 result[role]={}
 for kind in ['cpu','alloc','wall']:
  out=base/f'{role}-{kind}.collapsed'
  cmd=['taskset','-c','6-7,14-15',java,'-jar',converter,'--'+kind,'--total','--threads','--simple','--lines',
       '--from',str(int(active)),'--to',str(int(active)+4000),'-o','collapsed',str(base/f'{role}.jfr'),str(out)]
  proc=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  if proc.returncode: raise RuntimeError(proc.stdout)
  self=collections.Counter(); inclusive=collections.Counter(); threads=collections.Counter(); total=0
  envelope=0
  for line in out.read_text().splitlines():
   stack,count=line.rsplit(' ',1); count=int(count); fs=stack.split(';'); total+=count
   threads[fs[0]]+=count; self[fs[-1]]+=count
   for frame in set(fs): inclusive[frame]+=count
   if any('ZLinkChannelEnvelope' in f for f in fs): envelope+=count
  r={'total':total,'envelopePercent':100*envelope/total if total else 0,
     'self':self.most_common(20),'threads':threads.most_common(12),
     'envelope':[(k,v,round(100*v/total,3)) for k,v in inclusive.most_common() if 'ZLinkChannelEnvelope' in k],
     'inclusive':inclusive.most_common(30)}
  result[role][kind]=r
  print(role,kind,'total',total,'envelope%',round(r['envelopePercent'],2))
  print('SELF',[(k,round(100*v/total,2)) for k,v in r['self'][:10]])
  print('ENVELOPE',r['envelope'][:20]); print('THREADS',r['threads'][:8])
(base/'jfr-summary.json').write_text(json.dumps(result,indent=2))
