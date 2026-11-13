#!/usr/bin/env python3
import collections, json, re, subprocess
from pathlib import Path

JOB=Path('/root/jobs/fx/cx/v21/s033/dprof')
TRACE='/root/jobs/fx/cx/v21/p032net/tools/dotnet-trace8/dotnet-trace'

def classify(s):
    x=s.lower()
    if any(k in x for k in ['gc.', 'garbagecollector', 'gchandles', 'gc_heap', 'gc_alloc', 'native memory.alignedalloc', 'alignedalloc', 'message.finalize']): return 'allocation/GC'
    if any(k in x for k in ['semaphore','monitor.wait','waitone','spinwait','queueuserworkitem','threadpool.unsafequeue','completiondispatcher','executionpool','dispatchpump','poller.wait','lowlevellifosemaphore']): return 'lane/worker hand-off & wake'
    if any(k in x for k in ['protobuf','codec','serialize','deserialize','encode','decode','parsingprimitivesmessages','parsefrom']): return 'codec'
    if any(k in x for k in ['movenext','asyncmethodbuilder','taskcompletion','task.runcontinuations','awaittaskcontinuation','valuetask','executioncontext','getstatemachinebox']): return 'task/coroutine/future'
    if 'withgrpcbench' in x or 'benchmeasurement' in x: return 'benchmark driver'
    if any(k in x for k in ['systems.zlink','native','poller','socketkernel','socket.','interop+sys.', 'sendmsg','recvmsg','recvfrom','sendto']): return 'binding/Core call'
    if 'zlink.framework' in x: return 'Framework bookkeeping'
    return 'other'

def perf(raw,role):
    d={}
    p=raw/f'{role}.perf.csv'
    if p.exists():
        for line in p.read_text(errors='ignore').splitlines():
            a=line.split(',')
            if len(a)>2:
                try: d[a[2]]=float(a[0])
                except ValueError: pass
    return d

