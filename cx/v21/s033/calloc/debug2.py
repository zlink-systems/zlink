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
            # Skip allocator/runtime/library templates whose signatures mention ZLink types.
            if fnl.startswith(('std::','google::','operator new','malloc','calloc','realloc')) or 'std::_function_handler<' in fnl:
                continue
            if '/framework/languages/cpp/framework/' in locl or 'zlink::framework::' in fnl:
                file=(loc.split(':')[0] if loc not in ('??:0','??','?','') else 'framework/?')
                site=file+':'+fn
                break
            if '/bindings/cpp/' in locl:
                file=(loc.split(':')[0] if loc not in ('??:0','??','?','') else 'bindings/?')
                site=file+':'+fn
                break
            if fnl.startswith(('zlink::request_','zlink::async_result_t','zlink::detail::completion_','zlink::detail::resume_async_slot','zlink::detail::operation_state_t','zlink::detail::submit_raw_request_state','zlink::completion_t')):
                site='bindings/cpp:'+fn
                break
            # Core/binding-independent native frames are not the requested attribution layer.
            if fnl.startswith(('zlink::message_t','zlink::advanced::','zlink::socket_t','zlink::poller_t','zlink::router_socket_t')):
                continue
        if site=='[no Framework/binding frame]':
            for fn,loc,path in frames:
                leaf=path.rsplit('/',1)[-1]
                if leaf in ('bench_cpp_client','bench_cpp_framework_server','bench_cpp_zlink_server'):
                    site=f'{leaf}.cpp:{fn} [benchmark caller]'; break
        r['site']=site; classified.append(r)
    return classified
cell=JOB/'out-retry'/'run1'/'zlink-framework-cpp-request-backpressure-1024'; role='target'
samples=parse_samples(cell/'capture'/f'{role}.samples.bin'); maps,sym=symbolize(cell/f'{role}.maps',samples); samples=classify(samples,maps,sym)
for r in samples:
 if r['site']!='[no Framework/binding frame]': continue
 fs=[]
 for ip in r['ips']:
  m=next((m for m in maps if m[0]<=ip<m[1]),None)
  if not m: continue
  fn,loc=sym.get((m[3],ip-m[0]+m[2]),('??','??:0')); fs.append((fn,loc,m[3]))
 print('SAMPLE',r['tid'],r['size'])
 for f in fs: print(' ',f)
 if fs and r['size']>5000: break
