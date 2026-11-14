import json,sys
from pathlib import Path
base=Path(sys.argv[1]); result={}
for file in base.glob('heap-*.json'):
 p=json.loads(file.read_text());pid=file.stem.split('-')[1]
 event=json.loads((base/('event-loop-'+pid+'.json')).read_text());role='source' if '/client/main.js' in event['argv'][1] else 'target'
 calls=sum(json.loads((base/(phase+'-source-stats.json')).read_text())['submitted'] for phase in ['warmup','active'])
 rows=[];total=0
 def walk(n,inside=False):
  global total
  frame=n['callFrame'];own=inside or frame['url'].endswith('/channel-envelope.js')
  size=n['selfSize'];total+=size
  if own and size: rows.append(dict(name=frame['functionName'],url=frame['url'],line=frame['lineNumber']+1,bytes=size))
  for c in n.get('children',[]):walk(c,own)
 walk(p['head']);alloc=sum(r['bytes'] for r in rows)
 result[role]=dict(callsWarmupAndActive=calls,estimatedEnvelopeBytes=alloc,estimatedBytesPerMessage=alloc/calls,wholeProcessSampleBytes=total,rows=rows)
(base/'heap-summary.json').write_text(json.dumps(result,indent=2))
for role,v in result.items():print(role,'sampled inclusive codec B/msg',round(v['estimatedBytesPerMessage'],2),'calls',v['callsWarmupAndActive'])
