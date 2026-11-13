#!/usr/bin/env python3
"""Use the original aggregator's medians; retain exact raw provenance."""
import json,pathlib,statistics
JOB=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868')
RAW=JOB/'raw'
patterns=['send-saturation','request-serial','request-backpressure']
phases=['L0','main']
BASE=pathlib.Path('/root/jobs/fx/cx/v21/s033/r4190w/raw')
def read(path):return json.loads(path.read_text())
def stats(aggregate,pattern,size,implementation,paths):
    rows=read(aggregate)['rows']
    row=next(r for r in rows if (r['pattern'],r['payload_size'],r['implementation'])==(pattern,size,implementation))
    assert row['runs']==3,(aggregate,row)
    entries=[]
    for p in paths:
        p=p.resolve();x=read(p);c=x['cells'][0]
        assert not c.get('contaminated') and not c.get('drain_bound_hit'),p
        assert c['submitted']==c['completed']==c['target_stats']['received'],p
        assert c['errors']==c['target_stats']['errors']==c.get('header_validation_failures',0)==c.get('abandoned',0)==0,p
        assert c['trigger']['warmup']==30000 and c['trigger']['durationMs']==5000,p
        assert x['metadata']['sourceCpus']=='0-7' and x['metadata']['targetCpus']=='8-15',p
        entries.append({'raw':str(p),'msg_s':c['throughput_per_second'],'submitted':c['submitted'],
                        'completed':c['completed'],'received':c['target_stats']['received'],'errors':0,
                        'ladder_level':x['metadata'].get('ablationLadderLevel',8)})
    value=row['median']['throughput_per_second']
    assert value==statistics.median(e['msg_s'] for e in entries),(value,entries)
    return {'median_msg_s':value,'median_raw':next(e['raw'] for e in entries if e['msg_s']==value),
            'runs':entries,'g5':row['g5_status'],'g5_spread_percent':row['g5_spread_percent']}
def rate(s):
    import os
    path=os.path.relpath(s['median_raw'],JOB)
    return f"[{s['median_msg_s']:,.0f}]({path})"
def ratio(value):return f'[{value:.4f}](raw/summary.json)'
summary={'criterion':0.95,'matrix':[],'cross':[]}
lines=['| 패턴 | 크기 | L0 / binding msg/s | L0÷binding · 0.95 | main L8 참조 / 당시 binding msg/s | main÷binding · 0.95 |',
       '|---|---|---:|---:|---:|---:|']
for pattern in patterns:
 for size in [1024,4096]:
    cell={'pattern':pattern,'payload_size':size}
    for phase in phases:
        d={}
        for label,impl in [('framework','zlink-framework-cpp'),('binding','zlink-cpp')]:
            paths=[(RAW/'L0-owner'/f'{pattern}-{size}' if phase=='L0' else BASE/'bench/cpp')/f'run{run}'/f'{impl}-{pattern}-{size}'/'results.json' for run in [1,2,3]]
            aggregate=RAW/'aggregates/L0.json' if phase=='L0' else BASE/'aggregates/cpp.json'
            d[label]=stats(aggregate,pattern,size,impl,paths)
        d['ratio']=d['framework']['median_msg_s']/d['binding']['median_msg_s']
        d['pass_0_95']=d['ratio']>=0.95
        cell[phase]=d
    summary['matrix'].append(cell)
    a,b=cell['L0'],cell['main']
    lines.append(f"| {pattern} | {size//1024}K | {rate(a['framework'])} / {rate(a['binding'])} | {ratio(a['ratio'])} · {'통과' if a['pass_0_95'] else '미달'} | {rate(b['framework'])} / {rate(b['binding'])} | {ratio(b['ratio'])} · {'통과' if b['pass_0_95'] else '미달'} |")
lines+=['','| 모드 | 패턴 · 1K | raw 서버/raw client | FW 서버/raw client | FW 서버/FW client | target 비율 | source 비율 |',
        '|---|---|---:|---:|---:|---:|---:|']
for phase in ['L0']:
 for pattern in ['send-saturation']:
    cell={'phase':phase,'pattern':pattern,'payload_size':1024}
    for combination in ['raw-raw','framework-raw','framework-framework']:
        impl='zlink-framework-cpp' if combination=='framework-framework' else 'zlink-cpp'
        paths=[RAW/'cross'/'L0-owner'/combination/f'run{run}'/f'{impl}-{pattern}-1024'/'results.json' for run in [1,2,3]]
        cell[combination]=stats(RAW/'aggregates'/f'cross-{phase}-{combination}.json',pattern,1024,impl,paths)
    a,b,c=[cell[k]['median_msg_s'] for k in ['raw-raw','framework-raw','framework-framework']]
    cell['target_retention']=b/a;cell['source_retention']=c/b
    cell['target_loss_percentage_points']=(a-b)/a*100
    cell['source_loss_percentage_points']=(b-c)/a*100
    summary['cross'].append(cell)
    lines.append(f"| {phase} | {pattern} | {rate(cell['raw-raw'])} | {rate(cell['framework-raw'])} | {rate(cell['framework-framework'])} | {ratio(b/a)} | {ratio(c/b)} |")
(RAW/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n')
(RAW/'tables.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
