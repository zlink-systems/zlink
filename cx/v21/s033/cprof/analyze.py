from pathlib import Path
from collections import Counter,defaultdict
import json,re,subprocess,csv,time
ROOT=Path('/root/jobs/fx/cx/v21/s033/cprof')
STAGES=['allocation','handoff/wake','task/coroutine/future','codec','binding/Core','Framework bookkeeping','benchmark driver','other']
# Classification is ordered and exclusive. It stops before broad runtime caller frames.
ALLOC=re.compile(r'(^|\W)(?:__GI_)?(?:__libc_|__GI___libc_)?(?:malloc|calloc|realloc|free|cfree|aligned_alloc|posix_memalign)(?:\b|_)|_int_(?:malloc|free|realloc|memalign)|_int_free_merge_chunk|malloc_consolidate|sysmalloc|tcache_|operator (?:new|delete)|alloc_new_heap|arena_get|arena_for_chunk|heap_trim')
SYNC=re.compile(r'futex|__lll_|__GI___lll_|pthread_mutex|pthread_cond|condition_variable|__gthread_mutex|__gthread_cond|__mutex_base|__mutex_cancel|__pthread_(?:mutex|cond)|try_to_wake_up|wake_up_q|finish_task_switch|pick_next_task|__schedule|schedule_hrtimeout|schedule_timeout|select_task_rq|dequeue_task|enqueue_task|update_(?:curr|load_avg)|set_next_task|ttwu_|check_preempt|sched_yield')
CODEC=re.compile(r'envelope_codec_t::|client_call_codec_t::|service_wire_codec_t::|runtime::protocol::.*(?:encode|decode|write|read|pack|unpack|measure)|google::protobuf::|BenchPayload::(?:_InternalSerialize|ByteSizeLong|_InternalParse|MergeImpl|CopyFrom)|protobuf_codec|header_sax_t|canonical_header|serialize_payload|deserialize_payload|serializer_registry_t::|encode_bench_payload|response_envelope|decode_header|encode_header')
TASK=re.compile(r'(?:task_shared_state|task_completion_source|request_task|request_receiver|request_admission_source|task_t<|task_promise|task_awaiter|observe_task|complete_task|make_ready_task|_State_baseV2|_Result_base|__future_base|std::promise|std::future|coroutine_handle|dispatch_task|task_continuation|await_result|try_publish|try_complete)')
LANE=re.compile(r'state_lane_t::|serial_execution_queue_t::|offload_executor_t::(?:post|submit|enqueue|schedule|worker_loop|complete_work|release_role|transfer_role)|completion_dispatcher_t::|coroutine_executor_t::|dispatch_completion\(|completion_signal_t::|ready_queue_t::|application_job_queue_t::|application_permit_supply_t::')
DRIVER=re.compile(r'framework_driver_t::|binding_driver_t::|raw_driver_t::|source_t::|zlink_cpp_bench::|\(anonymous namespace\)::(?:request_loop|command_loop)|request_loop\(|command_loop\(')
CORE=re.compile(r'^zlink::(?!framework|framework_codecs)|^zlink_(?:msg|socket|send|recv|request|reply|poll|routing|router|context)|^s_(?:send|recv)|asio|boost::asio|tcp_(?:send|recv)|(?:__sys_|__x64_sys_)(?:send|recv|read|write|poll|epoll)|sock_(?:send|recv)|__sock_|sk_stream_|skb_|ip_|inet_|napi_|net_rx_|do_softirq|handle_softirqs|__dev_queue|loopback|udp_|security_socket|unix_|packet_|__alloc_skb|copy_.*(?:user|iter)|ep_poll|epoll_|eventfd_|signalfd_|do_epoll|do_readv|do_writev|vfs_(?:read|write)|__GI___libc_(?:read|write|send|recv|poll)|__GI___poll|__GI_epoll')

def owner_name(sym):
 # Remove template arguments and the return type before recognizing a function owner.
 sym=sym.replace('(anonymous namespace)','anon')
 out=[];depth=0
 for ch in sym:
  if ch=='<':
   if depth==0:out.append('<>')
   depth+=1
  elif ch=='>' and depth:depth-=1
  elif depth==0:out.append(ch)
 text=''.join(out).split('(',1)[0]
 return text.split()[-1] if text.split() else text
