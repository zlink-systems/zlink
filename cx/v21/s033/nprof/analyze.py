import json, re, pathlib, glob, os, collections
R=pathlib.Path('/root/jobs/fx/cx/v21/s033/nprof')
CAPS={}
for cap in R.glob('prof-*/run1/*/capture'):
    meta=json.loads((cap/'meta.json').read_text()); name=cap.parent.name
    impl='framework' if 'framework-node' in name else 'binding'
    pattern='send' if 'send-saturation' in name else 'serial'
    b=json.loads((cap/'before.json').read_text()); a=json.loads((cap/'after.json').read_text()); elapsed=a['monotonic']-b['monotonic']
    for role in ('source','target'):
        msgs=a['stats'][role].get('completed',a['stats'][role].get('received',0))-b['stats'][role].get('completed',b['stats'][role].get('received',0))
        if role=='target': msgs=a['stats'][role].get('received',a['stats'][role].get('completed',0))-b['stats'][role].get('received',b['stats'][role].get('completed',0))
        bt=b['processes'][role]['threads']; at=a['processes'][role]['threads']; trows=[]
        for tid,x in at.items():
            if tid not in bt: continue
            y=bt[tid]; cpu=(x['runtime_ns']-y['runtime_ns'])/1000
            ctx=x['voluntary']+x['involuntary']-y['voluntary']-y['involuntary']
            if cpu>50: trows.append((tid,x['comm'],cpu,ctx))
        proc_cpu=sum(x[2] for x in trows); proc_ctx=sum(x[3] for x in trows)
        main=next((x for x in trows if int(x[0])==meta['pids'][role]),None)
        proc={'elapsed':elapsed,'messages':msgs,'cpu':proc_cpu/max(msgs,1),'share':proc_cpu/1e6/elapsed,'ctx':proc_ctx/max(msgs,1),'main_cpu':main[2]/max(msgs,1) if main else 0,'main_share':main[2]/1e6/elapsed if main else 0,'main_ctx':main[3]/max(msgs,1) if main else 0,'threads':trows,'capture':cap,'file':cap.parent.name+'-'+role+'-process-full.txt'}
        CAPS[(pattern,impl,role)]=proc

def cat(s):
    l=s.lower()
    if any(k in l for k in ('scavenge','markcompact','garbagecollection','collectgarbage','allocateyoung','allocateraw','newjsarraybuffer','createunsafearraybuffer','arraybufferallocator','malloc','calloc','realloc','_int_malloc','malloc_consolidate','buffer.alloc')): return 'alloc/GC'
    if any(k in l for k in ('promise','async','await','resumegenerator','promiseinit','promisebefore','promiseafter')): return 'Promise/async'
    if any(k in l for k in ('queueMicrotask','complet','callback','dispatch','epoll_wait','uv_run','runapplicationworker','receivebatch','processreceived')): return 'handoff/wake'
    if any(k in l for k in ('protobuf','writer','reader','encode','decode','codec','envelope','multipart','channelheader','tryparsecanonical')): return 'codec'
    if any(k in l for k in ('socket_submit','napi','zlink::','zlink_','sendto','recvfrom','sendmsg','recvmsg','tcp_sendmsg','tcp_recvmsg','sys_sendto','sys_recvfrom','raw_route')): return 'binding/Core/kernel'
    if '/framework/dist/' in l or '/framework-codec-' in l: return 'Framework bookkeeping'
    if '/bench/grpc/node/' in l: return 'bench driver'
    return 'other'

def entries(path):
    out=[]
    for line in open(path,errors='replace'):
        m=re.match(r'\s*([0-9.]+)%\s+\[[^]]+\]\s+(.*?)\s+-\s+-\s*$',line)
        if m: out.append((float(m.group(1)),m.group(2).strip()))
    return out

print('PROFILE SUMMARY')
for pat in ('send','serial'):
    print('\n',pat)
    for impl in ('framework','binding'):
        for role in ('source','target'):
            p=CAPS[(pat,impl,role)]; report=R/(p['capture'].parent.name+'-'+role+'-full.txt')
            es=entries(report); groups=collections.defaultdict(float)
            for pct,s in es: groups[cat(s)]+=pct
            print(impl,role,'cpu',round(p['cpu'],2),'main',round(p['main_cpu'],2),'ctx',round(p['ctx'],3),'mainctx',round(p['main_ctx'],3),'share',round(p['main_share'],3),'percentSum',round(sum(x for x,_ in es),2))
            print(' stages',', '.join(f'{k}={p["cpu"]*v/100:.2f}' for k,v in groups.items()))
            # allocation sampler
            hfile=R/f'heap-{"framework" if impl=="framework" else "binding"}-{"send" if pat=="send" else "serial"}'/'run1'/f'{"zlink-framework-node" if impl=="framework" else "zlink-node"}-{"send-saturation" if pat=="send" else "request-serial"}-4096'/'capture'/f'{role}.heapprofile'
            hx=json.load(open(hfile)); sizes=sum(x['size'] for x in hx['samples'])
            hb=json.load(open(str(hfile.parent/'before.json'))); ha=json.load(open(str(hfile.parent/'after.json')))
            heap_msgs=ha['stats'][role].get('completed',ha['stats'][role].get('received',0))-hb['stats'][role].get('completed',hb['stats'][role].get('received',0))
            if role=='target': heap_msgs=ha['stats'][role].get('received',ha['stats'][role].get('completed',0))-hb['stats'][role].get('received',hb['stats'][role].get('completed',0))
            print(' heap bytes/msg',round(sizes/heap_msgs,1),'records/msg',round(len(hx['samples'])/heap_msgs,4),'records',len(hx['samples']))

print('\nCRITICAL FRAMEWORK THREAD TOP METHODS')
for pat,role in [('send','target'),('serial','source')]:
 p=CAPS[(pat,'framework',role)]; report=R/(p['capture'].parent.name+'-'+role+'-self.txt')
 es=entries(report); rows=[]
 for pct,s in es:
  if 'JS:' in s and ('/packages/framework/dist/' in s or '/packages/framework-codec-' in s):
   s=s.replace('/root/worktree/zlink-ratio040/framework/languages/node/packages/framework/dist/','framework/languages/node/packages/framework/src/').replace('.js:', '.ts:')
   rows.append((pct*p['main_cpu']/100,pct,s))
 rows.sort(reverse=True)
 print('\n',pat,role,'main CPU',p['main_cpu'])
 for i,(us,pct,s) in enumerate(rows[:25],1): print(i,round(us,3),f'{pct:.2f}%',s[:220])
print('\nCRITICAL THREAD CPU STAGES')
for pat,role in [('send','target'),('serial','source')]:
 p=CAPS[(pat,'framework',role)]; report=R/(p['capture'].parent.name+'-'+role+'-self.txt'); groups=collections.defaultdict(float)
 for pct,s in entries(report):groups[cat(s)]+=pct
 print(pat,'critical main cpu',round(p['main_cpu'],2),'µs/msg')
 for k,v in groups.items():print(' ',k,round(p['main_cpu']*v/100,2))
