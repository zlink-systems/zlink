from pathlib import Path
import json,re,collections
from summarize_bpf import summarize
P=Path('/root/jobs/fx/cx/v21/s033/jenv')
def maps(path):
 result={}
 for line in path.read_text().splitlines():
  try:d=json.loads(line)
  except ValueError:continue
  if d.get('type') in ('map','hist'):result.update(d['data'])
 return result
# Correct BPF counter interval bookkeeping in all retained captures.
for case in ('main','prototype-unpaced','prototype','binding','main-counters','prototype-counters','binding-counters'):
 cap=P/'runs'/case/'capture-bpf'
 if cap.exists():summarize(cap)
for c in ('main','prototype','binding'):
 case=c+'-causes';out=P/'runs'/case;bpf=summarize(out/'capture-bpf');m=maps(out/'capture-bpf/bpf.stderr');rows=[];sockets=[];kernel={}
 perf=json.load(open(out/'capture/summary.json'));n=perf['messages'];sizes=(1435,1443) if c=='main' else (1092,1092) if c=='prototype' else (1252,1252)
 for role in ('source','target'):
  pid=bpf['roles'][role]['pid'];kernel[role]=[]
  for row in bpf['roles'][role]['rows']:
   if not row['io'] or not row['tcp'] or row['syscall'] not in ('sendto','recvfrom'):continue
   if not any(row['tcp'][x].endswith(':5254') or row['tcp'][x].endswith(':5247') for x in ('local','remote')):continue
   counts=row['counts'];key=f"{pid},{row['tid']},{row['fd']},{44 if row['syscall']=='sendto' else 45}"
   rows.append({'role':role,**row,'histogram':m.get('@sizes',{}).get(key,[])})
  samples=json.load(open(out/'capture'/(role+'.samples.json')));cycles={}
  for line in (out/'capture'/(role+'.cycles.csv')).read_text().splitlines():
   f=line.split(',')
   if len(f)>3 and f[3]=='cycles:k' and f[1].isdigit():cycles[f[0].rsplit('-',1)[-1]]=int(f[1])
  for tid,t in perf['roles'][role]['threads'].items():
   ss=[s for s in samples if s['event']=='cycles:k' and s['tid']==tid and s['frames']]
   tcp=[s for s in ss if any(re.search(r'^(?:tcp_|__tcp_|inet_|__inet_|ip_|__ip_|skb_|__skb_|sock_|__sock_|sk_|__sk_|__dev_|dev_|netif_|net_rx_|__netif_|__alloc_skb|consume_skb|kfree_skb|__kfree_skb)',f[0]) for f in s['frames'])]
   direction='send' if t['stages'].get('send',0)>t['stages'].get('recv',0) else 'recv';record=sizes[0] if (role=='source')==(direction=='send') else sizes[1]
   frac=sum(s['period'] for s in tcp)/sum(s['period'] for s in ss) if ss else 0;tcp_cycles=cycles.get(tid,0)*frac*perf['seconds']/10.0
   kernel[role].append({'comm':t['comm'],'cpu_us':t['cpu_us_per_message'],'stages':t['stages'],'record_bytes':record,'tcp_cycles_per_byte':tcp_cycles/(n*record),'tcp_cycles_per_request':tcp_cycles/n,'tcp_samples':len(tcp),'kernel_samples':len(ss)})
 for key,count in m.get('@socket_options',{}).items():
  pid,tid,sport,dport,snd,rcv,nonagle,userlocks=map(int,key.split(','))
  if 5247 not in (sport,dport) and 5254 not in (sport,dport):continue
  sockets.append({'pid':pid,'tid':tid,'sport':sport,'dport':dport,'SO_SNDBUF':snd,'SO_RCVBUF':rcv,'TCP_NODELAY':int(bool(nonagle&1)),'nonagle':nonagle,'sk_userlocks':userlocks,'samples':count})
 d=json.load(open(P/'runs'/c/'extra-summary.json'))
 d['counter_61132_capture']=d.get('counter_capture');d['counter_61132_rate']=json.load(open(P/'runs'/(c+'-counters')/'capture-bpf/summary.json'))['rate']
 d['cause_capture']=case;d['cause_perf_rate']=perf['rate'];d['cause_rate']=json.load(open(out/'results.json'))['cells'][0]['throughput_per_second'];d['cause_kernel']=kernel
 d['counter_capture']=case;d['counter_rate']=bpf['rate'];d['syscalls']=rows;d['sockets']=sockets
 (P/'runs'/c/'extra-summary.json').write_text(json.dumps(d,indent=2))
 print(c,'50k full',d['cause_rate'],'perf',perf['rate'],'BPF',bpf['rate'],flush=True)
