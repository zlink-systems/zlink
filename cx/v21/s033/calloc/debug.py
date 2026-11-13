#!/usr/bin/env python3
import json, pathlib, struct, subprocess, re, collections, sys
ROOT=pathlib.Path('/root/worktree/zlink-ratio040')
JOB=pathlib.Path('/root/jobs/fx/cx/v21/s033/calloc')
REC_SIZE=120; FRAME_N=12; OFF=4096+4096*64

def addr_map(mapsfile):
    seg=[]
    for line in open(mapsfile):
        x=line.rstrip().split(None,5)
        if len(x)<6: continue
        rg,perm,off,dev,ino,path=x
        if not path.startswith('/'): continue
        a,b=(int(v,16) for v in rg.split('-'))
        seg.append((a,b,int(off,16),path))
    return seg

def symbolize(mapsfile, samples):
    maps=addr_map(mapsfile); todo=collections.defaultdict(set)
    for r in samples:
        for ip in r['ips']:
            m=next((m for m in maps if m[0]<=ip<m[1]),None)
            if m: todo[m[3]].add(ip-m[0]+m[2])
    out={}
    for path,addrs in todo.items():
        addrs=sorted(addrs)
        try:
            p=subprocess.run(['addr2line','-f','-C','-e',path],input=''.join(f'0x{x:x}\n' for x in addrs),text=True,capture_output=True,timeout=120)
            rows=p.stdout.splitlines()
            if len(rows)<2*len(addrs): continue
            for i,addr in enumerate(addrs):
                fn=rows[2*i].strip(); loc=rows[2*i+1].strip()
                out[(path,addr)]=(fn,loc)
        except (OSError,subprocess.SubprocessError): pass
    return maps,out

def parse_samples(path):
    data=path.read_bytes(); n=len(data)//REC_SIZE; out=[]
    for i in range(n):
        vals=struct.unpack_from('<QQII12Q',data,i*REC_SIZE)
        tid,size,kind,nf,*ips=vals
        if not tid or not nf: continue
        out.append({'tid':tid,'size':size,'kind':kind,'ips':ips[:min(nf,FRAME_N)]})
    return out

def classify(samples,maps,sym):
    classified=[]
    for r in samples:
        frames=[]
        for ip in r['ips']:
            m=next((m for m in maps if m[0]<=ip<m[1]),None)
            if not m: continue
            fn,loc=sym.get((m[3],ip-m[0]+m[2]),('??','??:0'))
            frames.append((fn,loc,m[3]))
        site='[no Framework/binding frame]'
        for fn,loc,path in frames:
            fnl=fn.lower(); locl=loc.lower().replace('\\','/')
            if '/framework/languages/cpp/framework/' in locl or 'zlink::framework::' in fnl:
                site=(loc.split(':')[0] if loc not in ('??:0','??') else 'framework/?')+':'+fn
                break
            if '/bindings/cpp/' in locl or ('zlink::' in fnl and 'zlink::framework::' not in fnl and ('bindings' in fnl or 'client_t::' in fnl or 'socket_t::' in fnl or 'request_t::' in fnl or 'message_t::' in fnl)):
                site=(loc.split(':')[0] if loc not in ('??:0','??') else 'bindings/?')+':'+fn
                break
        r['site']=site; classified.append(r)
    return classified

def proc_scope(cell,role,tids=None):
    cap=cell/'capture'; delta=json.load(open(cap/f'{role}.alloc-delta.json'))
    samples=parse_samples(cap/f'{role}.samples.bin')
    maps,sym=symbolize(cell/f'{role}.maps',samples)
    samples=classify(samples,maps,sym)
    before=json.load(open(cap/'before.json')); after=json.load(open(cap/'after.json'))
    msgs=after['stats']['source']['completed']-before['stats']['source']['completed']
    if role=='target':
        msgs=after['stats']['source']['completed']-before['stats']['source']['completed']
    ctr=delta['slots']
    if tids is None: tids=set(map(int,ctr))
    total=sum(sum(v[k] for k in ('malloc','calloc','realloc')) for tid,v in ((int(t),v) for t,v in ctr.items()) if tid in tids)
    ss=[r for r in samples if r['tid'] in tids]
    weight=len(ss)*256
    factor=total/weight if weight else 0
    groups=collections.defaultdict(lambda:[0,0,0])
    for r in ss:
        x=groups[r['site']];x[0]+=1;x[1]+=r['size']
    rows=[]
    for site,values in groups.items():
        n,b=values[:2]
        rows.append({'site':site,'calls_per_msg':n*256*factor/max(msgs,1),'bytes_per_msg':b*256*factor/max(msgs,1),'samples':n})
    rows.sort(key=lambda x:x['calls_per_msg'],reverse=True)
    good=sum(x['calls_per_msg'] for x in rows if x['site']!='[no Framework/binding frame]')
    threads=[]
    for tid,v in ctr.items():
        alloc=sum(v[k] for k in ('malloc','calloc','realloc'))
        n=sum(1 for r in samples if r['tid']==int(tid))
        threads.append({'tid':int(tid),'calls':alloc,'calls_per_message':alloc/max(msgs,1),'sample_count':n,'comm':after['processes'][role]['threads'].get(tid,{}).get('comm','')})
    threads.sort(key=lambda x:x['calls'],reverse=True)
    return {'messages':msgs,'calls':total,'calls_per_message':total/max(msgs,1),'samples':len(ss),'sample_weight':weight,'coverage':good/max(total,1),'factor':factor,'rows':rows,'threads':threads,'records':len(samples)}

cell=JOB/'out-retry'/'run1'/'zlink-cpp-request-backpressure-1024'; role='source'
samples=parse_samples(cell/'capture'/f'{role}.samples.bin'); maps,sym=symbolize(cell/f'{role}.maps',samples)
for r in samples:
 fs=[]
 for ip in r['ips']:
  m=next((m for m in maps if m[0]<=ip<m[1]),None)
  if not m: continue
  fn,loc=sym.get((m[3],ip-m[0]+m[2]),('??','??:0'))
  fs.append((fn,loc,m[3]))
 if any('message_t' in f[0] for f in fs): continue
 print('SAMPLE',r['tid'],r['size'])
 for x in fs: print(' ',x)
 if r is samples[20]: break
