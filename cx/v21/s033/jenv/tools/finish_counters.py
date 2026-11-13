from pathlib import Path
import json,subprocess
from summarize_bpf import summarize
P=Path('/root/jobs/fx/cx/v21/s033/jenv')
for c in ('main','prototype','binding'):
 case=c+'-counters';out=P/'runs'/case
 subprocess.run(['python3',str(P/'tools/wire_pcap.py'),str(out/'wire.pcap')],check=True)
 bpf=summarize(out/'capture-bpf');maps={}
 for line in (out/'capture-bpf/bpf.stderr').read_text().splitlines():
  try:d=json.loads(line)
  except ValueError:continue
  if d.get('type') in ('map','hist'):maps.update(d['data'])
 rows=[];sockets=[]
 for role in ('source','target'):
  pid=bpf['roles'][role]['pid']
  for row in bpf['roles'][role]['rows']:
   if not row['io'] or not row['tcp'] or row['syscall'] not in ('sendto','recvfrom'):continue
   if not any(row['tcp'][x].endswith(':5254') or row['tcp'][x].endswith(':5247') for x in ('local','remote')):continue
   counts=row['counts'];key=f"{pid},{row['tid']},{row['fd']},{44 if row['syscall']=='sendto' else 45}"
   rows.append({'role':role,**row,'mean_positive_bytes':counts.get('positive_return_sum',0)/counts.get('positive',1),'histogram':maps.get('@sizes',{}).get(key,[])})
 for key,count in maps.get('@socket_options',{}).items():
  pid,tid,sport,dport,snd,rcv,nonagle,userlocks=map(int,key.split(','))
  if 5247 not in (sport,dport) and 5254 not in (sport,dport):continue
  sockets.append({'pid':pid,'tid':tid,'sport':sport,'dport':dport,'SO_SNDBUF':snd,'SO_RCVBUF':rcv,'TCP_NODELAY':int(bool(nonagle&1)),'nonagle':nonagle,'sk_userlocks':userlocks,'samples':count})
 d=json.load(open(P/'runs'/c/'extra-summary.json'))
 d.setdefault('syscalls_original',d['syscalls']);d.setdefault('sockets_original',d['sockets']);d['syscalls']=rows;d['sockets']=sockets;d['counter_capture']=case;d['counter_rate']=bpf['rate']
 (P/'runs'/c/'extra-summary.json').write_text(json.dumps(d,indent=2))
 print(c,'focused BPF rate',bpf['rate'],'full rate',json.load(open(out/'results.json'))['cells'][0]['throughput_per_second'],flush=True)
