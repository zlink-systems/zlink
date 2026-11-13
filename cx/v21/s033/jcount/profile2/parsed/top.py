import collections,json,os
base='/root/jobs/fx/cx/v21/s033/jcount/profile2/parsed'
for role in ('source','target'):
  files={k:json.load(open(f'{base}/{k}-{role}.json'))['recording']['events'] for k in ('framework','binding')}
  print(f'[{role}] fw samples={len(files["framework"])} binding={len(files["binding"])} delta={len(files["framework"])-len(files["binding"])}')
  for label,events in files.items():
    c=collections.Counter(); byrole=collections.Counter()
    for ev in events:
      v=ev['values']; th=v.get('sampledThread') or {}
      tn=th.get('javaName','?')
      frames=(v.get('stackTrace') or {}).get('frames',[])
      frame=next((f for f in frames if f['method']['type']['name'].startswith('systems/zlink/framework/')),None)
      if frame:
        m=frame['method']; cls=m['type']['name'].replace('/','.')
        key=(cls,m['name'],frame.get('lineNumber','?'))
        c[key]+=1; byrole[tn]+=1
    if label=='framework':
      print('  framework frame count',sum(c.values()), 'thread roles',byrole.most_common(8))
      total=sum(c.values()); accum=0
      for (cls,m,line),n in c.most_common():
        accum+=n
        print(f'  {n:4d} {accum/total:5.1%} {cls}.{m}:{line}')
        if accum>=total*.8: break
