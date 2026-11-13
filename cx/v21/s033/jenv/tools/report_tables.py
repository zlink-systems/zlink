from pathlib import Path
import json,collections
P=Path('/root/jobs/fx/cx/v21/s033/jenv');cases=('main','prototype','binding');labels={'main':'main Framework','prototype':'prototype paced','prototype-unpaced':'prototype unpaced','binding':'binding paced'}
def load(case,file):return json.load(open(P/'runs'/case/file))
def fmt(x):return f'{x:,.3f}'
def table(headers,rows):
 return '| '+' | '.join(headers)+' |\n|'+ '|'.join(['---']*len(headers))+'|\n'+''.join('| '+' | '.join(str(x) for x in row)+' |\n' for row in rows)+'\n'
def thread(case,role,comm):return next(t for t in load(case,'capture/summary.json')['roles'][role]['threads'].values() if t['comm']==comm)
def first():
 rows=[]
 rows.append(['request / reply record B','1,435 / 1,443','1,092 / 1,092','1,252 / 1,252'])
 for role,name in [('source','IO/0'),('source','IO/2'),('target','IO/0'),('target','IO/1')]:rows.append([role+' '+name+' CPU µs/request']+[fmt(thread(c,role,'ZLINKbg/'+name)['cpu_us_per_message']) for c in cases])
 rows.append(['source I/O 합계 CPU µs/request']+[fmt(load(c,'capture/summary.json')['roles']['source']['io_cpu_us_per_message']) for c in cases])
 rows.append(['target I/O 합계 CPU µs/request']+[fmt(load(c,'capture/summary.json')['roles']['target']['io_cpu_us_per_message']) for c in cases])
 for role in ('source','target'):
  rows.append([role+' mesh CPU µs/request']+[fmt(sum(v['cpu_us'] for v in load(c,'extra-summary.json')['mesh'][role])) if c!='binding' else '해당 thread 없음' for c in cases])
 for field,label in [('caller_us','caller µs'),('throughput','active 전체 req/s')]:rows.append([label]+[fmt(load(c,'extra-summary.json')[field]) for c in cases])
 rows.append(['perf 창 req/s']+[fmt(load(c,'capture/summary.json')['rate']) for c in cases])
 rows.append(['BPF 창 req/s']+[fmt(load(c,'capture-bpf/summary.json')['rate']) for c in cases])
 return table(['항목']+[labels[c] for c in cases],rows)
def stages():
 rows=[]
 for c in ('main','prototype-unpaced','prototype','binding'):
  for role,name in [(r,n) for r in ('source','target') for n in ('IO/0','IO/1','IO/2','IO/3')]:
   t=thread(c,role,'ZLINKbg/'+name);s=t['stages'];rows.append([labels[c],role+' '+name,fmt(t['cpu_us_per_message']),fmt(s.get('send',0)),fmt(s.get('recv',0)),fmt(s.get('epoll_wait',0)+s.get('epoll_ctl',0)),fmt(s.get('read',0)+s.get('write',0)),fmt(s.get('userspace_other',0)),fmt(s.get('other_syscall',0)),fmt(s.get('not_sampled',0)),t['samples']])
 return table(['실행','thread','합계 µs','TCP send','TCP receive','epoll','eventfd/wake','Core userspace','기타 syscall','미표본','표본 수'],rows)
def jfr():
 rows=[]
 for category in ('registry take','reply decode','multipart view decode','completion handoff','other'):
  row=[category]
  for c in ('main','prototype-unpaced','prototype'):
   d=load(c,'extra-summary.json')['mesh_jfr'];row.append(f"{fmt(d['us'][category])} ({d['counts'].get(category,0)}/{d['samples']})")
  rows.append(row)
 return table(['mesh source JFR 분류 µs (표본)']+[labels[c] for c in ('main','prototype-unpaced','prototype')],rows)
def syscalls(focused=False):
 rows=[]
 for c in (cases if focused else ('main','prototype-unpaced','prototype','binding')):
  d=load(c,'extra-summary.json');items=d['syscalls'] if focused else d.get('syscalls_original',d['syscalls'])
  counter_case=d['counter_capture'] if focused else c
  for role,name,op in [('source','IO/0','sendto'),('source','IO/2','recvfrom'),('target','IO/0','recvfrom'),('target','IO/1','sendto')]:
   matches=[r for r in items if r['role']==role and r['comm']=='ZLINKbg/'+name and r['syscall']==op]
   if not matches:continue
   counts=collections.Counter();hist=collections.Counter()
   for r in matches:
    counts.update(r['counts'])
    for b in r['histogram']:
     group=0 if b['max']<2048 else 1 if b['max']<4096 else 2 if b['max']<8192 else 3
     hist[group]+=b['count']
   positives=counts['positive'];total=sum(hist.values());n=load(counter_case,'capture-bpf/summary.json')['messages']
   rows.append([labels[c]+(' 50k' if focused else ''),role+' '+name,op,fmt(load(counter_case,'capture-bpf/summary.json')['rate']),fmt(counts['calls']/n),fmt(counts['positive_return_sum']/positives),fmt(counts['ret_-11']/n),'/'.join(f'{100*hist[i]/total:.1f}%' for i in range(4))])
 return table(['실행','thread','syscall','계수 창 req/s','호출/request','성공 B/호출','EAGAIN/request','<2 / 2–4 / 4–8 / ≥8 KiB 비율'],rows)
