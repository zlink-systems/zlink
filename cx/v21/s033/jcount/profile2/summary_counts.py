import collections,datetime,glob,json,os,time,re
root='/root/jobs/fx/cx/v21/s033/jcount/profile2'
offset=time.time_ns()-time.monotonic_ns()
for build in ('framework','binding'):
 p=glob.glob(f'{root}/{build}/run1/*')[0]
 r=json.load(open(p+'/results.json'))['cells'][0]
 req=r['completed']; lo=(r['active_started_at_ns']+offset)/1e9; hi=(r['active_ended_at_ns']+offset)/1e9
 print(f'[{build}] active_completed={req} throughput={r["throughput_per_second"]:.1f} active_s={r["active_elapsed_ms"]/1000:.3f}')
 for role in ('source','target'):
  before={x['tid']:x for x in json.load(open(f'{p}/{role}-cpu-before.json'))['threads']}
  after={x['tid']:x for x in json.load(open(f'{p}/{role}-cpu-after.json'))['threads']}
  threads=[]
  for tid,b in before.items():
   a=after.get(tid)
   if a:
    ticks=(a['user_ticks']-b['user_ticks'])+(a['system_ticks']-b['system_ticks'])
    if ticks>0: threads.append((ticks,tid,a['comm']))
  cpu_us=sum(x[0] for x in threads)*10000
  alloc_count=0; alloc_weight=0; monitors=0; monitor_us=0
  evs=json.load(open(f'{root}/parsed/{build}-{role}.json'))['recording']['events']
  for e in evs:
   v=e['values']; ts=datetime.datetime.fromisoformat(v['startTime']).timestamp()
   if not lo<=ts<=hi: continue
   if e['type']=='jdk.ObjectAllocationSample':
    alloc_count+=1; alloc_weight+=int(v.get('weight',0))
   elif e['type']=='jdk.JavaMonitorEnter':
    monitors+=1; monitor_us+=float(re.fullmatch(r'PT([0-9]+(?:\.[0-9]+)?)S',v.get('duration','PT0S')).group(1))*1e6
  perf_path=f'{p}/perf-{role}.csv'
  syscall=None
  for line in open(perf_path):
   if 'raw_syscalls:sys_enter' in line: syscall=int(line.split(',')[0])
  print(f' {role}: CPU={cpu_us/req:.3f} us/req ({cpu_us/1e6:.3f}s total, active+drain, {len(threads)} threads); allocation_samples={alloc_count} ({alloc_count/req:.4f}/req), weighted_bytes={alloc_weight} ({alloc_weight/req:.2f} B/req); JavaMonitorEnter={monitors} ({monitors/req:.6f}/req, wait_us={monitor_us:.0f}); syscalls={syscall} ({syscall/req:.4f}/req)')
  print('  top_cpu_threads:',[(comm,tid,ticks) for ticks,tid,comm in sorted(threads,reverse=True)[:8]])
