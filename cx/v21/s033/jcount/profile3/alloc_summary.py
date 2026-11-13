import datetime,glob,json,time,collections
root='/root/jobs/fx/cx/v21/s033/jcount/profile3'; off=time.time_ns()-time.monotonic_ns()
for build in ('framework','binding'):
 p=glob.glob(f'{root}/{build}/run1/*')[0]
 c=json.load(open(p+'/results.json'))['cells'][0]; req=c['completed']; lo=(c['active_started_at_ns']+off)/1e9; hi=(c['active_ended_at_ns']+off)/1e9
 print(f'[{build}] active_completed={req}, throughput={c["throughput_per_second"]:.1f}')
 for role in ('source','target'):
  evs=json.load(open(f'{root}/parsed/{build}-{role}.json'))['recording']['events']
  by=collections.defaultdict(list)
  for e in evs:
   v=e['values']; t=datetime.datetime.fromisoformat(v['startTime']).timestamp()
   th=v['thread']; key=(th.get('javaName',''),th.get('osThreadId',0))
   by[key].append((t,int(v['allocated'])))
  allocated=0; per=[]
  for key,rows in by.items():
   rows.sort(); before=[x for x in rows if x[0]<=lo]; after=[x for x in rows if x[0]>=hi]
   if not after: continue
   baseline=before[-1][1] if before else 0
   delta=after[0][1]-baseline
   if delta>0: allocated+=delta; per.append((delta,key))
  print(f' {role}: runtime ThreadAllocationStatistics delta={allocated} B ({allocated/req:.2f} B/req), threads={len(per)}, largest={[(name,tid,b) for b,(name,tid) in sorted(per,reverse=True)[:8]]}')
