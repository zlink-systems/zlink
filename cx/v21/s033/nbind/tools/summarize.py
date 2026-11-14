#!/usr/bin/env python3
import collections,json,pathlib,re,subprocess,sys,os
JOB=pathlib.Path('/root/jobs/fx/cx/v21/s033/nbind')
RAW=JOB/'raw'
header=re.compile(r'^(\d+)/(\d+)\s+(\d+\.\d+):\s*(\d+)')
def loads(p):return json.loads(p.read_text())
def counter_delta(start,end):
 a={x['tid']:x for x in start};result=collections.Counter();per=[]
 for x in end:
  d={k:x[k]-a.get(x['tid'],{}).get(k,0) for k in ['malloc','calloc','realloc','free','bytes']}
  assert all(v>=0 for v in d.values()),d
  result.update(d);per.append(dict(tid=x['tid'],**d))
 return dict(result),per

def parse_perf(directory,role,n):
 p=directory/f'{role}-perf-script.txt'
 if not p.exists():
  with p.open('w') as out,(directory/f'{role}-perf-script.log').open('w') as err:
   subprocess.run(['taskset','-c','6-7,14-15','perf','script','-i',str(directory/f'{role}.perf.data'),'-F','pid,tid,time,period,ip,sym,dso','--ns','--no-inline'],stdout=out,stderr=err,check=True,env=dict(os.environ,DEBUGINFOD_URLS='',PERF_BUILDID_DIR=str(RAW/'perf-symbols')))
 task_ms=None
 for line in (directory/f'{role}-perf-stat.txt').read_text().splitlines():
  v=line.split(';')
  if len(v)>2 and v[2]=='task-clock':task_ms=float(v[0])
 assert task_ms is not None
 buckets=collections.Counter();sub=collections.Counter();visible=collections.Counter();leafs=collections.Counter();cpu_threads=collections.Counter();export=collections.Counter();api=collections.Counter();counts=collections.Counter();unknown=0
 current=None;frames=[];sample_count=0
 def flush():
  nonlocal current,frames,sample_count,unknown
  if current is None or not frames:return
  pid,tid,stamp,weight=current;sample_count+=1;cpu_threads[tid]+=weight
  # Exclusive CPU owner: first concrete JS/addon/Core frame in leaf-to-root order.
  # V8/libc helper costs follow their nearest owner; kernel leaf remains kernel.
  owner=None
  if frames[0][1].startswith('[kernel'):owner='kernel'
  else:
   for sym,dso in frames:
    if 'libzlink.so' in dso:owner='libzlink';break
    if dso.endswith('/zlink.node'):owner='addon';break
    if '/perf-' in dso and dso.endswith('.map'):owner='JS_runtime';break
   if owner is None:owner='addon' if any(sym.startswith('napi_') or 'v8impl::' in sym for sym,_ in frames) else 'JS_runtime'
  buckets[owner]+=weight;counts[owner]+=1
  leafs[frames[0][0]+' ('+frames[0][1]+')']+=weight
  if all(dso=='[unknown]' for _,dso in frames):unknown+=weight
  text='\n'.join(sym+' '+dso for sym,dso in frames)
  api_names=[('submit','socket_submit_send('),('submit','socket_submit_request('),('receive','router_try_recv_message('),('receive','router_recv_message('),('reply_submit','socket_reply('),('completion_receive','socket_completion_recv('),('body_from','message_from_buffer('),('data_view','message_frame_data('),('data_copy','message_frame_copy_data('),('message_close','message_frame_close('),('poll_wait','poller_wait_into(')]
  for key,needle in api_names:
   if needle in text:api[key]+=weight;break
  for key,needle in [('envelope','channel-envelope.js'),('service_wire','service-wire-m6a-codec.js'),('binding_js','/dist/zlink/'),('protobuf','protobufjs'),('gc','GarbageCollector')]:
   if needle in text:visible[key]+=weight
  if owner=='JS_runtime':
   if 'channel-envelope.js' in text:sub['envelope']+=weight
   elif 'service-wire-m6a-codec.js' in text:sub['service_wire']+=weight
   elif 'protobufjs' in text or 'bench.proto' in text:sub['protobuf']+=weight
   elif '/dist/zlink/' in text:sub['binding_js']+=weight
   elif any(q in text for q in ['GarbageCollector','CollectGarbage','Scavenger','MarkCompact','Sweeper','Scavenge','ConcurrentMarking']):sub['gc']+=weight
   else:sub['bench_vm_other']+=weight
  if owner=='addon' and any(q in frames[0][0] for q in ['napi_','v8impl::','CallApiCallback']):sub['napi_primitive_leaf']+=weight
  if re.search('memcpy|memmove|CopyBytes|copy_n|mempcpy',frames[0][0]):sub['copy_leaf_'+owner]+=weight
  if re.search('malloc|calloc|realloc|free|operator new|operator delete',frames[0][0]):sub['alloc_leaf_'+owner]+=weight
  for sym,dso in frames:
   if dso.endswith('/zlink.node'):
    export[sym]+=weight;break
  current=None;frames=[]
 with p.open() as f:
  for line in f:
   h=header.match(line)
   if h:
    flush();current=(int(h[1]),int(h[2]),float(h[3]),int(h[4]));continue
   if not line.strip():flush();continue
   if line[0].isspace() and current:
    content=line.strip();a,sep,dso=content.rpartition(' (')
    if sep and dso.endswith(')'):
     frame=a.split(' ',1)
     if len(frame)==2:frames.append((frame[1],dso[:-1]))
 flush()
 total=sum(buckets.values());us=task_ms*1000/n
 def times(c):return {k:dict(weight_ns=v,percent=100*v/total,us_per_message=us*v/total) for k,v in c.items()}
 return dict(messages=n,task_clock_ms=task_ms,total_us_per_message=us,sample_count=sample_count,weighted_sample_ns=total,unknown_percent=unknown/total*100,
             split=times(buckets),js_exclusive=times(sub),visible_inclusive=times(visible),addon_inclusive_frames=times(export),
             api_inclusive=times(api),category_samples=dict(counts),threads=times(cpu_threads),top_leaf=[dict(frame=k,percent=100*v/total,us_per_message=us*v/total) for k,v in leafs.most_common(45)])

