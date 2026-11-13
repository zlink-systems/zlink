#!/usr/bin/env python3
import json, subprocess, re, pathlib, collections
ROOT=pathlib.Path('/root/jobs/fx/cx/v21/s033/jprof'); PROFILES=ROOT/'profiles'
impls=['zlink-framework-java','zlink-java']; patterns=['send-saturation','request-backpressure','request-serial']
def cp(impl,pat,mode):
 name=f'jprof-plain-{impl}-{pat}' if mode=='plain' else f'jprof-{impl}-{pat}-{mode}'
 return ROOT/'out'/name/'run1'/f'{impl}-{pat}-1024'
def result(impl,pat,mode):
 p=cp(impl,pat,mode)/'results.json'; return json.load(open(p))['cells'][0] if p.exists() else None
def method(f):
 m=f.get('method') or {}; cls=(m.get('type') or {}).get('name','?').replace('/','.')
 return f"{cls}.{m.get('name','?')}:{f.get('lineNumber',-1)}"
def stage(m,frames):
 n=m.lower(); stack=' '.join(method(f).lower() for f in frames)
 if any(x in n for x in ['allocate','newinstance','allocateinstance','malloc','free','g1young','g1concurrent','garbagecollector']): return 'allocation'
 if any(x in n for x in ['locksupport','thread.park','thread.unpark','forkjoinpool','statelane','executionqueue','concurrentlinkedqueue','arrayblockingqueue','synchronousqueue','condition','awaitturn','awaitwork','deactivate','enqueueaccepted','runworker']): return 'handoff'
 if any(x in n for x in ['completablefuture','completionstage','futuretask','completionbridge','whencomplete','thenapply','asyncresult']): return 'task/future'
 if any(x in n for x in ['protobuf','jackson','jsonparser','codec','serializer','serialization','envelope','m6awire']): return 'codec'
 if any(x in n for x in ['downcallstub','native','socket','sendmsg','recvmsg','zlinkruntime','zlinkbinding','foreign.']): return 'binding/core'
 if 'systems.zlink.bench' in n or 'benchdriver' in n or 'benchstack' in n or 'benchmetric' in n or (('java.util.arrays.fill' in n or 'java.util.random' in n) and 'systems.zlink.bench' in stack): return 'benchmark driver'
 if 'systems.zlink.framework' in n or 'zlinkframework' in n or 'framework' in n: return 'framework bookkeeping'
 return 'other'
def profile_events(impl,pat,role):
 f=PROFILES/f'jprof-{impl}-{pat}-profile-{role}.jfr'; raw=subprocess.check_output(['jfr','print','--json','--events','jdk.ExecutionSample,jdk.ObjectAllocationSample,jdk.JavaMonitorWait,jdk.JavaMonitorEnter',str(f)])
 return json.loads(raw)['recording']['events'],f
def sample_threads(samples):
 counts=collections.Counter(); names={}; methods=collections.defaultdict(collections.Counter); stages=collections.defaultdict(collections.Counter); incl=collections.defaultdict(collections.Counter)
 for e in samples:
  v=e['values']; t=v.get('sampledThread') or v.get('eventThread') or {}; tid=int(t.get('osThreadId') or 0); jid=int(t.get('javaThreadId') or 0); key=tid if tid else -jid
  names[key]=t.get('osName') or t.get('javaName') or f'virtual-thread-{jid}'
  counts[key]+=1; fs=(v.get('stackTrace') or {}).get('frames') or []
  if fs:
   m=method(fs[0]); methods[key][m]+=1; stages[key][stage(m,fs)]+=1
   for f in fs:
    fm=method(f)
    if fm.startswith('systems.zlink.framework.') or fm.startswith('systems.zlink.framework.spring.'): incl[key][fm]+=1
 return counts,names,methods,stages,incl
def cpu_file(impl,pat,role,msgs):
 p=cp(impl,pat,'profile')/f'thread-cpu-{role}.tsv'; lines=p.read_text().splitlines(); hz=100
 m=re.search(r'hz=(\d+)',lines[0]); hz=int(m.group(1)) if m else 100
 d={}
 for l in lines[1:]:
  a=l.split('\t')
  try: d[int(a[0])]={'us':int(a[1])/hz*1e6/max(1,msgs),'name':a[-1]}
  except: pass
 return d

def jfr_counts(f):
 out={}
 for l in subprocess.check_output(['jfr','summary',str(f)],text=True).splitlines():
  a=l.split()
  if len(a)>1 and a[0].startswith('jdk.'):
   try:out[a[0]]=int(a[1])
   except:pass
 return out
