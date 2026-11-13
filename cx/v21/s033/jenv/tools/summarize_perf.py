from pathlib import Path
import re,json,subprocess,collections
J=Path('/root/jobs/fx/cx/v21/s033/jenv/runs')
head=re.compile(r'^(.+?)\s+(\d+)/(\d+)\s+([0-9.]+):\s+(\d+)\s+(cpu-clock|cycles:k):')
frame=re.compile(r'^\s+([0-9a-f]+) (.*) \(([^()]*)\)$')
def parse(data,cache):
 if cache.exists():return json.load(open(cache))
 err=open(str(cache)+'.stderr','w');proc=subprocess.Popen(['perf','script','--ns','-i',str(data),'-F','comm,pid,tid,time,event,period,ip,sym,dso'],stdout=subprocess.PIPE,stderr=err,text=True)
 samples=[];cur=None
 for l in proc.stdout:
  m=head.match(l)
  if m:
   if cur:samples.append(cur)
   cur={'tid':m[3],'time':float(m[4]),'period':int(m[5]),'event':m[6],'frames':[]};continue
  if not l.strip():
   if cur:samples.append(cur);cur=None
  elif cur:
   m=frame.match(l)
   if m:cur['frames'].append((m[2],m[3]))
 if cur:samples.append(cur)
 assert proc.wait()==0
 err.close();cache.write_text(json.dumps(samples,separators=(',',':')));return samples

def bucket(frames):
 s='\n'.join(f[0] for f in frames)
 if re.search(r'__libc_send|__GI___libc_send|^__send(?:to|msg)?(?:\+|$)|^send(?:to|msg)?$',s,re.M):return 'send'
 if re.search(r'__libc_recv|__GI___libc_recv|^__recv(?:from|msg)?(?:\+|$)|^recv(?:from|msg)?$',s,re.M):return 'recv'
 if re.search(r'^(?:__GI___libc_|__libc_|__)?read(?:\+.*)?$',s,re.M):return 'read'
 if re.search(r'^(?:__GI___libc_|__libc_|__)?write(?:\+.*)?$',s,re.M):return 'write'
 if 'epoll_wait' in s:return 'epoll_wait'
 if 'epoll_ctl' in s:return 'epoll_ctl'
 if 'entry_SYSCALL' in s or 'do_syscall_64' in s:return 'other_syscall'
 return 'userspace_other'
all=[]
for cap in J.glob('*/capture'):
 if cap.parent.name not in ('main','prototype','prototype-unpaced','binding'):continue
 if not (cap/'after.json').exists():continue
 b=json.load(open(cap/'before.json'));a=json.load(open(cap/'after.json'));is_send='send-saturation' in str(cap)
 n=a['stats']['target']['received']-b['stats']['target']['received'] if is_send else a['stats']['source']['completed']-b['stats']['source']['completed'];seconds=a['monotonic']-b['monotonic']
 r={'capture':str(cap),'messages':n,'seconds':seconds,'rate':n/seconds,'roles':{}}
 for role in ('source','target'):
  samples=parse(cap/(role+'.perf.data'),cap/(role+'.samples.json'));threads={};agg=collections.Counter();allself=collections.Counter();allinclusive=collections.Counter()
  for tid,t in b['processes'][role]['threads'].items():
   if 'IO/' not in t['comm'] or tid not in a['processes'][role]['threads']:continue
   cpu=(a['processes'][role]['threads'][tid]['runtime_ns']-t['runtime_ns'])/1e3/n
   ss=[s for s in samples if s['tid']==tid and s['frames'] and s['event']=='cpu-clock'];counts=collections.Counter(bucket(s['frames']) for s in ss);num=len(ss)
   stages={k:cpu*v/num for k,v in counts.items()} if num else {'not_sampled':cpu}
   for k,v in stages.items():agg[k]+=v
   threads[tid]={'comm':t['comm'],'cpu_us_per_message':cpu,'cores':cpu*n/1e6/seconds,'samples':num,'stages':stages,'bucket_counts':dict(counts)}
   for s in ss:
    allself[s['frames'][0][0]]+=cpu/num
    for f in set(f[0] for f in s['frames']):allinclusive[f]+=cpu/num
  r['roles'][role]={'threads':threads,'io_cpu_us_per_message':sum(t['cpu_us_per_message'] for t in threads.values()),'io_stage_us_per_message':dict(agg),'self_us_per_message':allself.most_common(45),'inclusive_us_per_message':allinclusive.most_common(60)}
 (cap/'summary.json').write_text(json.dumps(r,indent=2));all.append(r)
 print(cap.parent.name,round(r['rate']),[(role,round(r['roles'][role]['io_cpu_us_per_message'],2)) for role in ('source','target')],flush=True)
(J/'perf-summary.json').write_text(json.dumps(all,indent=2))
