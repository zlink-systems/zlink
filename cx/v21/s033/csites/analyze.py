import json,pathlib as P,struct,bisect,subprocess as S,collections as C,re,sys
j=P.Path('/root/jobs/fx/cx/v21/s033/csites');stage=sys.argv[1];run=j/(stage+'-counts-framework-request-backpressure-1');d=json.loads((run/'counts-raw.json').read_text());n=d['stats_after']['source']['completed'];result={'requests':n,'stats_after':d['stats_after'],'roles':{},'sites':{},'sites_exclusive':{},'arena_upstream':{},'sample_coverage':{}}
patterns={
 'R-a':r'service_mailbox_t::has_application_dispatch',
 'R-b':r'mesh_node_host_service_t::dispatch_application',
 '1':r'serializer_registry_t::cached_serializer',
 '2':r'canonical_header_reader_t.*::|header_sax_t::|envelope_codec_t::decode_header|decode_header_bytes',
 '3':r'application_payload_t::from_parts',
 '5':r'decode_application_parts\(.*application_payload_t|decode_application_body|decode_application_part_views.*application_payload_t',
 '6':r'decode_application_part_views|decode_application_parts_bytes.*span',
 '7':r'route_packet_dispatcher_t::dispatch_request',
 'kept_reply_body':r'route_request_completion_t::operator',
 '10':r'channel_reply_writer_t::create_reply_header',
 '12':r'format_utc_deadline|client_call_codec_t::create_envelope',
 '17':r'operation_registry_state_t::register_operation'}
for side in ['source','target']:
 before=d['before'][side];after=d['after'][side];counts=C.Counter()
 for tid,a in after['threads'].items():
  b=before['threads'].get(tid,{});role=a['role'];counts[(role,'locks')]+=sum(a[k]-b.get(k,0) for k in ['mutex_lock','trylock_ok']);counts[(role,'heap')]+=sum(a[k]-b.get(k,0) for k in ['malloc','calloc','realloc','aligned'])
 mapsfile=next(p for p in run.glob('maps-*.txt') if ('bench_cpp_client' if side=='source' else 'bench_cpp_framework_server') in p.read_text().splitlines()[0]);pid=mapsfile.stem.split('-')[1];maps=[]
 for line in mapsfile.read_text().splitlines():
  f=line.split(None,5)
  if len(f)<6:continue
  lo,hi=[int(x,16) for x in f[0].split('-')];maps.append((lo,hi,lo-int(f[2],16),f[5]))
 maps.sort();starts=[m[0] for m in maps]
 def lookup(pc):
  ix=bisect.bisect_right(starts,pc)-1
  if ix<0:return
  m=maps[ix]
  if pc>=m[1] or ('/bench_cpp_' not in m[3] and 'libstdc++.so' not in m[3]):return
  return (m[3],pc-m[2]-1)
 raw=(run/f'count-{pid}.bin').read_bytes();a=before['sample_count'];z=after['sample_count'];result['sample_coverage'][side]={'active_samples':z-a,'retained_samples':min(z-a,524288),'coverage':min(z-a,524288)/(z-a)};a=max(a,z-524288)
 samples=[];addresses=C.defaultdict(set)
 for seq in range(a,z):
  off=135168+(seq%524288)*192;tid,role,kind,nf=struct.unpack_from('4Q',raw,off)
  stack=[]
  for pc in struct.unpack_from(f'{min(nf,20)}Q',raw,off+32):
   p=lookup(pc)
   if p:stack.append(p);addresses[p[0]].add(p[1])
  samples.append((role,'locks' if kind==0 else 'heap',stack))
 cache=run/(side+'-symbols.json')
 if cache.exists():
  resolved={(k.split('\n')[0],int(k.split('\n')[1],16)):tuple(v) for k,v in json.loads(cache.read_text()).items()}
 else:
  resolved={}
 for binary,pcs in addresses.items():
  pcs=sorted(pc for pc in pcs if (binary,pc) not in resolved)
  for k in range(0,len(pcs),500):
   chunk=pcs[k:k+500];lines=S.check_output(['addr2line','-f','-C','-e',binary]+[hex(x) for x in chunk],text=True).splitlines()
   for ix,pc in enumerate(chunk):resolved[(binary,pc)]=(lines[2*ix],lines[2*ix+1])
 cache.write_text(json.dumps({'\n'.join([p,hex(a)]):v for (p,a),v in resolved.items()},indent=2))
 pc_rows={p:frozenset(row for row,pattern in patterns.items() if re.search(pattern,f[0])) for p,f in resolved.items()}
 totals=C.Counter();matches=C.Counter();tops=C.Counter();exclusive=C.Counter();upstream=C.Counter()
 for role,kind,stack in samples:
  totals[(role,kind)]+=1;frames=[resolved.get(p,('??','??:0')) for p in stack]
  selected=set()
  for p in stack:selected.update(pc_rows.get(p,()))
  for row in selected:matches[(row,role,kind)]+=1
  if any('monotonic_buffer_resource::_M_new_buffer' in f[0] or 'new_delete_resource' in f[0] for f in frames):upstream[(role,kind)]+=1
  first=next((f for f in frames if 'zlink::framework' in f[0] and not f[0].startswith(('std::','boost::'))),('UNATTRIBUTED','?:0'));tops[(role,kind,first)]+=1
  for row,pattern in patterns.items():
   if re.search(pattern,first[0]):exclusive[(row,role,kind)]+=1
 result['roles'][side]={str(role):{'locks':counts[(role,'locks')]/n,'heap':counts[(role,'heap')]/n,'locks_calls':counts[(role,'locks')],'heap_calls':counts[(role,'heap')]} for role in sorted({r for r,c in counts})}
 result['sites'][side]={row:{kind:sum(counts[(r,kind)]*matches[(row,r,kind)]/totals[(r,kind)] if totals[(r,kind)] else 0 for r in {r for r,c in counts})/n for kind in ['locks','heap']} for row in patterns}
 result['sites_exclusive'][side]={row:{kind:sum(counts[(r,kind)]*exclusive[(row,r,kind)]/totals[(r,kind)] if totals[(r,kind)] else 0 for r in {r for r,c in counts})/n for kind in ['locks','heap']} for row in patterns}
 result['arena_upstream'][side]={kind:sum(counts[(r,kind)]*upstream[(r,kind)]/totals[(r,kind)] if totals[(r,kind)] else 0 for r in {r for r,c in counts})/n for kind in ['locks','heap']}
 result[side+'_top']=[{'role':r,'kind':c,'function':f[0],'location':f[1],'samples':v,'per_request':counts[(r,c)]*v/totals[(r,c)]/n} for (r,c,f),v in tops.most_common(100)]
(j/(stage+'-analysis.json')).write_text(json.dumps(result,indent=2));print(json.dumps({'requests':n,'roles':result['roles'],'sites':result['sites']},indent=2))
