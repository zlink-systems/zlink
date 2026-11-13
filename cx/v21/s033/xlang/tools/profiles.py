import collections,json,pathlib,re,subprocess,sys,os
os.environ["DEBUGINFOD_URLS"]=""
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');TRACE='/root/jobs/fx/cx/v21/p032net/tools/dotnet-trace8/dotnet-trace';JFR='/usr/lib/jvm/java-25-openjdk-amd64/bin/jfr'
def load(p):return json.loads(p.read_text())
def logical(frames,side):
 # Helpers belong to the closest owning operation in the leaf-to-root stack.
 all_frames='\n'.join(frames).lower()
 if any(v in all_frames for v in ['xlangpace','xlang_pace','xlangsleep']):return 'pacing'
 if any(v in all_frames for v in ['g1collect','g1young','gc_heap','garbagecollect','garbage collection','(garbage collector)','g1par','gcworker']):return 'GC'
 for frame in frames:
  x=frame.lower()
  if any(v in x for v in ['tryrecv [','tryreceive','socketrecv','rawsocketreceive','recvflags.dontwait']):return 'binding receive/poll'
  if any(v in x for v in ['echo_handler_t','command_handler_t','benchechohandler','benchcommandhandler','echohandler.handle','commandhandler.handle','echohandler:handle','commandhandler:handle']):return 'handler'
  if side=='target' and 'zlink-framework-server/main.js' in x and 'handle [' in x:return 'handler'
  if any(v in x for v in ['completion_signal_t','completiondispatch','completepublicsettlement','completion_owner','completionowner','completemanagedoperation','decoderequestreply','observerequestreply','observedreply','receive<','request_reply_receiver','request_task_access','complete_request','completerequest','operationcompleted','observenativedeadlinereply']):return 'completion'
  if any(v in x for v in ['route_handler_invoker','route_packet_dispatcher','raw_route_port','mesh_node','meshnode','service_wire','servicewire','route_mesh','rawservicemesh','route_dispatch','routedispatch','dispatchnode','dispatchpump','mailbox','execution_gate','executiongate','serviceoperationregistry','dispatch_loop','dispatchloop','raw-service-mesh','raw-mesh-backend','mesh-dispatch']):return 'dispatch/mesh'
  if side=='source' and any(v in x for v in ['benchdrivers','benchmeasurement','bench_cpp','zlink_raw_driver','zlink_framework_driver','source_t::run_phase','bench-core.js','runactive','requestworkers','sendworkers','requestbackpressure','run_tasks']):return 'caller/submit'
  if side=='target' and any(v in x for v in ['bench_zlink','rawbench','withgrpcbench.zlinkraw','raw-server/main','zlinkrawbenchserver','servermetrics.record','pumprequests','pumpcommands']):return 'handler'
 # Generic executor frames alone do not identify the owning operation.
 # Leave them unresolved so managed stacks can refine them, or report other.
 if any(v in all_frames for v in ['protobuf','serialize','deserialize','encode','decode','buffer','memcpy','memmove']):return 'codec/copy'
 if any(v in all_frames for v in ['malloc','calloc','realloc','free','operator new','operator delete','allocate','allocation']):return 'allocation'
 return 'other'

def perf(out):
 p=out/'perf.script'
 if not p.exists():
  with p.open('w') as h:subprocess.run(['perf','script','-i',str(out/'perf.data'),'-F','comm,pid,tid,time,event,ip,sym,dso','--ns','--no-inline'],stdout=h,stderr=open(out/'perf-script.log','w'),check=True)
 samples=[];record=None
 for line in p.open(errors='replace'):
  if not line.strip():
   if record:samples.append(record);record=None
  elif not line[0].isspace():
   m=re.search(r'\s(\d+)/(\d+)\s+([\d.]+):\s+cpu-clock:',line)
   if m:record={'pid':int(m[1]),'tid':int(m[2]),'time':float(m[3]),'comm':line[:m.start()].strip(),'frames':[]}
  elif record:
   m=re.match(r'\s+[a-f\d]+\s+(.+?)\s+\((.*)\)\s*$',line)
   if m:record['frames'].append(m[1])
 if record:samples.append(record)
 return samples

def dotnet(out,side):
 trace=out/(side+'.nettrace');p=out/(side+'.speedscope.json')
 if not p.exists():subprocess.run([TRACE,'convert',str(trace),'--format','Speedscope','-o',str(out/side)],stdout=open(out/(side+'.convert.log'),'w'),stderr=subprocess.STDOUT,check=True)
 x=load(p);names=[v.get('name','?') for v in x['shared']['frames']];samples=[]
 for pro in x['profiles']:
  m=re.search(r'\((\d+)\)',pro.get('name',''));tid=int(m[1]) if m else None;stack=[]
  for e in pro.get('events',[]):
   if e['type']=='O':stack.append((e['frame'],e['at']))
   elif stack:
    frame,at=stack.pop()
    if names[frame]=='CPU_TIME':
     weight=e['at']-at
     if weight>0:samples.append({'tid':tid,'frames':[names[i] for i,t in reversed(stack)],'weight':weight})
 return samples

