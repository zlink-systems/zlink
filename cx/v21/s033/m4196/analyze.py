import json,pathlib,statistics,csv
root=pathlib.Path('/root/jobs/fx/cx/v21/s033/m4196/raw/final2')
def snap_cpu(p): return json.loads((p).read_text())
rows=[]
for p in sorted(root.glob('round*/*/*/results.json')):
    cell=p.parent
    variant=cell.parents[0].name
    rnd=int(cell.parents[1].name.removeprefix('round'))
    name=cell.name
    impl,pattern,payload=name.rsplit('-',2); payload=int(payload)
    result=json.loads(p.read_text())['cells'][0]
    src0=json.loads((cell/'source-before.json').read_text()); src1=json.loads((cell/'source-after.json').read_text())
    tgt0=json.loads((cell/'target-before.json').read_text()); tgt1=json.loads((cell/'target-after.json').read_text())
    st0=json.loads((cell/'source-stats-before.json').read_text()); st1=json.loads((cell/'source-stats-after.json').read_text())
    n=max(1,int(st1.get('completed',0))-int(st0.get('completed',0)))
    cpu={}
    for role,b,a in [('source',src0,src1),('target',tgt0,tgt1)]:
        total=0
        for tid in set(b['threads'])|set(a['threads']):
            before=b['threads'].get(tid,{}).get('runtime_ns',0)
            after=a['threads'].get(tid,{}).get('runtime_ns')
            if after is not None: total+=max(0,after-before)
        cpu[role]=total/1000/n
    rows.append({'round':rnd,'variant':variant,'implementation':impl,'pattern':pattern,'payload':payload,'msg_s':float(result['throughput_per_second']),'completed':result.get('completed_messages',result.get('completed')),'cpu_source_us_msg':cpu['source'],'cpu_target_us_msg':cpu['target'],'window_messages':n,'result_path':str(p)})
keys=sorted({(r['implementation'],r['pattern'],r['payload']) for r in rows})
with (root/'summary.csv').open('w',newline='') as f:
    w=csv.DictWriter(f,fieldnames=list(rows[0]));w.writeheader();w.writerows(rows)
for impl,pat,payload in keys:
    sub=[r for r in rows if (r['implementation'],r['pattern'],r['payload'])==(impl,pat,payload)]
    vs={v:sorted([r for r in sub if r['variant']==v],key=lambda x:x['round']) for v in ('base','pr')}
    if any(len(vs[v])!=3 for v in vs): continue
    m={v:statistics.median([x['msg_s'] for x in vs[v]]) for v in vs}
    ranges={v:(min(x['msg_s'] for x in vs[v]),max(x['msg_s'] for x in vs[v])) for v in vs}
    widths={v:ranges[v][1]-ranges[v][0] for v in vs}
    diff=abs(m['pr']-m['base']); exceeds=diff>max(widths.values())
    paired=[vs['pr'][i]['msg_s']/vs['base'][i]['msg_s'] for i in range(3)]
    print(f"{impl} {pat} {payload}: base {[round(x['msg_s']) for x in vs['base']]} med={m['base']:.1f} spread={ranges['base'][0]:.1f}-{ranges['base'][1]:.1f} | pr {[round(x['msg_s']) for x in vs['pr']]} med={m['pr']:.1f} spread={ranges['pr'][0]:.1f}-{ranges['pr'][1]:.1f} ratio={m['pr']/m['base']:.4f} paired_ratio_range={min(paired):.4f}-{max(paired):.4f} gap={diff:.1f} max_spread={max(widths.values()):.1f} exceeds={exceeds}")
    for v in ('base','pr'):
        for role in ('source','target'):
            values=[x[f'cpu_{role}_us_msg'] for x in vs[v]]
            print(f"  {v} {role}_cpu_us_msg median={statistics.median(values):.3f} range={min(values):.3f}-{max(values):.3f}")
