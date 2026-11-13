from pathlib import Path
import re,json,collections,subprocess,statistics,sys
j=Path(__file__).parent
profile_root=sys.argv[1] if len(sys.argv)>1 else 'perf'
header=re.compile(r'(\d+)/(\d+)\s+([\d.]+):\s+(\d+) cpu-clock:')
def frame(line):
 m=re.match(r'\s*([0-9a-f]+)\s+(.*) \((.*)\)$',line)
 return (m[2],m[3],m[1]) if m else None
def classify(fs):
 names=[f[0] for f in fs];all='\n'.join(names);leaf=names[0]
 # Leaf/inlined synchronization CPU, then identify the first state owner in the chain.
 sync=any(re.search(r'futex|_lll_lock|pthread_mutex|lll_mutex|pthread_cond|condition_variable',s) for s in names[:18])
 if sync:
  for s in names:
   if 'command_handler_t' in s or 'echo_handler_t' in s or 'server_metrics_t' in s:return 'handler / metrics mutex'
   if 'service_mailbox_t::' in s:return 'mailbox mutex / futex'
   if 'application_job_queue_t::' in s or 'handler_entry_t::' in s:return 'permit mutex / futex'
   if 'offload_executor_t::' in s:return 'executor mutex / futex'
   if 'service_provider_t::' in s:return 'DI mutex / futex'
  return 'other mutex / futex'
 if 'command_handler_t::handle' in leaf or 'echo_handler_t::handle' in leaf or 'server_metrics_t::record' in leaf or 'now_ns' in leaf:return 'handler body'
 # This row is CPU at shared ownership operations, not a measurement of HITM stall time.
 if ('_Sp_counted_base' in leaf or 'application_work_' in leaf or 'application_job_queue_t::permit_t::' in leaf or 'application_job_queue_t::release_credits' in leaf):return 'shared counter / refcount sites'
 # Attribute to the nearest recognized operation, avoiding caller-inclusive double counting.
 for s in names:
  if 'command_handler_t::handle' in s or 'echo_handler_t::handle' in s or 'server_metrics_t::record' in s:return 'handler body'
  if any(x in s for x in ('envelope_codec_t::','canonical_header_reader_t','header_sax_t','protobuf::','protobuf_codec','decode_mesh_record_header')):return 'codec / typed decode'
  if 'service_mailbox_t::' in s or 'service_mailbox_claim_t' in s:return 'take / settle bookkeeping'
  if any(x in s for x in ('application_claim_batch_t::','receive_record_t::','recycle_application_claim_batch','acquire_application_claim_batch','take_application_record','dispatch_application_claim','dispatch_taken_application')):return 'take / settle bookkeeping'
  if 'offload_executor_t::' in s:return 'executor bookkeeping'
  if 'application_job_queue_t::' in s or 'handler_entry_t::' in s:return 'permit bookkeeping'
  if any(x in s for x in ('route_handler_invoker_t::','route_packet_dispatcher_t::','mesh_record_dispatcher_t::','dispatch_application(','dispatch_mesh_record(','application_dispatch_terminal_owner_t::','route_handler_registry_t::','service_provider_t::')):return 'dispatch / DI / terminal'
 return 'other / unresolved'
