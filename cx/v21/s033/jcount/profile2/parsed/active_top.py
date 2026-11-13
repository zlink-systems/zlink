import collections,json,glob,os,time,datetime
base='/root/jobs/fx/cx/v21/s033/jcount/profile2/parsed'
offset=time.time_ns()-time.monotonic_ns()
for role in ('source','target'):
  counts={}
  totals={}
  fw_p=glob.glob(f'/root/jobs/fx/cx/v21/s033/jcount/profile2/framework/run1/*/results.json')[0]
  for build in ('framework','binding'):
    results=glob.glob(f'/root/jobs/fx/cx/v21/s033/jcount/profile2/{build}/run1/*/results.json')[0]
    c=json.load(open(results))['cells'][0]
    lo=(c['active_started_at_ns']+offset)/1e9; hi=(c['active_ended_at_ns']+offset)/1e9
    path=f'{base}/{build}-{role}.json'
    events=json.load(open(path))['recording']['events']
    chosen=[]
    for e in events:
      v=e['values']; t=datetime.datetime.fromisoformat(v['startTime']).timestamp()
      if lo<=t<=hi: chosen.append(e)
    totals[build]=len(chosen)
    counter=collections.Counter()
    for e in chosen:
      frames=(e['values'].get('stackTrace') or {}).get('frames',[])
      f=next((f for f in frames if f['method']['type']['name'].startswith('systems/zlink/')),None)
      if not f: continue
      m=f['method']; cls=m['type']['name'].replace('/','.'); name=m['name']; line=f.get('lineNumber','?')
      counter[(cls,name,line)]+=1
    counts[build]=counter
  print(f'[{role}] active execution samples fw={totals["framework"]} binding={totals["binding"]} delta={totals["framework"]-totals["binding"]}')
  delta={k:counts['framework'][k]-counts['binding'][k] for k in set(counts['framework'])|set(counts['binding'])}
  positive=[(k,v) for k,v in delta.items() if v>0]
  pos_total=sum(v for _,v in positive)
  need=max(1,int((totals['framework']-totals['binding'])*.8))
  print(f'positive first-frame site delta={pos_total}; target 80% total sample delta={need}')
  accum=0
  for (cls,m,line),n in sorted(positive,key=lambda x:-x[1]):
    accum+=n
    print(f'{n:4d} cum={accum:4d} {cls}.{m}:{line} [{"Framework-only" if ".framework." in cls else "shared binding"}]')
    if accum>=need: break
  print('top FW first frames:')
  for (cls,m,line),n in counts['framework'].most_common(15):print(f' {n:3d} {cls}.{m}:{line} [{"Framework-only" if ".framework." in cls else "shared binding"}]')