TASK_OWNER=re.compile(r'task_shared_state_t|task_completion_source_t|request_task_t|request_task_access_t|request_receiver_t|request_admission_source_t|request_public_state_t|task_promise|task_awaiter|observe_task|complete_task|make_ready_task|__future_base|std::promise|std::future|coroutine_handle|dispatch_task|task_continuation|await_result|try_publish|try_complete|task_t<>::(?:await|result|promise|~|task_t)')
DRIVER_OWNER=re.compile(r'(?:framework_driver_t|binding_driver_t|raw_driver_t|source_t|completion_signal_t|ready_queue_t|server_metrics_t|latency_sampler_t|stats_http_server_t)::|(?:^|::)(?:request_loop|command_loop|make_payload|now_ns|can_submit_framework_task|encode_bench_payload)')
CODEC_OWNER=re.compile(r'runtime::messaging::|runtime::protocol::|envelope_codec_t::|client_call_codec_t::|header_writer_t::|service_wire_codec_t::|runtime::protocol::.*(?:encode|decode|write|read|pack|unpack|measure)|google::protobuf::|BenchPayload::|protobuf_codec|header_sax_t|canonical_header|serialize_payload|deserialize_payload|serializer_registry_t::|encode_bench_payload|response_envelope|decode_header|encode_header')
def classify(frames):
 syms=[s for s,d in frames]
 if any(ALLOC.search(s) for s in syms[:15]):return 'allocation'
 if any(SYNC.search(s) for s in syms[:12]):return 'handoff/wake'
 for s,d in frames:
  own=owner_name(s)
  if CODEC_OWNER.search(own):return 'codec'
  if CORE.search(own) or '/libzlink.so' in d:
   if ('boost::asio' in own or 'asio' in own) and any('completion_dispatcher_t' in ss or 'coroutine_executor_t' in ss or 'dispatch_completion(' in ss for ss,dd in frames):return 'handoff/wake'
   return 'binding/Core'
  if DRIVER_OWNER.search(own):return 'benchmark driver'
  if LANE.search(own):return 'handoff/wake'
  if TASK_OWNER.search(own):return 'task/coroutine/future'
  if (own.startswith('std::_Sp_') or own.startswith('std::__shared_count')) and TASK_OWNER.search(s):return 'task/coroutine/future'
  if 'zlink::framework::' in own and '::bench::' not in own:return 'Framework bookkeeping'
  if 'std::_Function_handler' in own and 'zlink::framework::' in s:return 'Framework bookkeeping'
 return 'other'

head=re.compile(r'^(.+?)\s+(\d+)/(\d+)\s+([0-9.]+):\s+(\d+)\s+cpu-clock:')
frame=re.compile(r'^\s+([0-9a-f]+) (.*) \(([^()]*)\)$')
def parse_perf(data,cache):
 if cache.exists():return json.loads(cache.read_text())
 cmd=['perf','script','--ns','-i',str(data),'-F','comm,pid,tid,time,event,period,ip,sym,dso']
 err=open(str(cache)+'.stderr','w'); proc=subprocess.Popen(cmd,stdout=subprocess.PIPE,stderr=err,text=True)
 samples=[];cur=None
 for l in proc.stdout:
  m=head.match(l)
  if m:
   if cur:samples.append(cur)
   cur={'tid':m[3],'time':float(m[4]),'period':int(m[5]),'frames':[]};continue
  if not l.strip():
   if cur:samples.append(cur);cur=None
  elif cur:
   m=frame.match(l)
   if m:cur['frames'].append((m[2],m[3]))
 if cur:samples.append(cur)
 assert proc.wait()==0
 err.close();cache.write_text(json.dumps(samples,separators=(',',':')))
 return samples

def counts_summary(cell):
 cap=cell/'capture';b=json.loads((cap/'before.json').read_text());a=json.loads((cap/'after.json').read_text());txt=(cap/'bpf.txt').read_text()
 result={}
 for role in ['source','target']:
  pid=a['processes'][role]['pid'];calls=Counter();alloc=0;thread=defaultdict(lambda:Counter())
  for p,tid,nr,v in re.findall(r'@syscalls\[(\d+), (\d+), (\d+)\]: (\d+)',txt):
   if int(p)==pid:calls[int(nr)]+=int(v);thread[tid]['syscalls']+=int(v);thread[tid]['futex']+=int(v) if int(nr)==202 else 0
  for p,tid,v in re.findall(r'@alloc\[(\d+), (\d+)\]: (\d+)',txt):
   if int(p)==pid:alloc+=int(v);thread[tid]['alloc']+=int(v)
  msgs=a['stats']['source']['completed']-b['stats']['source']['completed']
  result[role]={'messages':msgs,'allocations':alloc,'syscalls':sum(calls.values()),'futex':calls[202],'syscalls_by_number':dict(calls),'threads':dict(thread)}
 result['seconds']=a['monotonic']-b['monotonic'];result['source_rate']=msgs/result['seconds'];result['gated']='START' in txt
 return result

