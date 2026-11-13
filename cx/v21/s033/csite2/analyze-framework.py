import json, glob, os, struct, subprocess, collections, re, bisect
J='/root/jobs/fx/cx/v21/s033/csite2/'
CAP=524288; DATA_START=135168; STRIDE=192; FRAME_MAX=20
NAME={0:'Other source threads',1:'Caller',2:'Receive role',3:'Completion workers'}
ROOT='/root/worktree/zlink-3867h'
SOURCE_CACHE={}
SOURCE_TEXT={}
SRCFILES=collections.defaultdict(list)
for base in ['framework/languages/cpp/framework','framework/bench/grpc/cpp','bindings/cpp']:
 for f in glob.glob(ROOT+'/'+base+'/**',recursive=True):
  if os.path.isfile(f): SRCFILES[os.path.basename(f)].append(f)
COUNTERS=['malloc','calloc','realloc','aligned','mutex_lock','cond_signal','rotation_start','rotation_end','completion_post','completion_run','cond_wait','role_mark','wait_return','trylock_ok','cond_broadcast','unused']

def load_maps(pid):
 rows=[]
 for line in open(J+f'maps-{pid}.txt'):
  a=line.rstrip().split(None,5)
  if len(a)<6: continue
  lo,hi=(int(x,16) for x in a[0].split('-')); off=int(a[2],16); path=a[5]
  rows.append((lo,hi,off,path,lo-off))
 rows.sort(key=lambda x:x[0]); return rows