def kernel():
 rows=[]
 for c in ('main','prototype-unpaced','prototype','binding'):
  for role,name in [('source','IO/0'),('source','IO/2'),('target','IO/0'),('target','IO/1')]:
   r=next(x for x in load(c,'extra-summary.json')['kernel'][role] if x['comm']=='ZLINKbg/'+name)
   rows.append([labels[c],role+' '+name,fmt(r['estimated_tcp_cycles_per_byte']),fmt(r['tcp_cycles_per_request']),f"{r['tcp_samples']}/{r['kernel_samples']}"])
 return table(['실행','thread','kernel TCP cycles/B 추정','kernel TCP cycles/request 추정','TCP/kernel 표본'],rows)
def sockets():
 rows=[]
 for c in cases:
  d=load(c,'extra-summary.json')
  captures=[('기본',c,d.get('sockets_original',d['sockets']))]
  if 'counter_capture' in d:captures.append(('50k I/O 전용',d['counter_capture'],d['sockets']))
  for capture,counter_case,items in captures:
   pids={role:load(counter_case,'capture-bpf/summary.json')['roles'][role]['pid'] for role in ('source','target')};groups=collections.defaultdict(list)
   for x in items:groups[x['pid'],x['sport'],x['dport']].append(x)
   for (pid,sport,dport),rs in sorted(groups.items()):
    role=next((r for r,p in pids.items() if p==pid),str(pid))
    rows.append([labels[c],capture,role,f'{sport} → {dport}',','.join(str(x) for x in sorted(set(r['TCP_NODELAY'] for r in rs))),','.join(str(x) for x in sorted(set(r['SO_SNDBUF'] for r in rs))),str(min(r['SO_RCVBUF'] for r in rs))+'–'+str(max(r['SO_RCVBUF'] for r in rs)),','.join(str(x) for x in sorted(set(r['sk_userlocks'] for r in rs)))])
 return table(['실행','계측','process','local → remote port','TCP_NODELAY','SO_SNDBUF B','SO_RCVBUF 관측 범위 B','sk_userlocks'],rows)
def decoder():
 rows=[]
 for c in cases:
  d=load(c,'extra-summary.json');pids={role:load(c,'capture-bpf/summary.json')['roles'][role]['pid'] for role in ('source','target')};groups=collections.defaultdict(list)
  for x in d['decoder']:
   if x['size'] not in (13,21,41,201,1027,1398):continue
   groups[x['pid'],x['size']].append(x)
  for (pid,size),rs in sorted(groups.items()):
   role=next((r for r,p in pids.items() if p==pid),str(pid));fit=sum(r['count'] for r in rs if r['allocator_fit']);miss=sum(r['count'] for r in rs if not r['allocator_fit']);types=sorted(set(r['storage_type'] for r in rs))
   rows.append([labels[c],role,size,fit,miss,f'{miss/(fit+miss)*100:.2f}%',','.join(str(x) for x in types)])
 return table(['실행','수신 process','native part B','allocator 범위 안','범위 밖','범위 밖 비율','storage type'],rows)
if __name__=='__main__':
 s=first()+stages()+jfr()+syscalls()+kernel()+sockets()+decoder();(P/'tables.md').write_text(s)

def cause_compare():
 rows=[]
 for field,label in [('cause_rate','전체 req/s'),('cause_perf_rate','perf 창 req/s'),('counter_rate','BPF 창 req/s')]:
  rows.append([label]+[fmt(load(c,'extra-summary.json')[field]) for c in cases])
 for role,name in [('source','IO/0'),('source','IO/2'),('target','IO/0'),('target','IO/1')]:
  ts=[next(x for x in load(c,'extra-summary.json')['cause_kernel'][role] if x['comm']=='ZLINKbg/'+name) for c in cases]
  rows.append([role+' '+name+' CPU µs/request']+[fmt(x['cpu_us']) for x in ts])
  rows.append([role+' '+name+' TCP cycles/B']+[fmt(x['tcp_cycles_per_byte']) for x in ts])
  rows.append([role+' '+name+' TCP cycles/request']+[fmt(x['tcp_cycles_per_request']) for x in ts])
 return table(['TCP 원인 확인: 50k req/s']+[labels[c] for c in cases],rows)