rows=[]
for p in sorted((j/profile_root).glob('*/*')):
 dirs=list(p.glob('run1/*/capture'))
 if not dirs or not (dirs[0]/'after.json').exists():continue
 d=dirs[0];cell=d.parent
 raw=d/'perf.script' if (d/'perf.script').exists() else p/'perf.script'
 if not raw.exists():
  with raw.open('w') as f,(p/'perf-script.log').open('w') as log:subprocess.run(['flock','-o','/root/jobs/fx/cx/measure.lock','taskset','-c','0-7','perf','script','-i',str(d/'perf.data'),'-F','comm,pid,tid,time,period,event,ip,sym,dso'],stdout=f,stderr=log,check=True)
 ss=json.loads((d/'samples.json').read_text());first,last=ss[0],ss[-1]
 n=last['stats']['target']['received']-first['stats']['target']['received']
 roles={}
 for role,tid in re.findall(r'ROLE (\S+) tid=(\d+)',(cell/'target.log').read_text()):roles[tid]=role
 totals=collections.Counter();switches=collections.Counter();previous=dict(first['processes']['target']['threads'])
 for snap in ss[1:]:
  for tid,now in snap['processes']['target']['threads'].items():
   old=previous.get(tid,{k:0 for k in ('runtime_ns','voluntary','involuntary')})
   totals[tid]+=max(0,now['runtime_ns']-old['runtime_ns'])
   switches[tid]+=max(0,now['voluntary']+now['involuntary']-old['voluntary']-old['involuntary'])
   previous[tid]=now
 app={tid for tid,r in roles.items() if r=='app-worker'};rec={tid for tid,r in roles.items() if r=='receive'}
 sync_details=collections.Counter();group=collections.Counter();samples=collections.Counter();leaves=collections.Counter();chains=collections.Counter();stamps=[];cpus=collections.Counter();rawsum=collections.Counter();counts=collections.Counter()
 for block in raw.read_text().split('\n\n'):
  lines=block.splitlines()
  if not lines:continue
  m=header.search(lines[0])
  if not m:continue
  tid=m[2];ts=float(m[3]);period=int(m[4]);fs=[f for line in lines[1:] if (f:=frame(line))]
  if not fs:continue
  # Main send data uses default local/raw clock; all later recordings explicitly use mono.
  if not (profile_root=='perf' and p.parts[-2:] == ('send-saturation','main')) and not first['time']<=ts<=last['time']:continue
  role='app' if tid in app else ('receive' if tid in rec else 'other')
  rawsum[role]+=period;counts[role]+=1
  if role!='app':continue
  g=classify(fs);group[g]+=period;samples[g]+=1;leaves[fs[0][0]]+=period
  if 'mutex / futex' in g:
   chain='\n'.join(x[0] for x in fs)
   sub='condition variable' if ('condition_variable' in chain or 'pthread_cond' in chain) else ('mutex wake' if any(x in chain for x in ('futex_wake','lll_lock_wake','pthread_mutex_unlock')) else ('mutex wait' if any(x in chain for x in ('futex_wait','lll_lock_wait')) else 'mutex user path'))
   sync_details[g+' / '+sub]+=period
  if 'mutex / futex' in g:chains[g+' | '+' -> '.join(f[0] for f in fs[:20])]+=period
  stamps.append(ts)
 total_cpu=sum(totals[t] for t in app)/1000/n
 fraction=sum(group.values())
 row={'pattern':p.parent.name,'variant':p.name,'messages':n,'elapsed_s':last['time']-first['time'],'application_us_msg':total_cpu,'receive_us_msg':sum(totals[t] for t in rec)/1000/n,'application_tids':sorted(app),'switches_msg':sum(switches.values())/n,'sampled_clock_ns':rawsum,'sample_counts':counts,'groups_us_msg':{g:v/fraction*total_cpu for g,v in group.items()},'group_samples':samples,'sync_details_us_msg':{g:v/fraction*total_cpu for g,v in sync_details.items()},'groups_sample_period_us_msg':{g:v/1000/n for g,v in group.items()},'profile_sample_range':(min(stamps),max(stamps)) if stamps else None,'leaf_top':leaves.most_common(45)}
 if (cell/'results.json').exists():row['throughput']=json.loads((cell/'results.json').read_text())['cells'][0]['throughput_per_second']
 (p/'lock-chains.json').write_text(json.dumps(chains.most_common(),indent=2));rows.append(row)
 print(row['pattern'],row['variant'],'CPU',round(total_cpu,3),'samples',counts['app'],'switch/msg',round(row['switches_msg'],3))
 for g,x in sorted(row['groups_us_msg'].items(),key=lambda x:-x[1]):print(' ',g,round(x,3),samples[g])
(j/('attribution-'+profile_root+'.json')).write_text(json.dumps(rows,indent=2))
