from pathlib import Path
import re,csv,json,sys,collections
j=Path(__file__).parent
prefix=sys.argv[1] if len(sys.argv)>1 else 'pm'
variant=sys.argv[2] if len(sys.argv)>2 else 'branch'
path=j/prefix/variant/'run1/zlink-framework-cpp-send-saturation-1024/capture/sched.script'
if not path.exists():path=j/prefix/variant/'sched.script'
event_re=re.compile(r'\s(\d+\.\d+):\s+sched:(sched_switch|sched_wakeup_new|sched_wakeup): (.*)')
switch_re=re.compile(r'.*?:(\d+) \[\d+\] (\S+) ==> .*?:(\d+) \[\d+\]')
wake_re=re.compile(r'.*?:(\d+) \[\d+\] CPU:')
waits=[];runnable={};running=set();running_since={};runs=[];eligible=set()
for line in path.open():
 eligible.update(int(x) for x in re.findall(r"bench_cpp_pm_se:(\d+)",line))
 m=event_re.search(line)
 if not m:continue
 t=float(m[1]);kind=m[2];payload=m[3]
 if kind=='sched_switch':
  s=switch_re.match(payload)
  if not s:continue
  prev,state,nxt=int(s[1]),s[2],int(s[3])
  running.discard(prev)
  if prev in running_since:runs.append((running_since.pop(prev),t,prev))
  if state.startswith('R') and prev:runnable.setdefault(prev,t)
  else:runnable.pop(prev,None)
  if nxt in runnable:waits.append((runnable.pop(nxt),t,nxt))
  running.add(nxt);running_since[nxt]=t
 else:
  s=wake_re.match(payload)
  if not s:continue
  tid=int(s[1])
  if tid not in running:runnable.setdefault(tid,t)
groups=collections.defaultdict(list)
for a,b,c,d,e in csv.reader((j/(prefix+'-'+variant+'.csv')).open()):groups[int(a)].append((int(b)/1e9,int(c),d,e))
records=[(rs[-1][0]-rs[0][0],id,rs) for id,rs in groups.items() if id>1 and rs[-1][2]=='request_complete']
ordered=sorted(records);chosen=[min(records,key=lambda r:abs(r[0]-.00754)),ordered[__import__('math').ceil(.99*len(ordered))-1]]
result=[]
for duration,id,rs in chosen:
 a,b=rs[0][0],rs[-1][0]
 ws=[(max(x,a),min(y,b),tid) for x,y,tid in waits if tid in eligible and y>a and x<b]
 out={'id':id,'duration_ms':duration*1000,'start_s':a,'end_s':b,'events':[{'offset_ms':(t-a)*1000,'tid':tid,'stage':s,'key':k} for t,tid,s,k in rs],'runnable_waits_over_0_2ms':[{'tid':tid,'wake_or_preempt_s':x,'scheduled_s':y,'duration_ms':(y-x)*1000} for x,y,tid in sorted(ws,key=lambda v:-(v[1]-v[0])) if y-x>.0002]}
 result.append(out)
 print(variant,id,'total_ms',round(out['duration_ms'],3))
 for e in out['events']:print(' ',round(e['offset_ms'],6),e['tid'],e['stage'])
 print(' runnable waits >=0.2 ms:')
 for w in out['runnable_waits_over_0_2ms'][:10]:print(' ',w['tid'],round(w['duration_ms'],6),w['wake_or_preempt_s'],w['scheduled_s'])
(j/(prefix+'-'+variant+'-sched-analysis.json')).write_text(json.dumps(result,indent=2))
