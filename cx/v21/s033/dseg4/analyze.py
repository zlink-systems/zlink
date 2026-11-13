import bisect, collections, json, pathlib, re, statistics
base=pathlib.Path('/root/jobs/fx/cx/v21/s033/dseg2')
pids=json.loads((base/'capture.json').read_text())['pids']; sp,tp=pids['source'],pids['target']

def load_trace(path, keep):
 out=[]
 with path.open() as f:
  for line in f:
   r=json.loads(line)
   if keep(r): r['t']=r['Timestamp']/1e9;out.append(r)
 return out
target=load_trace(base/f'trace.{tp}.jsonl',lambda r:r['Flow']=='B' or r['Flow']=='M')
source=load_trace(base/f'trace.{sp}.jsonl',lambda r:r['Flow']=='D')
bench=load_trace(base/f'trace.{sp}.bench.jsonl',lambda r:r.get('Flow')=='D' and r.get('Stage')==4)
def stat(v):
 a=sorted(v)
 if not a:return {'n':0}
 def q(p):
  x=(len(a)-1)*p;lo=int(x);hi=min(lo+1,len(a)-1);return round(a[lo]+(a[hi]-a[lo])*(x-lo),3)
 return {'n':len(a),'p50':q(.5),'p90':q(.9),'mean':round(statistics.fmean(a),3)}
def flow(rows,f,s):return sorted((r for r in rows if r['Flow']==f and r['Stage']==s),key=lambda r:r['t'])
# Perf script parse, pairing sys_enter/sys_exit and collecting context switches.
line_re=re.compile(r'^\s*(.*?)\s+(\d+)/(\d+)\s+\[(\d+)\]\s+([0-9.]+):\s+(.*)$')
ent_re=re.compile(r'raw_syscalls:sys_enter: NR (\d+) \(([^)]*)\)')
ext_re=re.compile(r'raw_syscalls:sys_exit: NR (\d+) = (-?\d+)')
sw_re=re.compile(r'\s*(.*?):(\d+) \[[^]]+\] (\S+) ==> (.*?):(\d+) \[')
pending={};calls=[];switches=[]
with (base/'perf.script').open(errors='replace') as f:
 for line in f:
  m=line_re.match(line)
  if not m:continue
  comm,pid,tid,cpu,ts,event=m.groups();pid=int(pid);tid=int(tid);cpu=int(cpu);ts=float(ts)
  em=ent_re.search(event)
  if em and pid in (sp,tp): pending[pid,tid]=(ts,int(em.group(1)),em.group(2),comm,cpu);continue
  em=ext_re.search(event)
  if em and pid in (sp,tp):
   nr,ret=int(em.group(1)),int(em.group(2));p=pending.pop((pid,tid),None)
   if p and p[1]==nr:
    args=p[2].split(','); hx=lambda x:int(x.strip(),16) if re.fullmatch(r'[0-9a-fA-F]+',x.strip()) else -1
    calls.append({'pid':pid,'tid':tid,'nr':nr,'ret':ret,'start':p[0],'end':ts,'args':p[2],'fd':hx(args[0]) if args else -1,'len':hx(args[2]) if len(args)>2 else -1,'comm':p[3],'cpu':p[4]})
   continue
  if 'sched:sched_switch:' in event:
   sm=sw_re.search(event.split('sched:sched_switch:',1)[1])
   if sm:switches.append({'t':ts,'cpu':cpu,'old':int(sm.group(2)),'oldcomm':sm.group(1),'new':int(sm.group(5)),'newcomm':sm.group(4),'pid':pid})
# The four large datagram calls are identifiable by PID, syscall, and successful frame size.
def data(pid,nr):return sorted((c for c in calls if c['pid']==pid and c['nr']==nr and 1000<=c['ret']<=1600),key=lambda c:c['start'] if nr==44 else c['end'])
S=data(sp,44);Tr=data(tp,45);T=data(tp,44);R=data(sp,45)
print('socket calls',{'source_send':len(S),'target_recv':len(Tr),'target_send':len(T),'source_recv':len(R),'source_send_ret':collections.Counter(c['ret'] for c in S),'target_recv_ret':collections.Counter(c['ret'] for c in Tr),'target_send_ret':collections.Counter(c['ret'] for c in T),'source_recv_ret':collections.Counter(c['ret'] for c in R)})
perf_start=min(c['start'] for c in calls);perf_end=max(c['end'] for c in calls)
# Trim markers/stages to the actual perf capture interval.
def trim(xs):return [x for x in xs if perf_start<=x['t']<=perf_end]
B={s:trim(flow(target,'B',s)) for s in range(1,5)};D={s:trim(flow(source,'D',s)) for s in range(1,4)};D4=trim(bench)
M=trim([r for r in target if r['Flow']=='M'])
print('perf window',perf_start,perf_end,'trace counts',{'B':{k:len(v) for k,v in B.items()},'D':{k:len(v) for k,v in D.items()},'D4':len(D4),'M':len(M)})
# Indexed temporal lookup.
def times(xs,key):return [key(x) for x in xs]
Sstarts=times(S,lambda x:x['start']);Trends=times(Tr,lambda x:x['end']);Tstarts=times(T,lambda x:x['start']);Rends=times(R,lambda x:x['end'])
D1=D[1];D1times=times(D1,lambda x:x['t']);D4times=times(D4,lambda x:x['t']);Sstarts=times(S,lambda x:x['start'])
Bby={s:{r['Serial']:r for r in B[s]} for s in range(1,5)};Dby={s:{r['Serial']:r for r in D[s]} for s in range(1,4)}
def before(xs,ts,t):
 i=bisect.bisect_right(ts,t)-1;return xs[i] if i>=0 else None
