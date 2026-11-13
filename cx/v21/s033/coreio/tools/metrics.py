from pathlib import Path
import json,collections
J=Path('/root/jobs/fx/cx/v21/s033/coreio')
def key(cap):
 name=Path(cap).parent.name
 language='java' if '-java-' in name else 'cpp'
 impl='Framework' if name.startswith('zlink-framework-') else 'binding'
 pattern='request-backpressure' if 'request-backpressure' in name else 'send-saturation'
 return language,pattern,impl
wire={}
for r in json.load(open(J/'wire-summary.json')):
 if r['role']!='source':continue
 k=key(r['capture'])
 w={}
 for direction in ('send','recv'):
  hs=[(h['count'],h,s) for s in r['streams'] if s['direction']==direction for h in s['record_hist'] if h['wire_bytes']>1000]
  if hs:
   _,h,s=max(hs,key=lambda x:x[0]);w[direction]={'part_sizes':h['part_sizes'],'wire_bytes':h['wire_bytes'],'kinds':h['kinds'],'decoded_records':s['records'],'fd':s['fd']}
 wire[k]=w
perf={key(r['capture']):r for r in json.load(open(J/'perf-summary.json'))}
bpf={}
for r in json.load(open(J/'bpf-summary.json')):
 if r['mode']!='bpf':continue
 k=key(r['capture']);stamp=json.load(open(Path(r['capture'])/'after.json'))['monotonic']
 if k not in bpf or stamp>bpf[k][0]:bpf[k]=stamp,r
result=[]
for k in sorted(perf):
 if k not in wire or k not in bpf:continue
 p=perf[k];b=bpf[k][1];w=wire[k]
 sendrows=[x for x in b['roles']['source']['rows'] if x['io'] and x['syscall']=='sendto'];byfd=collections.Counter()
 for row in sendrows:byfd[row['fd']]+=row['counts'].get('positive_return_sum',0)
 fd,forwardbytes=byfd.most_common(1)[0];nwire=forwardbytes/w['send']['wire_bytes']
 r={'language':k[0],'pattern':k[1],'implementation':k[2],'wire':w,'perf_capture':p['capture'],'counter_capture':b['capture'],'counter_stats_messages':b['messages'],'wire_message_estimate':nwire,'forward_fd':fd,'paired_epoll':b.get('paired_epoll',False),'roles':{}}
 for role in ('source','target'):
  pr=p['roles'][role];br=b['roles'][role];cpu=pr['io_cpu_us_per_message'];hotid,h=max(pr['threads'].items(),key=lambda x:x[1]['cpu_us_per_message']);counts={g:{} for g in ('io','other')}
  for g in ('io','other'):
   for sc,v in br['totals'][g].items():counts[g][sc]={f:count/nwire for f,count in v['counts'].items()}
  efd={g:collections.Counter() for g in ('io','other')};writers=[]
  for row in br['rows']:
   if row['syscall'] not in ('read','write'):continue
   fdtype=row.get('fdlink','') or ''
   if fdtype!='anon_inode:[eventfd]' and not fdtype.startswith('pipe:'):continue
   g='io' if row['io'] else 'other';efd[g][row['syscall']]+=row['counts'].get('calls',0)
   efd[g][row['syscall']+'_success']+=row['counts'].get('positive',0)
   efd[g][row['syscall']+'_eagain']+=row['counts'].get('ret_-11',0)
   writers.append({'tid':row['tid'],'comm':row['comm'],'io':row['io'],'fd':row['fd'],'fdlink':row['fdlink'],'syscall':row['syscall'],'calls':row['counts'].get('calls',0),'per_message':row['counts'].get('calls',0)/nwire})
  io_epolls={x['fd'] for x in br['rows'] if x['io'] and x['syscall']=='epoll_wait'}
  reactor_ctl_other=sum(x['counts'].get('calls',0) for x in br['rows'] if not x['io'] and x['syscall']=='epoll_ctl' and x['fd'] in io_epolls)/nwire
  r['roles'][role]={'core_reactor_ctl_other':reactor_ctl_other,'io_cpu_us_per_message':cpu,'hot_tid':hotid,'hot_cpu_us_per_message':h['cpu_us_per_message'],'hot_cores':h['cores'],'cpu_stages':pr['io_stage_us_per_message'],'threads':pr['threads'],'calls_per_message':counts,'eventfd_per_message':{g:{f:count/nwire for f,count in c.items()} for g,c in efd.items()},'eventfd_threads':sorted(writers,key=lambda x:x['calls'],reverse=True),'connections':{fd:v['tcp'] for fd,v in br['fds'].items() if v.get('tcp',{}).get('state')=='01' and (int(v['tcp']['remote'].rsplit(':',1)[1]) in (5287,5288,5294,5247,5248,5254) or int(v['tcp']['local'].rsplit(':',1)[1]) in (5287,5288,5294,5247,5248,5254))}}
 result.append(r)
(J/'metrics.json').write_text(json.dumps(result,indent=2))
models=[]
for language,pattern in sorted(set((x['language'],x['pattern']) for x in result)):
 pair={r['implementation']:r for r in result if r['language']==language and r['pattern']==pattern}
 if len(pair)!=2:continue
 f=pair['Framework'];b=pair['binding'];m={'language':language,'pattern':pattern,'roles':{}}
 for role in ('source','target'):
  fr=f['roles'][role];br=b['roles'][role];removal={};fractions={}
  for stage,sc in [('send','sendto'),('recv','recvfrom'),('write','write'),('epoll_wait','epoll_wait')]:
   fc=fr['calls_per_message']['io'].get(sc,{}).get('calls',0);bc=br['calls_per_message']['io'].get(sc,{}).get('calls',0)
   ratio=1.0
   if stage in ('send','recv'):
    direction='send' if (stage=='send' and role=='source') or (stage=='recv' and role=='target') else 'recv'
    if direction not in f['wire'] or direction not in b['wire']:continue
    ratio=f['wire'][direction]['wire_bytes']/b['wire'][direction]['wire_bytes']
   fraction=max(0,1-bc*ratio/fc) if fc else 0;fractions[stage]=fraction
   removal[stage]=fr['cpu_stages'].get(stage,0)*fraction
  ts={tid:sum(v['stages'].get(s,0)*q for s,q in fractions.items()) for tid,v in fr['threads'].items()}
  hot=fr['hot_tid'];m['roles'][role]={'removal_by_stage':removal,'aggregate_model_us':sum(removal.values()),'existing_hottest_thread_model_us':ts[hot],'max_thread_model_us':max(ts.values()),'thread_model_us':ts,'f_io_cpu':fr['io_cpu_us_per_message'],'b_io_cpu':br['io_cpu_us_per_message'],'excess_cpu':fr['io_cpu_us_per_message']-br['io_cpu_us_per_message']}
 models.append(m)
(J/'savings-model.json').write_text(json.dumps(models,indent=2))
for r in result:print(r['language'],r['pattern'],r['implementation'],'wire',r['wire']['send']['wire_bytes'],'Nwire',round(r['wire_message_estimate']),'IO CPU',[(role,round(r['roles'][role]['io_cpu_us_per_message'],2)) for role in ('source','target')])
for m in models:print('model',m['language'],m['pattern'],{role:(round(v['aggregate_model_us'],2),round(v['existing_hottest_thread_model_us'],2)) for role,v in m['roles'].items()})