def jfr(out,side):
 p=out/(side+'.jfr-execution.json')
 if not p.exists():
  with p.open('w') as h:subprocess.run([JFR,'print','--events','jdk.ExecutionSample','--json',str(out/(side+'.jfr'))],stdout=h,check=True)
 x=load(p);samples=[]
 for e in x['recording']['events']:
  v=e['values'];thread=v.get('sampledThread') or {};frames=[]
  for f in (v.get('stackTrace') or {}).get('frames',[]):
   m=f['method'];klass=m['type']['name'].replace('/','.');frames.append(klass+'.'+m['name'])
  samples.append({'tid':thread.get('osThreadId'),'frames':frames,'weight':1})
 return samples

def node(out,side,pid):
 x=load(out/f'node-{pid}.cpuprofile');nodes={n['id']:n for n in x['nodes']};parents={c:n['id'] for n in x['nodes'] for c in n.get('children',[])};samples=[]
 for node,weight in zip(x.get('samples',[]),x.get('timeDeltas',[])):
  frames=[];seen=set();n=node
  while n not in seen and n in nodes:
   seen.add(n);f=nodes[n]['callFrame'];frames.append(f"{f['functionName']} [{f.get('url','')}:{f.get('lineNumber',0)+1}]")
   if n not in parents:break
   n=parents[n]
  if frames and any(v in frames[0].lower() for v in ['(idle)','(program)','(root)']):continue
  samples.append({'tid':pid,'frames':frames,'weight':weight})
 return samples

def analyze(out):
 summary=load(out/'summary.json');job=summary['job'];pids=load(out/'pids.json');a=load(out/'before.json');b=load(out/'after.json');native=perf(out);r={'job':job,'roles':{}}
 for side,pid in pids.items():
  sam=[v for v in native if v['pid']==pid and a['time']<=v['time']<=b['time']];assert sam,('no samples in monotonic capture window',side,out);threads=collections.defaultdict(list)
  for v in sam:threads[v['tid']].append(v)
  managed=[]
  if job['lang']=='java':managed=jfr(out,side)
  if job['lang']=='dotnet':managed=dotnet(out,side)
  if job['lang']=='node':managed=node(out,side,pid)
  bytid=collections.defaultdict(list)
  for v in managed:bytid[v['tid']].append(v)
  cats=collections.Counter();top=collections.Counter();threadroles=[];totalobserved=0
  for t in summary['threads']:
   if t['side']!=side:continue
   tid=t['tid'];cpu=t['cpu_us'];totalobserved+=cpu
   if 'ZLINKbg/IO/' in t['comm']:
    role='Core I/O send' if t['sendto_per_msg']>t['recvfrom_per_msg'] else 'Core I/O receive' if t['recvfrom_per_msg']>0 else 'Core I/O other'
    cats[role]+=cpu;threadroles.append({**t,'categories':{role:cpu}});continue
   samples=threads[tid];k=collections.Counter()
   for v in samples:k[logical(v['frames'],side)]+=1;top[v['frames'][0] if v['frames'] else '[no frame]']+=1
   # Managed stacks refine the managed thread's unclassified portion, without adding CPU.
   if bytid[tid]:
    mk=collections.Counter()
    for v in bytid[tid]:mk[logical(v['frames'],side)]+=v.get('weight',1)
    # Native perf classifies GC/allocator/kernel/native code; replace only unresolved/other.
    native_total=sum(k.values());amount=k.get('other',0)
    if native_total and amount:
     k.pop('other',None)
     for role,w in mk.items():k[role]+=amount*w/sum(mk.values())
    elif not native_total:k=mk
   if not k:k['other']=1
   tc={role:cpu*w/sum(k.values()) for role,w in k.items()}
   cats.update(tc);threadroles.append({**t,'samples':len(samples),'managed_samples':len(bytid[tid]),'categories':tc})
  cats['unobserved threads/tick residual']+=summary['roles'][side]['cpu_us']-totalobserved
  r['roles'][side]={'categories':dict(cats),'native_samples':len(sam),'managed_samples':len(managed),'threads':threadroles,'top_self':top.most_common(35)}
 (out/'profile-summary.json').write_text(json.dumps(r,indent=2));return r

rows=[]
for p in sorted((P/'runs/profile').glob('*/meta.json')):
 if not load(p).get('ok') or (p.parent/'clock-invalid.txt').exists():continue
 try:rows.append(analyze(p.parent));print('profile summarized',p.parent.name,flush=True)
 except Exception as e:print('PROFILE ERROR',p,e,flush=True)
(P/'profile-summary.json').write_text(json.dumps(rows,indent=2))
