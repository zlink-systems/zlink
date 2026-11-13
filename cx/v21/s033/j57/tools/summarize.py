import collections,json,subprocess,sys,os,re
from pathlib import Path
base=Path(sys.argv[1]); phases=json.loads((base/'phases.json').read_text())
active=next(p['epochMs'] for p in phases if p['phase']=='active')
duration=int(sys.argv[2]) if len(sys.argv)>2 else 4000
end=json.loads((base/'active-end.json').read_text())
life=json.loads((base/'lifecycle.json').read_text())
ap=next(p for p in phases if p['phase']=='active')
endEpoch=end.get('epochMs', ap['epochMs']+(life['startMonotonicUs']+end['time']*1e6-ap['monotonicUs'])/1000)
duration=round(endEpoch-active)
java='/usr/lib/jvm/java-25-openjdk-amd64/bin/java'
converter='/root/jobs/fx/cx/v21/s033/l2bp/tools/jfr-converter-4.6.jar'
categories={
 'owner_queue_mailbox':['ZLinkStateLane','ZLinkSerialExecutionQueue','ZLinkHandlerDispatch','ZLinkProcessExecutionLanes','ZLinkApplicationJobContext','ZLinkApplicationJobQueue'],
 'handler_scope_activation':['ZLinkChannelHandlerInvoker','ZLinkHandlerMethodInvoker','ZLinkHandlerInstanceOwner','ZLinkHandlerActivator','SpringActivation','SharedSpringActivation','ZLinkSpringHandlerFactory'],
 'di_resolution':['.getBean', '.doGetBean','.resolveDependency','BeanFactory','ConstructorPlan','ConstructionPlan'],
 'operation_registry':['ZLinkServiceOperationRegistry','ZLinkServiceOperationIds'],
 'completion_dispatch':['ZLinkServiceCompletionDispatcher','ZLinkCompletionBridge'],
 'serializer_lookup':['ZLinkCodecRegistration','ZLinkJsonMessageSerializer$','ZLinkJsonMessageSerializer.prepare'],
 'typed_json':['ZLinkJsonMessageSerializer','Jackson','jackson','ObjectReader','ObjectWriter','JsonGenerator','JsonParser','BeanSerializer','BeanDeserializer','ByteArraySerializer','ByteArrayDeserializer'],
 'envelope':['ZLinkChannelEnvelope'],
 'thread_schedule':['VirtualThread','Continuation','ForkJoinPool','ForkJoinTask','LockSupport.park','Unsafe.park','Unsafe.unpark','futex','pthread_cond','sched_yield'],
}
tag=sys.argv[3] if len(sys.argv)>3 else 'full'
result={}
for role in ['source','target']:
 result[role]={}
 stats=json.loads((base/f'active-{role}-stats.json').read_text())
 count=stats['completed'] if role=='source' else stats['received']
 result[role]['messages']=count
 for kind in ['cpu','alloc','wall']:
  out=base/f'{role}-{kind}-{tag}.collapsed'
  cmd=['taskset','-c','6-7,14-15',java,'-jar',converter,'--'+kind,'--total','--threads','--simple','--lines',
       '--from',str(int(active)),'--to',str(int(active)+duration),'-o','collapsed',str(base/f'{role}.jfr'),str(out)]
  proc=subprocess.run(cmd,stdout=subprocess.PIPE,stderr=subprocess.STDOUT,text=True)
  if proc.returncode: raise RuntimeError(proc.stdout)
  selfs=collections.Counter(); inclusive=collections.Counter(); threads=collections.Counter(); total=0
  costs=collections.Counter(); selfCosts=collections.Counter(); classes=collections.Counter()
  for line in out.read_text().splitlines():
   stack,n=line.rsplit(' ',1); n=int(n); frames=stack.split(';'); total+=n
   threads[frames[0]]+=n; selfs[frames[-1]]+=n
   for frame in set(frames): inclusive[frame]+=n
   for key,patterns in categories.items():
    if any(pattern in stack for pattern in patterns):costs[key]+=n
    if any(pattern in frames[-1] for pattern in patterns):selfCosts[key]+=n
   if kind=='alloc':classes[frames[-1]]+=n
  unit=1 if kind=='alloc' else (1000 if kind=='cpu' else 10000)
  result[role][kind]={'total':total,'perMessage':total*unit/count if count else None,
    'unit':'bytes/message' if kind=='alloc' else 'sampled microseconds/message (thread-summed)',
    'categoryInclusive':{k:{'weight':v,'percent':100*v/total if total else 0,'perMessage':v*unit/count if count else None} for k,v in costs.items()},
    'categorySelf':dict(selfCosts),'self':selfs.most_common(50),'threads':threads.most_common(40),
    'inclusive':inclusive.most_common(100),'classes':classes.most_common(50)}
  print(role,kind,'total',total,'perMessage',round(total*unit/count,3) if count else None)
  print('SELF',selfs.most_common(14))
  print('THREADS',threads.most_common(8))
  print('CATEGORIES',dict(costs))
(base/f'summary-{tag}.json').write_text(json.dumps(result,indent=2))
