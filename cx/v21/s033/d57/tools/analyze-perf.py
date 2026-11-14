import os,sys,json,subprocess,re,bisect,collections
from pathlib import Path
job=Path('/root/jobs/fx/cx/v21/s033/d57')
label=sys.argv[1]
def category(frames):
 for name in frames:
  if 'ZLinkStateLane' in name or 'ZLinkMeshNodeOwnedMailbox' in name or 'ZLinkMeshQueuedRecord' in name or 'ZLinkMeshReadyRegistry' in name: return 'owner queue/mailbox'
  if 'ZLinkScopedHandlerInstanceOwner' in name or 'ServiceLookup' in name or 'CreateScope' in name or 'ServiceProviderEngineScope' in name: return 'activation/DI scope'
  if 'ZLinkHandlerMethodInvoker' in name or 'ZLinkHandlerInvocationEngine' in name or 'ZLinkHandlerResultAwaiter' in name or 'lambda_method' in name: return 'handler invocation'
  if 'ZLinkCompletionDispatcher' in name or 'ZLinkMeshCompletionTable' in name: return 'completion dispatcher'
  if 'PendingOperation' in name or 'Deadline' in name or 'RequestNativeDurableWireAsync' in name or 'CreateOperationAsync' in name: return 'operation registry/deadline'
  if 'SerializerSelection' in name or 'CodecRegistry' in name: return 'serializer lookup'
  if 'System.Text.Json' in name or 'JsonPayloadCodec' in name or 'JsonDocument' in name or 'Base64' in name: return 'typed JSON'
  if 'ExecutionContext' in name or 'AsyncLocal' in name: continue
  if 'ZLinkFrameworkRuntime' in name or 'ApplicationJobQueueInvocation' in name: return 'runtime/permit scope'
  if 'ZLinkProcessExecutionPool' in name or 'ThreadPool' in name or 'WaitSubsystem' in name: return 'worker/threadpool'
  if 'Zlink.Framework' in name: return 'other Framework'
  if 'WithGrpcBench' in name or name.startswith('Program') or 'FrameworkBenchTransport' in name or 'RawBenchTransport' in name: return 'bench'
  if 'Systems.Zlink' in name or 'zlink::' in name or 'zlink_' in name: return 'binding/Core'
 return 'runtime/native/unresolved'
for d in sorted((job/'raw'/label).iterdir()):
 if not d.is_dir() or not (d/'source-stats.json').exists(): continue
 n=json.loads((d/'source-stats.json').read_text())['completed']
 c=json.loads((d/'process-counters.json').read_text()); result={'messages':n,'roles':{}}
 for role in ['source','target']:
  out=d/(role+'-profile.json')
  if not out.exists() and (d/(role+'.nettrace')).exists():
   with out.open('w') as f: subprocess.run(['taskset','-c','6-7,14-15','dotnet',str(job/'tools/analyzer/profile-analyzer.dll'),str(d/(role+'.nettrace')),str(d/'manifest.json')],stdout=f,check=True)
  if not out.exists():
   metrics=json.loads((d/'results.json').read_text())['cells'][0]
   event=dict(gcBytes=metrics['client_allocated_bytes' if role=='source' else 'server_allocated_bytes'],allocationStacks=[],sampleTypes={})
   out.write_text(json.dumps(dict(**event,method='exact benchmark phase allocation counter'),indent=2))
  event=json.loads(out.read_text());b=c['before'][role];e=c['after'][role]
  alloc=collections.Counter()
  for x in event['allocationStacks']: alloc[category(x['stack'].split(' | ',1)[-1].split(' <- '))]+=x['bytes']
  cpu=collections.Counter();leaves=collections.Counter();stacks=collections.Counter();period=0; samples=0;unresolved=0
  perfpath=d/(role+'.perf.data')
  if perfpath.exists():
   sp=d/(role+'-perf-script.txt')
   if not sp.exists():
    with sp.open('w') as f,(d/(role+'-perf-script.log')).open('w') as err:
     subprocess.run(['taskset','-c','6-7,14-15','perf','script','--ns','-i',str(perfpath),'-F','comm,pid,tid,time,period,event,ip,sym,dso'],stdout=f,stderr=err,check=True)
   rows=[]
   for mp in d.glob(role+'-perf-[0-9]*.map'):
    for l in mp.read_text().splitlines():
     try:
      a,z,name=l.split(' ',2);rows.append((int(a,16),int(z,16),name))
     except ValueError:pass
   rows.sort();addrs=[x[0] for x in rows]
   header=None;frames=[]
   def flush():
    global period,samples,unresolved
    if header is None:return
    ts,p=header
    if not b['monotonic']<=ts<=e['monotonic']:return
    period+=p;samples+=1
    cpu[category(frames)]+=p
    if frames: leaves[frames[0]]+=p;stacks[' <- '.join(frames)]+=p
   for l in sp.read_text().splitlines():
    m=re.search(r' (\d+\.\d+):\s+(\d+) cpu-clock:',l)
    if m:
     flush();header=(float(m[1]),int(m[2]));frames=[]
    elif l.startswith('\t'):
     m=re.match(r'\s+([0-9a-f]+) (.+?) \((.+)\)$',l)
     if not m:continue
     addr=int(m[1],16);name=m[2]
     if name=='[unknown]':
      i=bisect.bisect_right(addrs,addr)-1
      if i>=0 and addr<rows[i][0]+rows[i][1]:name=rows[i][2]
     frames.append(name)
   flush()
   (d/(role+'-cpu-stacks.json')).write_text(json.dumps(dict(stacks.most_common()),indent=2))
  result['roles'][role]=dict(cpu_us_per_msg=(e['cpu_ticks']-b['cpu_ticks'])/b['clock_ticks']*1e6/n,allocation_B_per_msg=event['gcBytes']/n,allocation_split_B_per_msg={k:v/n for k,v in alloc.items()},perf_cpu_us_per_msg=period/1000/n,perf_samples=samples,cpu_split_us_per_msg={k:v/1000/n for k,v in cpu.items()},perf_top=[dict(method=k,us_per_msg=v/1000/n) for k,v in leaves.most_common(30)],managed_sample_types=event['sampleTypes'])
 (d/'costs.json').write_text(json.dumps(result,indent=2))
 print(d.name,n)
 for role,r in result['roles'].items():print(role,round(r['cpu_us_per_msg'],2),round(r['allocation_B_per_msg']),r['cpu_split_us_per_msg'])
