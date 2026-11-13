import pathlib,json,collections,re,sys,subprocess
J=pathlib.Path(__file__).parent
name=sys.argv[1] if len(sys.argv)>1 else 'cpu'
CATS=['Framework bookkeeping','encode/serialize','Buffer copy','allocation','binding call','Promise/microtask','handoff/event loop','GC','benchmark','other']
def classify(fn,url=''):
 s=(fn+' '+url).lower()
 if any(x in s for x in ['garbage collector','scavenge','collectgarbage','markcompact','garbagecollection','minor_gc','major_gc']):return 'GC'
 if any(x in s for x in ['memcpy','memmove','mempcpy','copybytes','copy_data','copydata','copy_array','typedarrayset','typedarrayprototype','buffer::copy','writebytes','uint8array::set','copyactual','copyfastsmi','copyfastobject','copytypedarray']):return 'Buffer copy'
 if fn=='data' and 'ZLinkEncodedPayload' in url:return 'Buffer copy'
 if any(x in s for x in ['malloc','calloc','realloc','allocateyoung','allocateraw','newjsarraybuffer','arraybufferallocator','buffer.alloc','allocatebuffer','createunsafearraybuffer']):return 'allocation'
 if fn in ('alloc','allocUnsafe','allocUnsafeSlow','createUnsafeBuffer','allocate') and 'node:' in url:return 'allocation'
 if fn in ('from','fromArrayLike','fromArrayBuffer','fromObject','copy','copyImpl','set','_set','slice','subarray','getString') and ('buffer' in url or 'typedarray' in url):return 'Buffer copy' if fn not in ('subarray','fromArrayBuffer','getString') else 'allocation'
 if any(x in s for x in ['promise','async_hooks','asynclocal','microtask','asyncfunction','await','resumegenerator']):return 'Promise/microtask'
 if any(x in s for x in ['protobuf','writer','reader','encode','decode','codec','envelope','multipart','channelheader','tryparsecanonical','bufferutf8write','utf8slice']):return 'encode/serialize'
 if any(x in s for x in ['socket_submit','napi','zlink::','zlink_','/zlink/dist/','/zlink.node','native\n','sendto','recvfrom','sendmsg','recvmsg','tcp_sendmsg','tcp_recvmsg','sys_sendto','sys_recvfrom','raw_route']):return 'binding call'
 if any(x in s for x in ['uv_run','uv__','uvqueue','epoll','processimmediate','setimmediate','processnexttick','runtimer','listontimeout']):return 'handoff/event loop'
 if '/framework/dist/' in url:return 'Framework bookkeeping'
 if '/bench/grpc/node/' in url:return 'benchmark'
 return 'other'
def cpustats(cap,role):
 b=json.loads((cap/'before.json').read_text());a=json.loads((cap/'after.json').read_text());p=a['processes'][role]['pid'];k='completed' if role=='source' else 'received';n=a['stats'][role][k]-b['stats'][role][k];bt=b['processes'][role]['threads'];at=a['processes'][role]['threads'];us=(at[str(p)]['runtime_ns']-bt[str(p)]['runtime_ns'])/1000/n;return n,us,p
allrows=[]
for cap in sorted((J/name).glob('run*/*/capture')):
 for role in ('source','target'):
  n,main,pid=cpustats(cap,role);f=cap/f'{role}.cpuprofile'
  if not f.exists():continue
  cp=json.loads(f.read_text());nodes={x['id']:x for x in cp['nodes']};parents={c:i for i,x in nodes.items() for c in x.get('children',[])};weights=collections.Counter();samples=collections.Counter()
  for i,dt in zip(cp['samples'],cp['timeDeltas']):weights[i]+=dt;samples[i]+=1
  incl={}
  def inclusive(i):
   incl[i]=weights[i]+sum(inclusive(c) for c in nodes[i].get('children',[]));return incl[i]
  for i in nodes:
   if i not in parents:inclusive(i)
  nonidle=sum(v for i,v in weights.items() if nodes[i]['callFrame']['functionName'] not in ('(idle)','(root)'))
  groups=collections.Counter();methods=collections.defaultdict(lambda:[0,0,0]);unknown=collections.Counter()
  for i,x in nodes.items():
   fr=x['callFrame'];fn=fr['functionName'];url=fr.get('url','');cat=classify(fn,url)
   # Inspector represents native API bodies as unnamed leaves. Attribute only
   # these leaves to the nearest named caller; retain unresolved program time.
   if cat=='other' and fn=='' and url=='':
    ancestor=parents.get(i)
    while ancestor is not None:
     frame=nodes[ancestor]['callFrame'];caller=classify(frame['functionName'],frame.get('url',''))
     if caller!='other':cat=caller;break
     ancestor=parents.get(ancestor)
   if fn in ('(idle)','(root)'):continue
   groups[cat]+=weights[i]/nonidle*main
   label=f'{url}:{fr.get("lineNumber",-1)+1}:{fn}'
   methods[label][0]+=weights[i]/nonidle*main;methods[label][1]+=incl[i]/nonidle*main;methods[label][2]+=samples[i]
   if cat=='other':unknown[label]+=weights[i]/nonidle*main
  row={'cell':cap.parent.name,'role':role,'messages':n,'main_us':main,'stages_us':dict(groups),'methods':sorted([{'name':k,'self_us':v[0],'inclusive_us':v[1],'samples':v[2]} for k,v in methods.items()],key=lambda v:-v['self_us']),'unknown':unknown.most_common(30),'sample_count':len(cp['samples']),'profile_wall_us':sum(cp['timeDeltas']),'nonidle_us':nonidle};allrows.append(row)
  print(cap.parent.name,role,'main',round(main,3),'msg',n,'samples',len(cp['samples']))
  print(' | '.join(f'{k}={groups[k]:.3f}' for k in CATS))
  for x in row['methods'][:20]:print(f'{x["self_us"]:.3f} self {x["inclusive_us"]:.3f} incl',x['name'])
(J/(name+'-profile-summary.json')).write_text(json.dumps(allrows,indent=2)+'\n')