def process(label, side, rawpath, kind):
 d=json.load(open(rawpath)); before=d['before'][side]; after=d['after'][side]
 req=d['stats_after']['source']['submitted'] if side=='source' else d['stats_after']['target']['received']
 if side=='target' and not req: req=d['stats_after']['target']['allPhaseMessages']-d['stats_before']['target']['allPhaseMessages']
 counts=collections.Counter()
 bthreads=before['threads']; athreads=after['threads']
 for tid,aa in athreads.items():
  role=int(aa['role']); bb=bthreads.get(tid,{})
  for i in range(4): counts[(role,'heap')]+=int(aa[COUNTERS[i]])-int(bb.get(COUNTERS[i],0))
  counts[(role,'lock')]+=int(aa['mutex_lock'])-int(bb.get('mutex_lock',0))+int(aa['trylock_ok'])-int(bb.get('trylock_ok',0))
 start=int(before['sample_count']); end=int(after['sample_count'])
 pids=[]; thread_ids=set(athreads)
 for mp in glob.glob(J+'maps-*.txt'):
  pid=os.path.basename(mp)[5:-4]
  if pid not in thread_ids: continue
  try:
   first=open(mp).readline()
   if side=='source' and '/bench_cpp_client' in first: pids.append(pid)
   elif side=='target' and (('/bench_cpp_framework_server' in first) if label.startswith('framework') else ('/bench_cpp_zlink_server' in first)): pids.append(pid)
  except OSError: pass
 if not pids: raise RuntimeError((label,side,'no process maps for thread ids',sorted(thread_ids)[:3]))
 pid=pids[0]
 maps=load_maps(pid); data=open(J+f'count-{pid}.bin','rb').read(); actual=struct.unpack_from('Q',data,16)[0]
 starts=[x[0] for x in maps]
 def findmap(pc):
  i=bisect.bisect_right(starts,pc)-1
  return maps[i] if i>=0 and maps[i][0]<=pc<maps[i][1] else None
 samples=[]; pcs=set()
 for seq in range(start,end):
  ix=seq%CAP; off=DATA_START+ix*STRIDE
  tid,role,k,nf=struct.unpack_from('4Q',data,off)
  if k not in (0,1) or nf<1 or nf>FRAME_MAX: continue
  stack=struct.unpack_from(f'{nf}Q',data,off+32)
  samples.append((role,'lock' if k==0 else 'heap',stack))
  for pc in stack:
   m=findmap(pc)
   if m and ('bench_cpp_client' in m[3] or 'bench_cpp_framework_server' in m[3] or 'bench_cpp_zlink_server' in m[3]): pcs.add((m[3],pc-m[4]-1))
 # Resolve executable frames in batches.
 by_path=collections.defaultdict(list)
 for path,addr in pcs: by_path[path].append(addr)
 resolved={}
 for path,addrs in by_path.items():
  uniq=sorted(set(addrs)); exe=[]
  for i in range(0,len(uniq),600):
   chunk=uniq[i:i+600]
   out=subprocess.run(['addr2line','-f','-C','-e',path]+[hex(x) for x in chunk],capture_output=True,text=True,check=True).stdout.splitlines()
   for n,addr in enumerate(chunk):
    fn=out[n*2].strip() if n*2<len(out) else '??'
    loc=out[n*2+1].strip() if n*2+1<len(out) else '??:0'
    resolved[(path,addr)]=(fn,loc)
 sites=collections.Counter(); samples_by=collections.Counter()
 for role,cat,stack in samples:
  samples_by[(role,cat)]+=1; site=None
  for pc in stack:
   match=None
   m=findmap(pc)
   if m and ('bench_cpp_client' in m[3] or 'bench_cpp_framework_server' in m[3] or 'bench_cpp_zlink_server' in m[3]): match=(m[3],pc-m[4]-1)
   if not match: continue
   fn,loc=resolved.get(match,('??','??:0'))
   fn=re.sub(r'\s+\[clone.*?\]','',fn)
   if not fn.strip() or fn in ('??','main') or fn.startswith(('std::','__gnu_cxx::','boost::','grpc::','absl::','google::','fmt::','operator new','operator delete')): continue
   basename=loc.split(':',1)[0]
   candidates=SRCFILES.get(basename,[])
   if not candidates and fn.startswith(('zlink::framework','zlink::','(anonymous namespace)')):
    allfiles=[src for paths in SRCFILES.values() for src in paths]
    if fn.startswith('zlink::framework'):
     candidates=[src for src in allfiles if '/framework/languages/cpp/framework/' in src or '/framework/bench/grpc/cpp/' in src]
    elif fn.startswith('zlink::detail') or fn.startswith('zlink::'):
     candidates=[src for src in allfiles if '/bindings/cpp/' in src]+[src for src in allfiles if '/framework/' in src]
    else: candidates=[src for src in allfiles if '/framework/bench/grpc/cpp/' in src]+allfiles
   if candidates and fn!='??':
    parts=fn.split('(',1)[0].strip().split()
    if not parts: continue
    prefix=parts[-1]
    leaf=prefix.rsplit('::',1)[-1]
    leaf=re.sub(r'\\{.*','',leaf).strip().split('<',1)[0]
    key=(basename,leaf,fn[:fn.find('(')])
    if key in SOURCE_CACHE: best=SOURCE_CACHE[key]
    else:
     best=None
     if leaf and leaf not in ('operator','operator()'):
      for src in candidates:
       if src not in SOURCE_TEXT:
        try: SOURCE_TEXT[src]=open(src,errors='replace').read()
        except OSError: continue
       lines=SOURCE_TEXT[src].splitlines()
       hits=[i+1 for i,line in enumerate(lines) if leaf in line and '(' in line and not line.lstrip().startswith(('//','*'))]
       if hits:
        best=(os.path.relpath(src,ROOT),hits[0]); break
     if best is None and basename and SRCFILES.get(basename): best=(os.path.relpath(SRCFILES[basename][0],ROOT),None)
     SOURCE_CACHE[key]=best
    if best:
     loc2=best[0]+(':'+str(best[1]) if best[1] else ':line unavailable')
     site=(fn,loc2); break
  if site is None: site=('UNATTRIBUTED (no Framework/binding frame)','?:0')
  sites[(role,cat,site)]+=1
 out={'label':label,'side':side,'pid':pid,'requests':req,'calls':{f'{r}:{c}':v for (r,c),v in counts.items()},'sampled':{f'{r}:{c}':v for (r,c),v in samples_by.items()},'sites':[]}
 for role in sorted({r for r,c in samples_by}):
  for cat in ['lock','heap']:
   total=samples_by[(role,cat)]; exact=counts[(role,cat)]; cov=0
   rows=[]
   for (rr,cc,site),n in sorted(sites.items(),key=lambda x:(x[0][0]!=role,x[0][1]!=cat,-x[1])):
    if rr!=role or cc!=cat: continue
    frac=n/total if total else 0; cov+=n
    rows.append({'role':NAME.get(role,str(role)),'role_id':role,'kind':cat,'site':site[0],'loc':site[1],'sample_n':n,'sample_fraction':frac,'calls_per_request':exact*frac/req if req else 0,'coverage_after':cov/total if total else 0})
    if total and cov/total>=0.80: break
   out['sites']+=rows
 return out

runs=[]
for label,path in [('framework-final',J+'framework-final-counts.json')]:
 for side in ['source','target']:
  runs.append(process(label,side,path,''))
json.dump(runs,open(J+'analysis-final.json','w'),indent=2)
for x in runs:
 print('\n',x['label'],x['side'],'req',x['requests'])
 for r in x['sites']:
  print(f"{r['role']:23} {r['kind']:4} {r['calls_per_request']:8.3f} ({r['coverage_after']:.1%}) {r['site']} | {r['loc']}")
