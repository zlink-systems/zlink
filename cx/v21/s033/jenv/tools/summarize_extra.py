import json,collections,pathlib,datetime,subprocess,re,sys
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/jenv');sys.path.insert(0,str(P/'tools'))
from summarize_bpf import summarize

def maps(p):
 result={}
 for line in p.read_text().splitlines():
  try:d=json.loads(line)
  except ValueError:continue
  if d.get('type') in ('map','hist'):result.update(d['data'])
 return result

for case in ('main','prototype-unpaced','prototype','binding'):
 out=P/'runs'/case
 if not (out/'results.json').exists():continue
 r=json.load(open(out/'results.json'))['cells'][0]
 if not (out/'capture/summary.json').exists():continue
 perf=json.load(open(out/'capture/summary.json'));b=json.load(open(out/'capture/before.json'));a=json.load(open(out/'capture/after.json'))
 n=perf['messages'];extra={'case':case,'throughput':r['throughput_per_second'],'completed':r['completed'],'caller_us':r['jvm_thread_cores']*1e6/r['throughput_per_second'],'kernel':{},'mesh':{},'sockets':[],'decoder':[]}
 sizes={'main':(1435,1443),'prototype':(1092,1092),'prototype-unpaced':(1092,1092),'binding':(1252,1252)}[case]
 for role in ('source','target'):
  samples=json.load(open(out/'capture'/(role+'.samples.json')))
  cycles={}
  for l in (out/'capture'/(role+'.cycles.csv')).read_text().splitlines():
   fields=l.split(',')
   if len(fields)>3 and fields[3]=='cycles:k' and fields[1].isdigit():cycles[fields[0].rsplit('-',1)[-1]]=int(fields[1])
  row=[]
  for tid,t in perf['roles'][role]['threads'].items():
   ss=[s for s in samples if s['tid']==tid and s['event']=='cycles:k' and s['frames']]
   tcp=[s for s in ss if any(re.search(r'^(?:tcp_|__tcp_|inet_|__inet_|ip_|__ip_|skb_|__skb_|sock_|__sock_|sk_|__sk_|__dev_|dev_|netif_|net_rx_|__netif_|__alloc_skb|consume_skb|kfree_skb|__kfree_skb)',f[0]) for f in s['frames'])]
   # All hot I/O threads have one data direction; byte denominator is decoded record * completed requests.
   direction='send' if t['stages'].get('send',0)>t['stages'].get('recv',0) else 'recv'
   record=sizes[0] if (role=='source')==(direction=='send') else sizes[1]
   frac=sum(s['period'] for s in tcp)/sum(s['period'] for s in ss) if ss else 0
   normalized_tcp_cycles=cycles.get(tid,0)*frac*perf['seconds']/10.0
   tcp_self=collections.Counter()
   for sample in tcp:tcp_self[sample['frames'][0][0]]+=sample['period']/(n*record)*cycles.get(tid,0)*perf['seconds']/10.0/max(1,sum(s['period'] for s in ss))
   row.append({'tcp_self_cycles_per_byte':tcp_self.most_common(12),'tid':tid,'comm':t['comm'],'cycles_k':cycles.get(tid),'kernel_samples':len(ss),'tcp_samples':len(tcp),'tcp_kernel_fraction':frac,'direction':direction,'record_bytes':record,'tcp_sampled_cycles':sum(s['period'] for s in tcp),'estimated_tcp_cycles_per_byte':normalized_tcp_cycles/(n*record),'tcp_cycles_per_request':normalized_tcp_cycles/n})
  extra['kernel'][role]=row
  mts=[]
  for tid,t in b['processes'][role]['threads'].items():
   if 'zlink-jvm-raw-m' in t['comm'] and tid in a['processes'][role]['threads']:
    mts.append({'tid':tid,'comm':t['comm'],'cpu_us':(a['processes'][role]['threads'][tid]['runtime_ns']-t['runtime_ns'])/1e3/n})
  extra['mesh'][role]=mts
 m=maps(out/'capture-bpf/bpf.stderr')
 bpf=json.load(open(out/'capture-bpf/summary.json'))
 extra['syscalls']=[]
 for role in ('source','target'):
  pid=bpf['roles'][role]['pid']
  for row in bpf['roles'][role]['rows']:
   if not row['io'] or not row['tcp'] or row['syscall'] not in ('sendto','recvfrom'):continue
   if not any(row['tcp'][x].endswith(':5254') or row['tcp'][x].endswith(':5247') for x in ('local','remote')):continue
   c=row['counts'];key=f"{pid},{row['tid']},{row['fd']},{44 if row['syscall']=='sendto' else 45}"
   hist=m.get('@sizes',{}).get(key,[])
   extra['syscalls'].append({'role':role,**row,'mean_positive_bytes':c.get('positive_return_sum',0)/c.get('positive',1),'histogram':hist})
 for key,count in m.get('@socket_options',{}).items():
  fields=list(map(int,key.split(',')));pid,tid,sport,dport,snd,rcv,nonagle=fields[:7];userlocks=fields[7] if len(fields)>7 else None
  if 5247 not in (sport,dport) and 5254 not in (sport,dport):continue
  extra['sockets'].append({'pid':pid,'tid':tid,'sport':sport,'dport':dport,'SO_SNDBUF':snd,'SO_RCVBUF':rcv,'TCP_NODELAY':int(bool(nonagle&1)),'nonagle':nonagle,'sk_userlocks':userlocks,'samples':count})
 if (out/'capture-decoder/bpf.stderr').exists():
  m=maps(out/'capture-decoder/bpf.stderr')
  for key,count in m.get('@decoder_result',{}).items():
   pid,tid,size,fit,typ=map(int,key.split(','));extra['decoder'].append({'pid':pid,'tid':tid,'size':size,'allocator_fit':fit,'storage_type':typ,'count':count})
 if (out/'source.jfr').exists():
  cache=out/'source.execution.json'
  if not cache.exists():
   with cache.open('w') as f:subprocess.run(['/usr/lib/jvm/java-25-openjdk-amd64/bin/jfr','print','--events','jdk.ExecutionSample','--json',str(out/'source.jfr')],stdout=f,check=True)
  wall=b['realtime']+(r['active_started_at_ns']/1e9-b['monotonic']);end=wall+r['active_elapsed_ms']/1000
  counts=collections.Counter();frames=collections.Counter();descriptor=0
  for event in json.load(open(cache))['recording']['events']:
   v=event['values'];thr=v.get('sampledThread',{}) or {};name=thr.get('javaName','');stamp=datetime.datetime.fromisoformat(v['startTime']).timestamp()
   if name!='zlink-jvm-raw-mesh-bench' or not wall<=stamp<=end:continue
   pairs=[(f.get('method',{}).get('type',{}).get('name',''),f.get('method',{}).get('name','')) for f in (v.get('stackTrace') or {}).get('frames',[])]
   group='other'
   for cls,method in pairs:
    if 'ZLinkServiceOperationRegistry' in cls and method=='take':group='registry take';break
    if 'ZLinkJavaRawMeshNode' in cls and method=='decodeRequestReply':group='reply decode';break
    if 'ZLinkServiceM6AWireCodec' in cls and method=='decodeMultipartView':group='multipart view decode';break
    if ('ZLinkServiceOperationRegistry' in cls and method=='complete') or 'ZLinkServiceCompletionDispatcher' in cls or 'CompletionOwner' in cls or ('CompletableFuture' in cls and method in ('complete','postComplete','uniWhenComplete')):group='completion handoff';break
   if any('ZLinkChannelEnvelope' in cls and method=='decodeNative' for cls,method in pairs):descriptor+=1
   counts[group]+=1
   if pairs:frames['.'.join(pairs[0])]+=1
  total=sum(counts.values());meshcpu=sum(x['cpu_us'] for x in extra['mesh']['source'])
  extra['mesh_jfr']={'samples':total,'counts':dict(counts),'us':{k:meshcpu*counts[k]/total if total else 0 for k in ('registry take','reply decode','multipart view decode','completion handoff','other')},'descriptor_inclusive_samples':descriptor,'top':frames.most_common(20)}
 (out/'extra-summary.json').write_text(json.dumps(extra,indent=2));print(case,extra['throughput'],extra['caller_us'],extra['mesh'],extra.get('mesh_jfr',{}).get('us'))