def after(xs,ts,t):
 i=bisect.bisect_right(ts,t);return xs[i] if i<len(xs) else None
cycles=[]
for b1 in B[1]:
 b2=Bby[2].get(b1['Serial']);b3=Bby[3].get(b1['Serial']);b4=Bby[4].get(b1['Serial'])
 if not b2 or not b3 or not b4:continue
 tr=before(Tr,Trends,b1['t']);s=before(S,Sstarts,tr['end']) if tr else None
 t=after(T,Tstarts,b4['t']);r=after(R,Rends,t['end']) if t else None
 d1=after(D1,D1times,r['end']) if r else None
 if not all((s,tr,t,r,d1)):continue
 d2=Dby[2].get(d1['Serial']);d3=Dby[3].get(d1['Serial'])
 if not d2 or not d3:continue
 d4=after(D4,D4times,d3['t']);ns=after(S,Sstarts,d4['t']) if d4 else None
 if not d4 or not ns:continue
 seq=[s,tr,b1,b2,b3,b4,t,r,d1,d2,d3,d4,ns]
 ts=[s['start'],tr['end'],b1['t'],b2['t'],b3['t'],b4['t'],t['start'],r['end'],d1['t'],d2['t'],d3['t'],d4['t'],ns['start']]
 if all(ts[i]<ts[i+1] for i in range(12)):cycles.append((seq,ts))
print('complete ordered request cycles',len(cycles))
labels=['source send→target recv','target recv→B1','B1→B2','B2→B3','B3→B4','B4→target send','target send→source recv','source recv→D1','D1→D2','D2→D3','D3→D4','D4→next source send']
for i,label in enumerate(labels):
 vals=[(ts[i+1]-ts[i])*1e6 for _,ts in cycles]
 th=collections.Counter((seq[i].get('tid'),seq[i].get('comm')) if 'tid' in seq[i] else (seq[i].get('ThreadId'),seq[i].get('ThreadName')) for seq,ts in cycles)
 print('SEG',label,stat(vals),'threads',th.most_common(5))
# B2-B3 consecutive marker breakdown, same managed thread and request serial.
markers=collections.defaultdict(list)
for m in M:markers[m['Serial']].append(m)
subs=collections.defaultdict(list);seqcounts=collections.Counter();b_intervals=[]
for b2 in B[2]:
 b3=Bby[3].get(b2['Serial'])
 if not b3 or b2['ThreadId']!=b3['ThreadId']:continue
 ms=sorted((m for m in markers[b2['Serial']] if b2['t']<m['t']<b3['t'] and m['ThreadId']==b2['ThreadId']),key=lambda m:m['t'])
 ev=[('B2',b2['t'])]+[(str(m['Stage']),m['t']) for m in ms]+[('B3',b3['t'])]
 seqcounts[tuple(x[0] for x in ev)]+=1
 for (a,ta),(b,tb) in zip(ev,ev[1:]):subs[a+'→'+b].append((tb-ta)*1e6)
 b_intervals.append((b2['t'],b3['t'],b2['Serial'],b2['ThreadId'],b2['ThreadName'],ms))
print('B2B3',stat([(b-a)*1e6 for a,b,*_ in b_intervals]),'intervals',len(b_intervals),'marker sequence patterns',seqcounts.most_common(8))
for k,v in subs.items():print('MARK',k,stat(v))
# Map managed Long Running intervals to native TIDs through scheduler switches and target process syscall TIDs.
target_long={c['tid'] for c in calls if c['pid']==tp and c['comm'].startswith('.NET Long Runni')}
by_cpu=collections.defaultdict(list)
for sw in switches:by_cpu[sw['cpu']].append(sw)
runs=collections.defaultdict(list)
for cpu,ss in by_cpu.items():
 active={}
 for sw in sorted(ss,key=lambda x:x['t']):
  if sw['old'] in target_long and sw['old'] in active:
   start=active.pop(sw['old']);runs[sw['old']].append((start,sw['t']))
  if sw['new'] in target_long and sw['newcomm'].startswith('.NET Long Runni'):
   active[sw['new']]=sw['t']
for tid in runs:runs[tid].sort()
runstarts={tid:[a for a,b in xs] for tid,xs in runs.items()}
callgroups=collections.defaultdict(list)
for c in calls:
 if c['pid']==tp:callgroups[c['tid']].append(c)
callstarts={tid:[c['start'] for c in xs] for tid,xs in callgroups.items()}
per=collections.Counter();mapped=collections.Counter();hascalls=0;examples=[]
for a,b,serial,mid,name,ms in b_intervals:
 native=None
 for tid,starts in runstarts.items():
  ix=bisect.bisect_right(starts,a)-1
  if ix>=0 and runs[tid][ix][0]<=a<runs[tid][ix][1]:native=tid;break
 if native is None:
  # The managed marker is on CPU; look for the closest native long-running interval spanning B3.
  for tid,starts in runstarts.items():
   ix=bisect.bisect_right(starts,b)-1
   if ix>=0 and runs[tid][ix][0]<=b<runs[tid][ix][1]:native=tid;break
 mapped[native]+=1
 if native is None:continue
 xs=callgroups.get(native,[]);st=callstarts.get(native,[]);i=bisect.bisect_left(st,a);j=bisect.bisect_right(st,b)
 within=[c for c in xs[i:j] if c['end']<=b]
 if within:hascalls+=1
 for c in within:per[(c['nr'],c['ret'])]+=1
 if len(examples)<3:examples.append((a,b,serial,mid,native,[(c['nr'],c['ret'],c['start']-a,c['end']-a) for c in within]))
print('B2B3 native TID mapping',mapped,'intervals_with_syscall',hascalls,'syscall_nr_ret_counts',per,'examples',examples)
print('target Long Running TIDs',target_long)