def main():
 allresults={}
 for cell in sorted((ROOT/'out-profile/run1').iterdir()):
  cap=cell/'capture'
  if not (cap/'after.json').exists():continue
  b=json.loads((cap/'before.json').read_text());a=json.loads((cap/'after.json').read_text());msgs=(a['stats']['target']['received']-b['stats']['target']['received']) if 'send-saturation' in cell.name else (a['stats']['source']['completed']-b['stats']['source']['completed']); secs=a['monotonic']-b['monotonic']
  res={'messages':msgs,'seconds':secs,'stats_before':b['stats'],'stats_after':a['stats'],'roles':{}}
  for role in ['source','target']:
   samples=parse_perf(cap/f'{role}.perf.data',cap/f'{role}.samples.json'); threads={}; groups=defaultdict(list)
   for sm in samples:groups[sm['tid']].append(sm)
   bt=b['processes'][role]['threads'];at=a['processes'][role]['threads']
   for tid,t in at.items():
    old=bt.get(tid,dict(runtime_ns=0,voluntary=0,involuntary=0));cpu=(t['runtime_ns']-old['runtime_ns'])/1e9
    sm=groups.get(tid,[]);weight=sum(x['period'] for x in sm);stage=Counter();selfs=Counter();incl=Counter();dsos={}; sub=Counter(); owners=Counter()
    for x in sm:
     fr=x['frames'];period=x['period'];cat=classify(fr);stage[cat]+=period
     if fr:
      selfs[fr[0][0]]+=period
      for sym in set(y[0] for y in fr):incl[sym]+=period
      for sym,dso in fr:dsos[sym]=dso
      # Direct sampled allocation / sync / Core consumers, useful for audit and candidate ceilings.
      if cat=='allocation' and any(SYNC.search(s) for s,d in fr[:12]):sub['allocator lock/wake']+=period
      if cat=='handoff/wake' and any('completion_signal_t' in s for s,d in fr):sub['driver completion signal']+=period
      if cat=='handoff/wake' and any('state_lane_t' in s for s,d in fr):sub['state lane sync']+=period
      if cat=='handoff/wake' and any('offload_executor_t' in s for s,d in fr):sub['offload executor sync']+=period
      if cat=='binding/Core':
       owner=next((s for s,d in fr if '/libzlink.so' in d or s.startswith('zlink_')), 'kernel/transport')
       owners[owner]+=period
    if not weight and cpu>0:stage['other']=1;weight=1
    factor=cpu*1e6/msgs/weight if weight else 0
    rows=[{'symbol':sym,'dso':dsos.get(sym,''),'self_us':selfs[sym]*factor,'inclusive_us':v*factor,'self_samples':round(selfs[sym]/5025125,2)} for sym,v in incl.items()]
    rows.sort(key=lambda r:r['self_us'],reverse=True)
    threads[tid]={'comm':t['comm'],'cpu_seconds':cpu,'cores':cpu/secs,'cpu_us':cpu*1e6/msgs,'context_switches':t['voluntary']+t['involuntary']-old['voluntary']-old['involuntary'],'samples':len(sm),'sampled_seconds':sum(x['period'] for x in sm)/1e9,'stage_us':{k:stage[k]*factor for k in STAGES},'sub_us':{k:v*factor for k,v in sub.items()},'core_owner_us':{k:v*factor for k,v in owners.items()},'functions':rows}
   totalcpu=sum(t['cpu_us'] for t in threads.values());totalstages={k:sum(t['stage_us'][k] for t in threads.values()) for k in STAGES};crit=max(threads,key=lambda k:threads[k]['cpu_seconds'])
   rr={'pid':a['processes'][role]['pid'],'cpu_us':totalcpu,'cores':totalcpu*msgs/1e6/secs,'context_switches_per_msg':sum(t['context_switches'] for t in threads.values())/msgs,'stage_us':totalstages,'critical_tid':crit,'threads':threads,'samples':len(samples)}
   res['roles'][role]=rr
   with open(cap/f'{role}.threads.csv','w') as f:
    w=csv.writer(f);w.writerow(['tid','comm','cpu_us/msg','cores','context_switches/msg','samples','sample_cpu_s','exact_cpu_s']+STAGES)
    for tid,t in sorted(threads.items(),key=lambda it:-it[1]['cpu_us']):w.writerow([tid,t['comm'],t['cpu_us'],t['cores'],t['context_switches']/msgs,t['samples'],t['sampled_seconds'],t['cpu_seconds']]+[t['stage_us'][s] for s in STAGES])
   with open(cap/f'{role}.critical-functions.csv','w') as f:
    w=csv.DictWriter(f,fieldnames=['symbol','dso','self_us','inclusive_us','self_samples']);w.writeheader();w.writerows(threads[crit]['functions'])
  allresults[cell.name]=res
 for c in sorted((ROOT/'out-counts/run1').glob('*')):
  if (c/'capture/after.json').exists():allresults.setdefault(c.name,{})['counts']=counts_summary(c)
 (ROOT/'analysis.json').write_text(json.dumps(allresults,indent=2))
 for k,r in allresults.items():
  print(k,'rate',round(r.get('messages',0)/r.get('seconds',1),1))
  for role,rr in r.get('roles',{}).items():
   ct=rr['threads'][rr['critical_tid']];print(role,'cpu',round(rr['cpu_us'],3),'critical',rr['critical_tid'],round(ct['cpu_us'],3),'stages', {s:round(v,3) for s,v in rr['stage_us'].items()},'critical stages',{s:round(v,3) for s,v in ct['stage_us'].items()})
if __name__=='__main__':main()
