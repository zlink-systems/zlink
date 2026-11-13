import pathlib,json,re,collections,sys,importlib.util
J=pathlib.Path(__file__).parent
# Keep this classifier independent of the Inspector's wall-time sampling.
def cat(s):
 s=s.lower()
 if any(x in s for x in ['scaveng','markcompact','garbagecollect','collectgarbage','markingvisitor','incrementalmarking','sweep','minor_gc','major_gc']):return 'GC'
 if any(x in s for x in ['memcpy','memmove','mempcpy','copybytes','copy_data','copydata','copy_array','typedarrayset','typedarrayprototype','buffer::copy','writebytes','copyfastsmi','copyfastobject','copytypedarray','zlinkencodedpayload.js:data']):return 'copy'
 if any(x in s for x in ['malloc','calloc','realloc','allocateyoung','allocateraw','newjsarraybuffer','arraybufferallocator','buffer.alloc','allocatebuffer','createunsafearraybuffer']):return 'allocation'
 if any(x in s for x in ['promise','microtask','async_hooks','resumegenerator']):return 'Promise/microtask'
 if any(x in s for x in ['protobuf','writer','reader','encode','decode','codec','envelope','multipart','channelheader','tryparsecanonical']):return 'encode/serialize'
 if any(x in s for x in ['socket_submit','napi','zlink::','zlink_','/zlink/dist/','/zlink.node','sendto','recvfrom','sendmsg','recvmsg','tcp_sendmsg','tcp_recvmsg','raw_route']):return 'binding/Core'
 if any(x in s for x in ['uv_run','uv__','epoll','futex','processimmediate','setimmediate','listontimeout','__schedule','try_to_wake_up']):return 'handoff/event loop'
 if '/framework/dist/' in s:return 'Framework bookkeeping'
 if '/bench/grpc/node/' in s:return 'benchmark'
 return 'other'
rows=[]
for cap in sorted((J/(sys.argv[1] if len(sys.argv)>1 else 'perf')).glob('run*/*/capture')):
 b=json.loads((cap/'before.json').read_text());a=json.loads((cap/'after.json').read_text())
 for role in ('source','target'):
  pid=a['processes'][role]['pid'];key='completed' if role=='source' else 'received';n=a['stats'][role][key]-b['stats'][role][key];us=(a['processes'][role]['threads'][str(pid)]['runtime_ns']-b['processes'][role]['threads'][str(pid)]['runtime_ns'])/1000/n
  text=(cap/f'{role}.perf.script').read_text(errors='replace');groups=collections.Counter();leafs=collections.Counter();count=0
  for block in text.split('\n\n'):
   lines=block.splitlines()
   if not lines:continue
   m=re.search(r'\s(\d+)/(\d+)\s+[0-9.]+:',lines[0])
   if not m or int(m[2])!=pid:continue
   frames=[re.sub(r'^\s*[0-9a-f]+\s+','',line).strip() for line in lines[1:] if line.strip()]
   if not frames:continue
   count+=1;leaf=frames[0];category=cat(leaf)
   if any(cat(x)=='GC' for x in frames[:12]):category='GC'
   groups[category]+=1;leafs[leaf]+=1
  row={'cell':cap.parent.name,'role':role,'main_us':us,'samples':count,'stages_us':{k:v/max(count,1)*us for k,v in groups.items()},'leafs':[(k,v,v/max(count,1)*us) for k,v in leafs.most_common(65)]};rows.append(row)
  print(row['cell'],role,'main_us',round(us,3),'samples',count)
  print({k:round(v,3) for k,v in row['stages_us'].items()})
  for fn,c,t in row['leafs'][:15]:print(round(t,3),fn)
(J/'perf-summary.json').write_text(json.dumps(rows,indent=2)+'\n')
