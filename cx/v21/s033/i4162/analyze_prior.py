import pathlib,re,json,csv,statistics
j=pathlib.Path(__file__).parent;base=pathlib.Path('/root/jobs/fx/cx/v21/s033/core124b/confirm');rows=[]
for pattern,transport,size in [('PUBSUB','tcp',256),('DEALER_DEALER','tls',64),('DEALER_DEALER','ws',64),('ROUTER_ROUTER_REQREP','tls',64)]:
 for version in ['B1','C','B2']:
  file=base/f'{version}-multi-{pattern}-{transport}.txt';run=None;diag=None
  if not file.exists():continue
  for lineno,line in enumerate(file.read_text().splitlines(),1):
   m=re.search(r'run (\d+)/5:',line)
   if m:run=int(m[1]);diag=None
   if 'median:' in line:break
   if line.startswith(f'BENCH_DIAG,{pattern},{size},'):
    v=line.split(',');diag=dict(zip(v[3::2],map(float,v[4::2])))
   if run is not None and re.match(r'\s*\| '+str(size)+r'B\s*\|',line):
    vals=[float(x) for x in re.findall(r'([\d.]+) (?:Kmsg/s|Kops/s|MB/s|ms)',line)]
    row=dict(pattern=pattern,transport=transport,size=size,version=version,run=run,throughput_k=vals[0],mean_ms=vals[2],p95_ms=vals[3],p99_ms=vals[4],raw=f'{file}:{lineno}',**(diag or {}))
    if diag:
     row['paced_target_ratio']=diag['paced_received_rate']/diag['target_sender_rate'];row['late_fraction']=diag['late_sends_gt_1ms']/diag['latency_sends']
    rows.append(row)
(j/'raw/prior-cells.json').write_text(json.dumps(rows,indent=2)+'\n')
for pattern,transport,size in [('PUBSUB','tcp',256),('DEALER_DEALER','tls',64),('DEALER_DEALER','ws',64),('ROUTER_ROUTER_REQREP','tls',64)]:
 selected=[r for r in rows if r['pattern']==pattern and r['transport']==transport]
 print(pattern,transport)
 for r in selected:
  print(r['version'],r['run'],'mean',r['mean_ms'],'p95',r['p95_ms'],'rate_ratio',round(r.get('paced_target_ratio',0),4),'late_fraction',round(r.get('late_fraction',0),4))