for pat in patterns:
 print(f'\n## {pat}')
 for impl in impls:
  plain=result(impl,pat,'plain'); prof=result(impl,pat,'profile'); cnt=result(impl,pat,'counts'); msgs=prof['completed']; rate=prof['throughput_per_second']
  print(f"\n### {'Framework' if impl==impls[0] else 'Binding'}: plain={plain['throughput_per_second']:.1f}/s profiled={prof['throughput_per_second']:.1f}/s counts={cnt['throughput_per_second']:.1f}/s profile_msgs={msgs:,}")
  for role in ('source','target'):
   evs,jfr=profile_events(impl,pat,role); samples=[e for e in evs if e['type']=='jdk.ExecutionSample']; alloc=[e for e in evs if e['type']=='jdk.ObjectAllocationSample']; counts,names,methods,stages,incl=sample_threads(samples); cpu=cpu_file(impl,pat,role,msgs); total_cpu=sum(x['us'] for x in cpu.values()); total_samples=max(1,len(samples));
   # Fill full names for Java tids from JFR; /proc labels native IDs not in JFR remain OS comm.
   for tid,x in cpu.items():
    if tid in names: x['name']=names[tid]
   # per-thread observed CPU drives ranking; JFR-only virtual threads are supplemental estimates.
   ranked=sorted(cpu,key=lambda tid:cpu[tid]['us'],reverse=True)
   print(f' {role}: proc CPU={total_cpu:.3f} us/msg; ExecutionSample={len(samples)}; allocation samples={len(alloc)} ({len(alloc)/msgs:.6f}/msg), weighted bytes={sum(int(e["values"].get("weight") or 0) for e in alloc)/msgs:.1f} B/msg; primary={cpu[ranked[0]]["name"] if ranked else "not measured"}')
   print('  Thread | CPU µs/message | share of core | role')
   for tid in ranked[:16]:
    x=cpu[tid]; print(f'  {x["name"]} | {x["us"]:.3f} | {x["us"]*rate/1e6:.3f} | '+('native/core IO' if 'ZLINK' in x['name'] or 'zlink-jvm' in x['name'] else 'JVM'))
   # Stage breakdown based on self samples for each thread; native-only thread CPU is explicitly binding/core.
   proc_stages=collections.Counter()
   for tid,x in cpu.items():
    n=counts.get(tid,0)
    if n:
     for st,k in stages[tid].items(): proc_stages[st]+=x['us']*k/n
    else:
     nm=x['name'].lower()
     fallback='binding/core' if ('zlink' in nm or 'io/' in nm) else ('allocation' if 'gc' in nm else 'other')
     proc_stages[fallback]+=x['us']
   # Virtual threads don't own Linux CPU separately; retain sampled work as unassigned to avoid double counting.
   v_samples=sum(counts[k] for k in counts if k<0)
   print('  Process stage µs/message (self-sample classification × measured per-thread CPU; native unsampled CPU mapped by thread name)')
   for st,v in proc_stages.most_common(): print(f'  {st} | {v:.3f} | {v/total_cpu:.1%}')
   crit=ranked[0] if ranked else None
   if crit:
    x=cpu[crit]; thread_samples=counts.get(crit,0); print(f'  Critical-thread stage µs/message ({x["name"]}; no JFR samples means native CPU assigned to binding/core)')
    if thread_samples:
     for st,k in stages[crit].most_common(): print(f'  {st} | {x["us"]*k/thread_samples:.3f} | {k/thread_samples:.1%}')
    else: print(f'  binding/core | {x["us"]:.3f} | 100.0%')
    print('  Framework-only functions on critical thread, self/inclusive µs/message (inclusive rows overlap)')
    if thread_samples:
     top=incl[crit].most_common(20)
     for m,k in top: print(f'  {m} | self={methods[crit][m]*x["us"]/thread_samples:.3f} | inclusive={k*x["us"]/thread_samples:.3f} | selfSamples={methods[crit][m]} inclusiveSamples={k}')
    else: print('  None sampled: critical thread is native/core IO and has no Java stack frames.')
   # Also print top Java-framework thread where native thread is critical, for actionable source locations.
   java_candidates=[t for t in ranked if counts.get(t,0) and ('zlink' not in cpu[t]['name'].lower() or 'raw-mesh' in cpu[t]['name'].lower())]
   if crit and counts.get(crit,0)==0 and java_candidates:
    jt=java_candidates[0]; jx=cpu[jt]; jn=counts[jt]
    print(f'  Busiest sampled Java Framework thread supplement: {jx["name"]} ({jx["us"]:.3f} µs/msg)')
    for m,k in incl[jt].most_common(20): print(f'  {m} | self={methods[jt][m]*jx["us"]/jn:.3f} | inclusive={k*jx["us"]/jn:.3f} | selfSamples={methods[jt][m]} inclusiveSamples={k}')
   jc=jfr_counts(jfr)
   for n in ('jdk.ThreadPark','jdk.JavaMonitorWait','jdk.JavaMonitorEnter'): print(f'  {n}: {jc.get(n,0)} ({jc.get(n,0)/msgs:.6f}/msg)')
  # Separate active-window syscall-count run; throughput is highly perturbed by strace and not used for CPU/stack attribution.
  cm=cnt['completed']; d=cp(impl,pat,'counts')
  print(' counts (strace and /proc, separate intrusive run)')
  for role in ('source','target'):
   txt=(d/f'strace-{role}.txt').read_text(); total=futex=0
   for line in txt.splitlines():
    m=re.match(r'\s*[\d.]+\s+[\d.]+\s+\d+\s+(\d+)\s+\S*\s*(\S+)',line)
    if m: total+=int(m.group(1)); futex+=int(m.group(1)) if m.group(2)=='futex' else 0
   s={}; e={}
   for path,out in [(d/f'threads-start-{role}.tsv',s),(d/f'threads-end-{role}.tsv',e)]:
    for line in path.read_text().splitlines():
     if line.startswith('#') or not line.strip():continue
     a=line.split('\t')
     if len(a)>=5:
      try:out[int(a[0])]=(int(a[2]),int(a[3]))
      except:pass
   vol=nv=0
   for tid,(v,n) in s.items():
    if tid in e:vol+=max(0,e[tid][0]-v);nv+=max(0,e[tid][1]-n)
   print(f'  {role}: syscalls={total} ({total/cm:.3f}/msg), futex={futex} ({futex/cm:.3f}/msg), context switches={vol+nv} ({(vol+nv)/cm:.3f}/msg)')
