import collections,datetime,glob,json,time
root='/root/jobs/fx/cx/v21/s033/jcount/profile2';off=time.time_ns()-time.monotonic_ns()
def bucket(build,role,name):
 if role=='source':
  if name=='bench-phase-active':return 'submit thread'
  if name.startswith('zlink-jvm-raw'):return 'mesh receive thread'
  if name.startswith('ForkJoinPool.commonPool-worker'):return 'completion workers'
 else:
  if not name:return 'unnamed JVM thread'
  if name.startswith('zlink-jvm-raw'):return 'mesh receive thread'
  if name.startswith('ForkJoinPool-1-worker'):return 'application workers'
 return 'other threads'
for role in ('source','target'):
 allc={}
 total={}
 for build in ('framework','binding'):
  rp=glob.glob(f'{root}/{build}/run1/*/results.json')[0];c=json.load(open(rp))['cells'][0];lo=(c['active_started_at_ns']+off)/1e9;hi=(c['active_ended_at_ns']+off)/1e9
  cnt=collections.Counter();n=0
  for e in json.load(open(f'{root}/exec/{build}-{role}.json'))['recording']['events']:
   v=e['values'];t=datetime.datetime.fromisoformat(v['startTime']).timestamp()
   if not lo<=t<=hi:continue
   n+=1;th=v.get('sampledThread') or {};name=th.get('javaName','')
   frame=next((f for f in (v.get('stackTrace') or {}).get('frames',[]) if f['method']['type']['name'].startswith('systems/zlink/')),None)
   if not frame:continue
   m=frame['method'];cls=m['type']['name'].replace('/','.');fn=m['name'];line=frame.get('lineNumber','?')
   cnt[(bucket(build,role,name),(cls,fn,line))]+=1
  allc[build]=cnt;total[build]=n
 delta={k:allc['framework'][k]-allc['binding'][k] for k in set(allc['framework'])|set(allc['binding'])};positive=[(k,n) for k,n in delta.items() if n>0]
 threshold=int((total['framework']-total['binding'])*.8);acc=0
 print(f'[{role}] FW samples {total["framework"]}; binding {total["binding"]}; delta {total["framework"]-total["binding"]}; 80% {threshold}')
 for (b,(cls,fn,line)),n in sorted(positive,key=lambda x:-x[1]):
  acc+=n;print(f'{n} {acc} {b} {cls}.{fn}:{line} {"Framework-only" if ".framework." in cls else "shared binding" if (".runtime." in cls or ".contracts." in cls) else "benchmark"}')
  if acc>=threshold:break
