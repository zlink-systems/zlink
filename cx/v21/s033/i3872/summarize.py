import json, pathlib, re, statistics
root=pathlib.Path('/root/jobs/fx/cx/v21/s033/i3872/raw')
rows=[]
for path in sorted(root.glob('i3872-r*-*/run1/*/results.json')):
    cell=json.loads(path.read_text())['cells'][0]
    stamp=path.parents[2].name
    m=re.fullmatch(r'i3872-r(\d+)-(send1024|send4096|serial1024|serial4096|backpressure1024)-(zlink-framework-node|zlink-node)',stamp)
    if not m: raise ValueError((path,stamp))
    round_no,label,impl=m.groups()
    rows.append({'round':int(round_no),'label':label,'impl':impl,'pattern':cell['pattern'],'payload':cell['payload_size'],'rate':float(cell['throughput_per_second']),'result':str(path)})
for label in ('send1024','send4096','serial1024','serial4096','backpressure1024'):
    fw=[x for x in rows if x['label']==label and x['impl']=='zlink-framework-node']
    bd=[x for x in rows if x['label']==label and x['impl']=='zlink-node']
    fw.sort(key=lambda x:x['round']); bd.sort(key=lambda x:x['round'])
    assert len(fw)==len(bd)==3,(label,len(fw),len(bd))
    ratios=[f['rate']/b['rate'] for f,b in zip(fw,bd)]
    print(label)
    print(' F',*[round(x['rate'],2) for x in fw], 'median',round(statistics.median(x['rate'] for x in fw),2),'range',round(min(x['rate'] for x in fw),2),round(max(x['rate'] for x in fw),2))
    print(' B',*[round(x['rate'],2) for x in bd], 'median',round(statistics.median(x['rate'] for x in bd),2),'range',round(min(x['rate'] for x in bd),2),round(max(x['rate'] for x in bd),2))
    print(' R',*[round(x,4) for x in ratios], 'median_round_ratio',round(statistics.median(ratios),4),'median_rate_ratio',round(statistics.median(x['rate'] for x in fw)/statistics.median(x['rate'] for x in bd),4),'range',round(min(ratios),4),round(max(ratios),4))
print('rows',len(rows))