def heap_summary(directory,role,n):
 p=loads(directory/f'{role}.heapprofile');rows=[];total=0;groups=collections.Counter()
 def walk(x,parents):
  nonlocal total
  cf=x['callFrame'];label=cf.get('functionName','')+' '+cf.get('url','')+':'+str(cf.get('lineNumber',-1)+1)
  stack=parents+[label];s=x['selfSize'];total+=s;text='\n'.join(stack)
  if 'channel-envelope.js' in text:group='envelope'
  elif 'service-wire-m6a-codec.js' in text:group='service_wire'
  elif '/dist/zlink/' in text:group='binding_js'
  elif 'protobufjs' in text:group='protobuf'
  else:group='bench_vm_other'
  groups[group]+=s
  if s:rows.append(dict(frame=label,bytes=s,group=group,stack=stack[-8:]))
  for c in x.get('children',[]):walk(c,stack)
 walk(p['head'],[])
 return dict(estimated_total_bytes=total,estimated_bytes_per_message=total/n,groups_bytes_per_message={k:v/n for k,v in groups.items()},sample_count=len(p.get('samples',[])),top_nodes=sorted(rows,key=lambda x:x['bytes'],reverse=True)[:45])

summary=[]
paths=[p for p in RAW.iterdir() if p.is_dir() and (p.name.startswith('final-audit-') or p.name.startswith('window-') or p.name.startswith('permit-'))]
if len(sys.argv)>1:paths=[RAW/sys.argv[1]]
for d in sorted(paths):
 if not (d/'events.json').exists():continue
 events=loads(d/'events.json')
 if not any(x['event']=='valid' for x in events):continue
 meta=loads(d/'build.json');source=loads(d/'active-source.json');target=loads(d/'active-target.json');n=source['completed']
 assert n==source['submitted']==target['received'] and source['errors']==target['errors']==0
 item=dict(label=d.name,**meta,messages=n,exits=[e for e in events if e['event']=='exit'],roles={})
 for role in ['source','target']:
  j=loads(d/f'{role}-instrument.json');r={}
  r['control_cpu_us_per_message']=sum(j['ended']['cpu'][k]-j['began']['cpu'][k] for k in ['user','system'])/n
  if meta['mode']=='audit':
   r.update(total_calls_per_message=sum(j['counts'].values())/n,calls_per_message={k:v/n for k,v in j['counts'].items()},callbacks_per_message={k:v/n for k,v in j['callbacks'].items()},bytes_per_message={k:v/n for k,v in j['bytes'].items()},outcomes_per_message={k:v/n for k,v in j['outcomes'].items()})
  if meta['mode']=='cpu':r['cpu']=parse_perf(d,role,n)
  if meta['mode']=='alloc':
   native,threads=counter_delta(loads(d/'alloc-start.json')[role],loads(d/'alloc-end.json')[role]);r['native_per_message']={k:v/n for k,v in native.items()};r['native_per_message']['alloc_calls']=sum(native[k] for k in ['malloc','calloc','realloc'])/n;r['native_threads']=threads;r['heap']=heap_summary(d,role,n)
  item['roles'][role]=r
 (d/'summary.json').write_text(json.dumps(item,indent=2)+'\n');summary.append(item)
(RAW/'per-message-summary.json').write_text(json.dumps(summary,indent=2)+'\n')
for x in summary:
 print(x['label'],x['messages'],*(f"{role}="+str(round(r.get('cpu',{}).get('total_us_per_message',r.get('heap',{}).get('estimated_bytes_per_message',r.get('total_calls_per_message',0))),3)) for role,r in x['roles'].items()))