def get_samples(trace):
    tmp=trace.with_suffix('.speedscope.json')
    output_base=trace.with_suffix('')
    subprocess.run([TRACE,'convert',str(trace),'--format','Speedscope','-o',str(output_base)],stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
    x=json.loads(tmp.read_text())
    frames=x['shared']['frames']; threads=collections.Counter(); self_by_thread=collections.defaultdict(collections.Counter); inclusive_by_thread=collections.defaultdict(collections.Counter)
    for profile in x['profiles']:
        stack=[]
        for e in profile.get('events',[]):
            if e['type']=='O':
                idx=e['frame']; name=frames[idx].get('name','?')
                if name=='CPU_TIME':
                    names=[frames[i].get('name','?') for i in stack]
                    if names:
                        tid=re.search(r'\((\d+)\)',profile.get('name',''))
                        tid=tid.group(1) if tid else profile.get('name','?')
                        threads[tid]+=1
                        self_by_thread[tid][names[-1]]+=1
                        for n in set(names): inclusive_by_thread[tid][n]+=1
                stack.append(idx)
            elif stack: stack.pop()
    tmp.unlink(missing_ok=True)
    return sum(threads.values()),threads,self_by_thread,inclusive_by_thread

def thread_cpu(raw,role):
    import csv
    samples=collections.defaultdict(list); names={}; times=[]; hz=100
    p=raw/'thread-cpu.csv'
    if not p.exists(): return {},0.0
    for row in csv.reader(p.open()):
        if len(row)!=6 or row[1]!=role: continue
        try: ts=float(row[0]); tid=row[2]; ticks=int(row[3]); hz=int(row[4]); name=row[5]
        except ValueError: continue
        times.append(ts); samples[tid].append(ticks); names[tid]=name
    elapsed=max(times)-min(times) if times else 0
    cpu={tid:(max(0,(max(v)-min(v))/hz),names.get(tid,'?')) for tid,v in samples.items()}
    return cpu,elapsed

def result(impl,pattern,payload,mode):
    tag=f'dprof-{mode}-'
    p=JOB/'bench-out'/f'{tag}{pattern}-{impl}'/'run1'/f'{impl}-{pattern}-{payload}'/'results.json'
    return json.loads(p.read_text())['cells'][0]

rows=[]
for pattern in ['request-backpressure','request-serial']:
 for payload in [1024]:
  for impl in ['zlink-framework-dotnet','zlink-dotnet']:
   cell={'pattern':pattern,'payload':payload,'impl':'F' if impl.endswith('framework-dotnet') else 'B'}
   unc=result(impl,pattern,payload,'unprofiled'); prof=result(impl,pattern,payload,'profile')
   cell['unprofiled_mps']=unc['throughput_per_second']; cell['profile_mps']=prof['throughput_per_second']; cell['completed']=prof['completed']
   cell['alloc_src_Bmsg']=prof['client_allocated_bytes']/prof['completed']; cell['alloc_tgt_Bmsg']=prof['server_allocated_bytes']/prof['completed']
   cell['source_cores']=prof['client_cores']; cell['target_cpu_pct']=prof['server_cpu_percent']; cell['active_elapsed_ms']=prof['active_elapsed_ms']
   raw=JOB/'raw'/f'dprof-profile-{pattern}-{impl}'
   for role in ['source','target']:
    d=perf(raw,role); cmsg=prof['completed']; task_us=d.get('task-clock',0)*1000/cmsg
    total,threads,self_by_thread,inclusive_by_thread=get_samples(raw/f'{role}.nettrace')
    cpu,observed_s=thread_cpu(raw,role)
    tops=[]
    for tid,(sec,name) in sorted(cpu.items(),key=lambda z:z[1][0],reverse=True)[:10]:
      us=sec*1e6/cmsg*prof['active_elapsed_ms']/1000/max(observed_s,1e-9); tops.append({'tid':tid,'name':name,'cpu_s':sec,'us_msg':us,'core_share':sec/max(observed_s,1)})
    # Runtime /proc CPU ticks are authoritative per thread; perf task-clock is authoritative
    # for the process total. Scale the observed thread distribution to the process total.
    observed_total=sum(v[0] for v in cpu.values()) or 1
    scale=task_us/(observed_total*1e6/cmsg)
    cats=collections.Counter(); critical=collections.Counter()
    for tid,fc in self_by_thread.items():
     thread_samples=sum(fc.values()) or 1
     thread_cpu_msg=cpu.get(tid,(0,''))[0]*1e6/cmsg*scale
     for fn,n in fc.items():
      cats[classify(fn)]+=thread_cpu_msg*n/thread_samples
      if tops and tid==tops[0]['tid']: critical[classify(fn)]+=cpu.get(tid,(0,''))[0]*1e6/cmsg*prof['active_elapsed_ms']/1000/max(observed_s,1e-9)*n/thread_samples
    cats['other'] += max(0,task_us-sum(cats.values()))
    cell[role]={'task_us_msg':task_us,'samples':total,'task_clock_ms':d.get('task-clock',0),'contexts_msg':d.get('context-switches',0)/cmsg,'syscalls_msg':d.get('raw_syscalls:sys_enter',0)/cmsg,'futex_msg':d.get('syscalls:sys_enter_futex',0)/cmsg,'threads':tops,'observed_thread_cpu_s':observed_total,'thread_observation_s':observed_s,'cat_us_msg':dict(cats),'critical_cat_us_msg':dict(critical),'self_top':self_by_thread,'inclusive_by_thread':inclusive_by_thread,'perf':d}
   rows.append(cell)
   print(f"done {pattern} {payload} {cell['impl']}",flush=True)
(JOB/'profile-summary.json').write_text(json.dumps(rows,indent=2)+'\n')
for c in rows:
 print(f"{c['pattern']} {c['payload']} {c['impl']} unprof={c['unprofiled_mps']:.0f} prof={c['profile_mps']:.0f} alloc={c['alloc_src_Bmsg']:.0f}/{c['alloc_tgt_Bmsg']:.0f} B/m")
 for role in ['source','target']:
  d=c[role]; top=[(t['name'],round(t['us_msg'],1),round(t['core_share'],2)) for t in d['threads'][:4]]
  cats={k:round(v,1) for k,v in d['cat_us_msg'].items()}
  print(f"  {role} task={d['task_us_msg']:.1f} us/msg cs={d['contexts_msg']:.2f} sys={d['syscalls_msg']:.1f} futex={d['futex_msg']:.2f}; top={top}; cats={cats}")
