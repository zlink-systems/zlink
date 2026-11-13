import collections,json,pathlib,re,struct,subprocess,sys
P=pathlib.Path('/root/jobs/fx/cx/v21/s033/xlang');HZ=100

def load(p):return json.loads(pathlib.Path(p).read_text())
def proctime(s):
 f=s[s.rindex(')')+2:].split();return int(f[11]),int(f[12])
def pcap(p,lo,hi,ports):
 d=collections.Counter()
 with p.open('rb') as f:
  h=f.read(24)
  if not h:return {}
  endian='<' if h[:4] in (b'\xd4\xc3\xb2\xa1',b'\x4d\x3c\xb2\xa1') else '>'
  nano=h[:4] in (b'\x4d\x3c\xb2\xa1',b'\xa1\xb2\x3c\x4d');link=struct.unpack(endian+'I',h[20:])[0]
  while True:
   h=f.read(16)
   if not h:break
   sec,us,n,orig=struct.unpack(endian+'4I',h);b=f.read(n);ts=sec+us/(1e9 if nano else 1e6)
   if not lo<=ts<=hi:continue
   offset=14 if link==1 else 16 if link==113 else 20 if link==276 else 4 if link==0 else 0
   if len(b)<offset+40 or b[offset]>>4!=4:continue
   ihl=(b[offset]&15)*4
   if b[offset+9]!=6:continue
   t=offset+ihl
   if len(b)<t+20:continue
   sport,dport=struct.unpack('!HH',b[t:t+4]);total=struct.unpack('!H',b[offset+2:offset+4])[0];payload=total-ihl-(b[t+12]>>4)*4
   if payload<=0:continue
   direction='request' if dport in ports['target'] or sport in ports['source'] else 'reply' if sport in ports['target'] or dport in ports['source'] else 'other'
   d[direction+'_packets']+=1;d[direction+'_bytes']+=payload
 return dict(d)
def ss(p):
 d={};key=None
 for line in p.read_text().splitlines():
  if line.startswith(('ESTAB','FIN','CLOSE','TIME')):key=' '.join(line.split()[3:5])
  elif key:
   m=re.search(r'data_segs_out:(\d+)',line)
   if m:d[key]=int(m.group(1))
 return d

def bpf(p):
 d={}
 for l in p.read_text().splitlines():
  try:x=json.loads(l)
  except ValueError:continue
  if x.get('type')=='map':d.update(x['data'])
 return d

