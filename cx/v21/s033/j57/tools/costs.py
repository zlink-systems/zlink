import json,collections,sys,re
from pathlib import Path
base=Path(sys.argv[1]); duration=int(sys.argv[2]); tag=sys.argv[3] if len(sys.argv)>3 else None; out_tag=sys.argv[4] if len(sys.argv)>4 else tag; result={}
patterns=[
('typed_serializer_lookup',('ZLinkCodecRegistration.',)),
('typed_json',('ZLinkJsonMessageSerializer.','ZLinkFrameworkJson','com.fasterxml','JsonGenerator.','JsonParser.','UTF8JsonGenerator.','UTF8StreamJsonParser.','ObjectWriter.','ObjectReader.','BeanSerializer.','BeanDeserializer.','ByteArraySerializer.','ByteArrayDeserializer.','Base64Variant.')),
('di_resolution',('BeanFactory.','BeanFactoryUtils.','AbstractAutowireCapableBeanFactory.','AbstractBeanFactory.','DefaultListableBeanFactory.','ZLinkSpringHandlerFactory.constructHandler','ZLinkSpringHandlerFactory$SharedSpringActivation.create','PublicConstructorPlan.','ConstructionPlan.')),
('handler_activation_scope',('ZLinkHandlerInstanceOwner.','ZLinkHandlerActivator','ZLinkSpringHandlerFactory','ZLinkChannelHandlerInvoker.','ZLinkHandlerMethodInvoker')),
('operation_registry',('ZLinkServiceOperationRegistry','ZLinkServiceOperationIds')),
('completion_dispatch',('ZLinkServiceCompletionDispatcher',)),
('owner_queue_mailbox',('ZLinkStateLane.','ZLinkStateLane$','ZLinkSerialExecutionQueue.','ZLinkHandlerDispatch.','ZLinkApplicationJobContext','ZLinkApplicationJobQueue.','ZLinkProcessExecutionLanes')),
('envelope',('ZLinkChannelEnvelope.', 'ZLinkChannelEnvelope$')),
('thread_schedule',('VirtualThread.','Continuation.','ForkJoinPool.','ForkJoinTask.','LockSupport.','Unsafe.park','Unsafe.unpark')),
]
for role in ['source','target']:
 stats=json.loads((base/f'active-{role}-stats.json').read_text()); count=stats['completed'] if role=='source' else stats['received']
 result[role]={'messages':count,'costs':{}}
 for kind in ['cpu','alloc','wall']:
  c=collections.Counter(); raw=collections.Counter(); total=0; compilegc=0; awaited=0
  for line in (base/f'{role}-{kind}-{tag}.collapsed' if tag else f'{role}-{kind}.collapsed').read_text().splitlines():
   stack,n=line.rsplit(' ',1); n=int(n); frames=stack.split(';')
   if any(x in frames[0] for x in ('Compiler','GC Thread','VM Thread','G1 ')):
    compilegc+=n; continue
   if kind=='alloc' and 'SourceMetrics.<init>' in stack: continue
   if kind=='wall' and not any(x in stack for x in ('ZLink','RawStack','BenchDrivers','CompletionOwner','NativePoller','NativeRouter')):continue
   if 'ZLinkStateLane.awaitTurn:136' in stack: awaited+=n
   key='other'
   for frame in reversed(frames[1:]):
    found=False
    for category, pats in patterns:
     if any(pat in frame for pat in pats):
      key=category; found=True;break
    if found:break
   if kind=='wall' and any(x in stack for x in ('ZLinkStateLane.awaitTurn','ZLinkStateLane.awaitResult')):key='owner_queue_mailbox'
   c[key]+=n;total+=n
  unit=1 if kind=='alloc' else 1000 if kind=='cpu' else 10000
  result[role]['costs'][kind]={'totalWeight':total,'backgroundWeight':compilegc,'ownerWaitWeight':awaited,
    'totalPerMessage':total*unit/count if count else None,
    'exclusive':{k:{'weight':v,'perMessage':v*unit/count if count else None,'percent':100*v/total if total else 0} for k,v in c.items()}}
(base/f'costs-{out_tag}.json' if out_tag else base/'costs.json').write_text(json.dumps(result,indent=2))
for role,v in result.items():
 print(role,'messages',v['messages'])
 for kind,c in v['costs'].items():
  print(kind,round(c['totalPerMessage'],3) if c['totalPerMessage'] else None,'background',c['backgroundWeight'],'ownerWait',c['ownerWaitWeight'])
  print({k:round(v['perMessage'],3) for k,v in c['exclusive'].items()})
