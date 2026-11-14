import collections, json, subprocess, sys, xml.etree.ElementTree as ET
from pathlib import Path
job=Path('/root/jobs/fx/cx/v21/s033/j2l2')
base=job/'raw'/sys.argv[1]
phases=json.loads((base/'phases.json').read_text())
warm,active=phases
result={}
rows=[json.loads(line) for line in (base/'stats.jsonl').read_text().splitlines()]

def counter_at(role, field, phase, at):
 points=[]
 for row in rows:
  if row['phaseRequested'] != phase: continue
  data=row if role=='source' else row.get('targetStats')
  if data is not None: points.append((row['epochMs'],data[field]))
 if not points: return None
 before=[p for p in points if p[0]<=at]; after=[p for p in points if p[0]>=at]
 if not before: return points[0][1]
 if not after: return points[-1][1]
 lo,hi=before[-1],after[0]
 if lo[0]==hi[0]: return lo[1]
 return lo[1]+(hi[1]-lo[1])*(at-lo[0])/(hi[0]-lo[0])

for window,begin,end in [('cold',warm['epochMs'],warm['epochMs']+2000),('steady',active['epochMs'],active['epochMs']+5000)]:
 result[window]={}
 for role in ['source','target']:
  counts=json.loads((base/f'{window if window=="cold" or "steadyEndMs" in active else "active"}-{role}-stats.json').read_text())
  field='submitted' if role=='source' else 'allPhaseMessages'
  interpolated=counter_at(role,field,'warmup' if window=='cold' else 'active',end)
  n=interpolated if interpolated is not None else counts[field]
  r={'messages':n,'denominator':'interpolated boundary counter' if interpolated is not None else 'snapshot boundary counter','submitted':counts['submitted'],'completed':counts.get('completed'), 'errors':counts['errors'],'windowMs':end-begin}
  result[window][role]=r
  for kind in ['cpu','alloc']:
   dest=base/f'{role}-{window}-{kind}.collapsed'
   subprocess.run(['taskset','-c','6-7,14-15','java','-jar',str(job/'tools/jfr-converter-4.6.jar'),
       '--'+kind,'--total','--threads','--simple','--lines','--from',str(int(begin)),'--to',str(int(end)),
       '-o','collapsed',str(base/f'{role}.jfr'),str(dest)],check=True,stdout=subprocess.DEVNULL)
   incl=collections.Counter(); self=collections.Counter(); total=0; env=0; categories=collections.Counter()
   for line in dest.read_text().splitlines():
    stack,count=line.rsplit(' ',1); count=int(count); frames=stack.split(';'); total+=count; self[frames[-1]]+=count
    for f in set(frames): incl[f]+=count
    isenv=any('ZLinkChannelEnvelope' in f or 'ZLinkServiceOperationIds' in f for f in frames)
    if isenv:
     env+=count
     key='other'
     if any('correlationId' in f and 'ZLinkServiceOperationIds' in f for f in frames): key='correlation'
     elif any('CanonicalReader.string' in f for f in frames): key='strings'
     elif any('HeaderReader' in f or 'decodeHeader' in f and 'Envelope' in f for f in frames): key='parse'
     elif any('encodeHeaderBytesUncached' in f for f in frames): key='encode'
     categories[key]+=count
   r[kind]={'total':total,'envelope':env,'envelopePerMessage':env/n if n else None,
     'envelopePercent':100*env/total if total else 0,'categories':dict(categories),'self':self.most_common(20),
     'envelopeFrames':[(k,v) for k,v in incl.most_common() if 'ZLinkChannelEnvelope' in k or 'ZLinkServiceOperationIds' in k]}
  lifecycle=json.loads((base/'lifecycle.json').read_text())
  born=next(x['bornEpochMs'] for x in lifecycle['processes'] if x['role']==role)
  compilation=[]
  try:
   root=ET.parse(base/f'{role}-compilation.xml').getroot()
   for node in root.findall('.//nmethod'):
    method=node.get('method','')
    if 'ZLinkChannelEnvelope' in method or 'ZLinkServiceOperationIds' in method:
     compilation.append({'method':method,'tier':node.get('level','4'),'compiler':node.get('compiler'),
       'secondsAfterWorkloadStart':round(float(node.get('stamp'))-(warm['epochMs']-born)/1000,4)})
  except ET.ParseError as e: compilation=[{'parseError':str(e)}]
  r['compilation']=compilation
  print(window,role,'messages',n,'envelope CPU%',round(r['cpu']['envelopePercent'],2),
      'sampled B/msg',round(r['alloc']['envelopePerMessage'] or 0,2))
audit={}
for phase in ['warmup','active']:
 s=json.loads((base/f'{phase}-source-stats.json').read_text()); t=json.loads((base/f'{phase}-target-stats.json').read_text())
 received=t['allPhaseMessages']
 audit[phase]={'submitted':s['submitted'],'completed':s['completed'],'received':received,'sourceErrors':s['errors'],'targetErrors':t['errors'],
     'valid':s['submitted']==s['completed']==received and s['errors']==t['errors']==0}
result['audit']=audit
(base/'summary.json').write_text(json.dumps(result,indent=2))
print('audit',audit)
