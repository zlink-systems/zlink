from pathlib import Path
import collections,csv,json,re,subprocess,statistics
P=Path('/root/jobs/fx/cx/v21/s033/creply')
head=re.compile(r'^(.+?)\s+(\d+)/(\d+)\s+([0-9.]+):\s+(\d+)\s+cycles:k:')
frame=re.compile(r'^\s+([0-9a-f]+) (.*) \(([^()]*)\)$')
def parse(f):
 cache=f.with_suffix('.samples.json')
 if cache.exists():return json.load(open(cache))
 proc=subprocess.Popen(['perf','script','--ns','-i',str(f),'-F','comm,pid,tid,time,event,period,ip,sym,dso'],stdout=subprocess.PIPE,stderr=subprocess.DEVNULL,text=True)
 ss=[];s=None
 for l in proc.stdout:
  m=head.match(l)
  if m:
   if s:ss.append(s)
   s={'comm':m[1],'pid':int(m[2]),'tid':int(m[3]),'time':float(m[4]),'period':int(m[5]),'frames':[]}
  elif not l.strip():
   if s:ss.append(s);s=None
  elif s:
   m=frame.match(l)
   if m:s['frames'].append([m[2],m[3]])
 if s:ss.append(s)
 assert proc.wait()==0
 cache.write_text(json.dumps(ss,separators=(',',':')));return ss

def window(w):
 b=w['before'];a=w['after'];sec=(a['start']+a['end']-b['start']-b['end'])/2
 n=a['stats']['source']['completed']-b['stats']['source']['completed']
 return n,sec,n/sec

def bpf_window(cap,mode):
 n,sec,rate=window(json.load(open(cap/mode/'window.json')))
 txt=(cap/(mode+'.log')).read_text()
 start=re.search(r'START (\d+)',txt);stop=re.search(r'STOP (\d+)',txt)
 if start and stop:sec=(int(stop[1])-int(start[1]))/1e9;n=rate*sec
 return n,sec,rate

def maps(log):
 r={}
 for l in log.read_text().splitlines():
  try:v=json.loads(l)
  except ValueError:continue
  if v['type']=='map':r.update(v['data'])
 return r

def io_stages(fs):
 s='\n'.join(f[0] for f in fs)
 if 'tcp_sendmsg' in s:return 'send'
 if 'tcp_recvmsg' in s:return 'recv'
 if 'epoll_wait' in s or 'ep_poll' in s:return 'epoll_wait'
 if 'epoll_ctl' in s:return 'epoll_ctl'
 return 'other'
def group(fs):
 s='\n'.join(f[0] for f in fs)
 if any(k in s for k in ('try_to_wake_up','ttwu_','wake_up_q')):return 'wakeup'
 if any(k in s for k in ('tcp_v4_rcv','tcp_v6_rcv','tcp_v4_do_rcv','tcp_rcv_established')):return 'TCP_receive_without_wakeup'
 if any(k in s for k in ('net_rx_action','__do_softirq','handle_softirqs','do_softirq')):return 'RX_softirq_other'
 if '__tcp_transmit_skb' in s:return 'TX_network_without_RX_softirq'
 return 'send_other'

allperf={};allsys={};allnet={}
for run in sorted((P/'runs').iterdir()):
 cap=run/'capture'
 if (cap/'perf-window.json').exists():
  n,sec,rate=window(json.load(open(cap/'perf-window.json')));rr={'rate':rate,'seconds':sec,'messages':n,'threads':{}}
  for role in ('source','target'):
   samples=parse(cap/(role+'.perf.data'));counters=collections.defaultdict(dict)
   for row in csv.reader(open(cap/(role+'.cycles.csv'))):
    if not row or row[0].startswith('#') or len(row)<6:continue
    m=re.search(r'-(\d+)$',row[0])
    if m:
     try:counters[int(m[1])][row[3]]=float(row[1])
     except ValueError:pass
   for tid,cc in counters.items():
    ss=[s for s in samples if s['tid']==tid and s['frames']];total=sum(s['period'] for s in ss)
    statcmd=json.load(open(cap/(role+'-stat.command.json')));duration=int(statcmd[statcmd.index('--timeout')+1])/1000
    cyc=cc.get('cycles:k',0)/(rate*duration);ins=cc.get('instructions:k',0)/(rate*duration)
    weights=collections.Counter();selfs=collections.Counter();inclusive=collections.Counter();groups=collections.Counter()
    for s in ss:
     weight=cyc*s['period']/total if total else 0;kind=io_stages(s['frames']);weights[kind]+=weight
     if kind=='send':
      selfs[s['frames'][0][0]]+=weight;groups[group(s['frames'])]+=weight
      for f in set(f[0] for f in s['frames']):inclusive[f]+=weight
    rr['threads'][str(tid)]={'role':role,'comm':ss[0]['comm'] if ss else 'idle','cycles_per_request':cyc,'instructions_per_request':ins,'ipc':ins/cyc if cyc else 0,'counters':cc,'samples':len(ss),'sample_period':total,'stage_cycles_per_request':dict(weights),'send_groups_cycles_per_request':dict(groups),'send_self_cycles_per_request':selfs.most_common(200),'send_inclusive_cycles_per_request':inclusive.most_common(60)}
  allperf[run.name]=rr
 if (cap/'syscall/window.json').exists():
  n,sec,rate=bpf_window(cap,'syscall');m=maps(cap/'syscall.log');r=[]
  for key,v in m.get('@calls',{}).items():
   pid,tid,fd,op=map(int,key.split(','));pos=m.get('@positive',{}).get(key,0);bytes_=m.get('@bytes',{}).get(key,0)
   row={'pid':pid,'tid':tid,'fd':fd,'op':op,'calls':v,'positive':pos,'calls_per_request':v/n,'bytes_per_positive':bytes_/pos if pos else 0,'bytes_per_request':bytes_/n,'requested_per_call':m['@requested'][key]/v,'short':m.get('@short',{}).get(key,0),'eagain':m.get('@status',{}).get(key+',-11',0),'requested_hist':{},'returned_hist':{}}
   for hist in ('requested_hist','returned_hist'):
    row[hist]={k.split(',')[-1]:vv for k,vv in m.get('@'+hist,{}).items() if k.startswith(key+',')}
   r.append(row)
  allsys[run.name]={'messages':n,'seconds':sec,'rate':rate,'rows':r}
 if (cap/'net/window.json').exists():
  n,sec,rate=bpf_window(cap,'net');allnet[run.name]={'messages':n,'seconds':sec,'rate':rate,'maps':maps(cap/'net.log')}
for name,data in [('perf-summary',allperf),('syscall-summary',allsys),('net-summary',allnet)]:
 (P/(name+'.json')).write_text(json.dumps(data,indent=2))
for case in ('main','prototype','binding'):
 vals=[]
 for f in sorted((P/'runs').glob(case+'-unpaced-*/results.json')):
  c=json.load(open(f))['cells'][0];vals.append(c['throughput_per_second'])
 print(case,vals,'median',statistics.median(vals) if vals else None)
for name,r in allperf.items():
 print(name,round(r['rate']),[(t['role'],t['comm'],round(t['cycles_per_request']),round(t['instructions_per_request']),{k:round(v) for k,v in t['stage_cycles_per_request'].items()}) for t in r['threads'].values() if t['samples']>20])
for name,r in allsys.items():print(name,'syscall rate',r['rate'],[(t['tid'],t['op'],round(t['calls_per_request'],3),round(t['bytes_per_positive']),t['short'],t['eagain']) for t in r['rows'] if t['calls']>1000])
