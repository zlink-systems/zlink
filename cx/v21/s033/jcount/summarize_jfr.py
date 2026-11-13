import json, os, subprocess, sys
for build in ('framework','binding'):
  d=f'/root/jobs/fx/cx/v21/s033/jcount/profile/{build}/run1/'
  cell=next(x for x in os.listdir(d) if x.endswith('1024'))
  for role in ('source','target'):
    path=f'{d}{cell}/{role}.jfr'
    raw=subprocess.check_output(['jfr','print','--json','--events','jdk.ExecutionSample','--stack-depth','24',path])
    events=json.loads(raw)['recording']['events']
    counts={}
    for ev in events:
      v=ev['values']; th=v.get('sampledThread') or v.get('eventThread') or {}
      name=th.get('javaName','?')
      frames=(v.get('stackTrace') or {}).get('frames',[])
      for f in frames:
        m=f.get('method',{}); t=m.get('type',{})
        cls=t.get('name',''); meth=m.get('name','?')
        if cls.startswith('systems/zlink/framework/'):
          loc=(cls.replace('/','.'),meth,m.get('lineNumber'),m.get('type',{}).get('package',{}).get('name'))
          # Attribute the sample to the first Framework frame on stack.
          counts.setdefault((name,loc),0); counts[(name,loc)]+=1
          break
    print(f'[{build} {role}] samples={len(events)} framework_frame_samples={sum(counts.values())}')
    for (thread,loc),n in sorted(counts.items(),key=lambda x:-x[1])[:25]:
      print(n,thread,*loc[:3])
