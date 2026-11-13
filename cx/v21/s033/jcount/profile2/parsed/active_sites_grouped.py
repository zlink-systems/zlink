import collections,json,glob,time,datetime
base='/root/jobs/fx/cx/v21/s033/jcount/profile2/parsed'; off=time.time_ns()-time.monotonic_ns()
for role in ('source','target'):
 cs={}; totals={}
 for build in ('framework','binding'):
  rp=glob.glob(f'/root/jobs/fx/cx/v21/s033/jcount/profile2/{build}/run1/*/results.json')[0]
  c=json.load(open(rp))['cells'][0]; lo=(c['active_started_at_ns']+off)/1e9; hi=(c['active_ended_at_ns']+off)/1e9
  cnt=collections.Counter(); lines=collections.Counter(); events=json.load(open(f'{base}/{build}-{role}.json'))['recording']['events']
  for e in events:
   if e['type']!='jdk.ExecutionSample': continue
   t=datetime.datetime.fromisoformat(e['values']['startTime']).timestamp()
   if not lo<=t<=hi: continue
   f=next((f for f in (e['values'].get('stackTrace') or {}).get('frames',[]) if f['method']['type']['name'].startswith('systems/zlink/')),None)
   if not f: continue
   m=f['method']; cls=m['type']['name'].replace('/','.'); name=m['name'];key=(cls,name)
   cnt[key]+=1;lines[(key,f.get('lineNumber','?'))]+=1
  totals[build]=len([e for e in events if e['type']=='jdk.ExecutionSample' and lo<=datetime.datetime.fromisoformat(e['values']['startTime']).timestamp()<=hi])
  cs[build]=(cnt,lines)
 delta={k:cs['framework'][0][k]-cs['binding'][0][k] for k in set(cs['framework'][0])|set(cs['binding'][0])}
 positive=[(k,n) for k,n in delta.items() if n>0]; target=int((totals['framework']-totals['binding'])*.8); acc=0
 print(f'[{role}] fw_samples={totals["framework"]}; binding_samples={totals["binding"]}; delta={totals["framework"]-totals["binding"]}; 80% threshold={target}')
 for key,n in sorted(positive,key=lambda x:-x[1]):
  acc+=n
  best=max((v,line) for (k,line),v in cs['framework'][1].items() if k==key)[1]
  cls,fn=key
  print(f'{n} cum={acc} {cls}.{fn}:{best} ({"Framework-only" if ".framework." in cls else "shared binding"})')
  if acc>=target:break
