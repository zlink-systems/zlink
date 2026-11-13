import json,re,bisect,collections,statistics
base='/root/jobs/fx/cx/v21/s033/dseg2/';pids=json.load(open(base+'capture.json'))['pids'];tp=pids['target']
a_win=1994613.354918868;z_win=1994616.387760548
B={s:{} for s in (2,3)}
for l in open(f"{base}trace.{tp}.jsonl"):
 r=json.loads(l)
 if r['Flow']=='B' and r['Stage'] in B:
  r['t']=r['Timestamp']/1e9
  if a_win<=r['t']<=z_win:B[r['Stage']][r['Serial']]=r
ent=re.compile(r' ([0-9.]+): raw_syscalls:sys_enter: NR (\d+) \(([^)]*)\)');ex=re.compile(r' ([0-9.]+): +raw_syscalls:sys_exit: NR (\d+) = (-?\d+)')
pending={};calls=collections.defaultdict(list)
for l in open(base+'longrun.syscalls.txt'):
 tid=int(re.search(r'\d+/(\d+) \[',l).group(1));m=ent.search(l);x=ex.search(l)
 if m:pending[tid]=(float(m.group(1)),int(m.group(2)),m.group(3))
 elif x:
  t,n,ret=float(x.group(1)),int(x.group(2)),int(x.group(3));q=pending.pop(tid,None)
  if q and q[1]==n:calls[tid].append((q[0],t,n,ret,q[2]))
for tid in calls:calls[tid].sort()
starts={t:[x[0] for x in xs] for t,xs in calls.items()};mp={16:3087303,17:3087304};ic=collections.Counter();counts=collections.Counter();seq=collections.Counter();duration=collections.defaultdict(list)
for s,b2 in B[2].items():
 b3=B[3].get(s)
 if not b3 or b2['ThreadId']!=b3['ThreadId']:continue
 tid=mp.get(b2['ThreadId']);xs=calls.get(tid,[]);t=starts.get(tid,[]);i=bisect.bisect_left(t,b2['t']);j=bisect.bisect_right(t,b3['t']);hit=[x for x in xs[i:j] if x[1]<=b3['t']]
 ic[len(hit)]+=1;seq[(b2['ThreadId'],tuple((x[2],x[3]) for x in hit))]+=1
 for x in hit:counts[(x[2],x[3])]+=1;duration[(x[2],x[3])].append((x[1]-x[0])*1e6)
print('active_intervals',sum(ic.values()),'calls_per_interval',ic,'syscall_seq',seq.most_common(15),'syscall_counts_nr_ret',counts)
print('syscall_duration_us',{k:{'n':len(v),'p50':round(statistics.median(v),3),'p90':round(sorted(v)[int(.9*(len(v)-1))],3),'mean':round(statistics.fmean(v),3)} for k,v in duration.items()})