def analyze(d):
 m=load(d/'meta.json');job=m['job'];a=load(d/'before.json');b=load(d/'after.json');c=load(d/'results.json')['cells'][0]
 n=b['stats']['source']['completed']-a['stats']['source']['completed']
 if not n:raise ValueError('zero completed')
 assert b['time']-a['time']>=.7*job['duration'],'capture window too short'
 r={'job':job,'throughput':m['throughput'],'path':str(d),'window_s':b['time']-a['time'],'messages':n,'actual_rate':n/(b['time']-a['time']),'roles':{},'total_us':0,'user_us':0,'kernel_us':0,'threads':[],'switches_per_msg':0}
 bp=bpf(d/'wake.log') if (d/'wake.log').exists() else {}
 send=bp.get('@send',{});recv=bp.get('@recv',{});r['wakeups_per_msg']=sum(bp.get('@wakes',{}).values())/n if '@wakes' in bp else None;r['wakeup_edges']=bp.get('@wakes',{})
 owners={int(tid):v['pid'] for shot in (a,b) for v in shot['processes'].values() for tid in v['threads']}
 wc=collections.Counter()
 for edge,count in bp.get('@wakes',{}).items():
  waker,tid,woken=map(int,edge.split(','));owner=owners.get(woken)
  wc['unknown' if owner is None else 'same_process' if owner==waker else 'cross_process']+=count
 r['wakeup_breakdown_per_msg']={k:v/n for k,v in wc.items()}
 for side,p in b['processes'].items():
  p0=a['processes'][side];u,k=proctime(p['stat']);u0,k0=proctime(p0['stat']);u=(u-u0)*1e6/HZ/n;k=(k-k0)*1e6/HZ/n
  r['roles'][side]={'user_us':u,'kernel_us':k,'cpu_us':u+k};r['total_us']+=u+k;r['user_us']+=u;r['kernel_us']+=k
  for tid,t in p['threads'].items():
   t0=p0['threads'].get(tid,{'runtime_ns':0,'user_ticks':0,'kernel_ticks':0,'switches':0})
   cpu=(t['runtime_ns']-t0['runtime_ns'])/1000/n
   if cpu<.001:continue
   item={'side':side,'tid':int(tid),'comm':t['comm'],'cpu_us':cpu,'user_us':(t['user_ticks']-t0['user_ticks'])*1e6/HZ/n,'kernel_us':(t['kernel_ticks']-t0['kernel_ticks'])*1e6/HZ/n,'switches_per_msg':(t['switches']-t0['switches'])/n,'sendto_per_msg':send.get(str(p['pid'])+','+tid,0)/n,'recvfrom_per_msg':recv.get(str(p['pid'])+','+tid,0)/n}
   r['threads'].append(item);r['switches_per_msg']+=item['switches_per_msg']
 r['threads'].sort(key=lambda t:t['cpu_us'],reverse=True)
 if (d/'tcp.pcap').exists():
  base={'cpp':5292 if job['fw'] else 5285,'java':5252 if job['fw'] else 5245,'dotnet':5212 if job['fw'] else 5205,'node':5232 if job['fw'] else 5225}[job['lang']]
  pk=pcap(d/'tcp.pcap',a['realtime'],b['realtime'],load(d/'data-ports.json'));assert sum(v for k,v in pk.items() if k.endswith('_packets'))>0, 'no TCP data captured';assert pk.get('other_packets',0)==0,pk;r['tcp']={k:v/n for k,v in pk.items()};r['tcp']['data_packets_per_msg']=sum(v for k,v in pk.items() if k.endswith('_packets'))/n
  sa=ss(d/'ss-before.txt');sb=ss(d/'ss-after.txt');r['ss_data_segs_per_msg']=sum(max(0,v-sa.get(k,0)) for k,v in sb.items())/n
 if (d/'alloc-before.json').exists():
  aa=load(d/'alloc-before.json');ab=load(d/'alloc-after.json');r['native_alloc']={}
  for side,rows in ab.items():
   old={v[0]:v for v in aa.get(side,[])};count=by=0
   for v in rows:
    prev=old.get(v[0],(0,0,0,0));count+=v[1]-prev[1];by+=v[2]-prev[2]
   r['native_alloc'][side]={'count_per_msg':count/n,'bytes_per_msg':by/n}
 # Native Java agent uses the same mmap layout but measures JVM heap rather than malloc.
 if job['lang']=='java' and job['mode']=='alloc':
  aa=load(d/'jvm-before.json');ab=load(d/'jvm-after.json');r['jvm_alloc']={}
  for side,rows in ab.items():
   old={v[0]:v for v in aa.get(side,[])};count=by=0
   for v in rows:
    prev=old.get(v[0],(0,0,0,0));count+=v[1]-prev[1];by+=v[2]-prev[2]
   r['jvm_alloc'][side]={'count_per_msg':count/n,'bytes_per_msg':by/n}
 if job['lang']=='node' and job['mode']=='alloc':
  r['v8_alloc']={}
  # Heap profiles start before active and stop before its end; use whole capture count.
  for side,p in b['processes'].items():
   v=load(d/f"node-{p['pid']}.json");hn=load(d/'heap-capture-count.json')['completed'];r['v8_alloc'][side]={**v,'bytes_per_msg':v['bytes']/hn,'count_per_msg':v['count']/hn}
 if job['lang']=='dotnet':
  r['dotnet_alloc']={'bytes_per_msg':(c.get('client_allocated_bytes',0)+c.get('server_allocated_bytes',0))/c['completed']}
  if (d/'allocation-summary.json').exists():
   aa=load(d/'allocation-summary.json');vv=[x['count_per_msg_estimate'] for x in aa['roles'].values()];r['dotnet_alloc']['count_per_msg_estimate']=sum(vv) if all(v is not None for v in vv) else None
 if job['lang']=='java' and (d/'allocation-summary.json').exists():r['jfr_alloc']=load(d/'allocation-summary.json')['roles']
 return r

rows=[]
for p in sorted((P/'runs').glob('*/*/meta.json')):
 if not load(p).get('ok') or (p.parent/'clock-invalid.txt').exists():continue
 try:r=analyze(p.parent);rows.append(r);(p.parent/'summary.json').write_text(json.dumps(r,indent=2))
 except Exception as e:print('ANALYSIS ERROR',p,e)
(P/'summary.json').write_text(json.dumps(rows,indent=2))
for r in rows:
 if r['job']['mode']!='unpaced':print(r['job'],round(r['actual_rate']),round(r['total_us'],3),'us/msg','packets',r.get('tcp',{}).get('data_packets_per_msg'))
print('summarized',len(rows))
