import collections,json,sys
from pathlib import Path
base=Path(sys.argv[1])
phases=json.loads((base/'phases.json').read_text())
active=next(p['monotonicUs'] for p in phases if p['phase']=='active')
result={}
for file in base.glob('*.cpuprofile'):
    p=json.loads(file.read_text())
    pid=file.name.split('.')[3]
    event=json.loads((base/f'event-loop-{pid}.json').read_text())
    role='source' if '/client/main.js' in event['argv'][1] else 'target'
    nodes={n['id']:n for n in p['nodes']}
    parent={c:n['id'] for n in p['nodes'] for c in n.get('children',[])}
    def name(n):
        f=n['callFrame']; return f"{f['functionName']} ({f['url']}:{f['lineNumber']+1})"
    self=collections.Counter(); inclusive=collections.Counter(); envelope=0; total=0
    clock=p['startTime']
    for id,dt in zip(p['samples'],p['timeDeltas']):
        clock+=dt
        if clock<active or clock>active+5000000: continue
        total+=dt; self[name(nodes[id])]+=dt
        found=False
        while id in nodes:
            label=name(nodes[id]); inclusive[label]+=dt
            if '/channel-envelope.js:' in label: found=True
            id=parent.get(id)
        if found: envelope+=dt
    loop=[s for s in event['samples'] if s['time']>=next(p['epochMs'] for p in phases if p['phase']=='active')]
    result[role]={'totalSampleUs':total,'envelopePercent':100*envelope/total if total else 0,
      'self':[(k,round(100*v/total,3)) for k,v in self.most_common(35)],
      'envelopeInclusive':[(k,round(100*v/total,3)) for k,v in inclusive.most_common() if '/channel-envelope.js:' in k],
      'eventLoop':loop}
(base/'cpu-summary.json').write_text(json.dumps(result,indent=2))
for role,r in result.items():
 print(role,'envelope%',round(r['envelopePercent'],2))
 print('SELF',*r['self'][:16],sep='\n')
 print('ENVELOPE',*r['envelopeInclusive'][:20],sep='\n')
 print('LOOP', [(round(s['delayP99Ms'],3),round(s['delayMaxMs'],3),round(s['utilization'],3)) for s in r['eventLoop']])
