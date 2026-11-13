#!/usr/bin/env python3
"""Validate raw counts/provenance, keep all runs, and use the repository G5 aggregator."""
import datetime,hashlib,json,pathlib,statistics,subprocess,sys
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868-l0e');R=J/'raw'
REPO=pathlib.Path('/root/worktree/zlink-v21-s033/l3868')
AGG=REPO/'framework/bench/grpc/tools/bench_aggregate.py'
manifest=json.loads((R/'build-provenance.json').read_text())
def stats(paths,out,impl):
    entries=[]
    for p in sorted(paths):
        x=json.loads(p.read_text());c=x['cells'][0];prov=json.loads((p.parent/'binary-provenance.json').read_text())
        validated=json.loads((p.parent/'validated.json').read_text())
        assert validated['result_sha256']==hashlib.sha256(p.read_bytes()).hexdigest(),p
        assert prov['commit']==manifest['commit'] and prov['binaries']==manifest['binaries'],p
        assert not c.get('contaminated') and not c.get('drain_bound_hit'),p
        assert c['errors']==c['target_stats']['errors']==0,p
        assert c['submitted']==c['completed']==c['target_stats']['received'],p
        assert c.get('header_validation_failures',0)==c.get('abandoned',0)==0,p
        assert c['trigger']['warmup']==30000 and c['trigger']['durationMs']==5000,p
        assert x['metadata']['sourceCpus']=='0-7' and x['metadata']['targetCpus']=='8-15',p
        entries.append({'raw':str(p),'msg_s':c['throughput_per_second'],'submitted':c['submitted'],'completed':c['completed'],'received':c['target_stats']['received'],'errors':0})
    rows=json.loads(out.read_text())['rows'];row=next(r for r in rows if r['implementation']==impl)
    assert row['runs']==len(entries),(out,impl,len(entries))
    median=statistics.median(e['msg_s'] for e in entries)
    assert median==row['median']['throughput_per_second']
    return {'median_msg_s':median,'median_raw':next(e['raw'] for e in entries if e['msg_s']==median),'runs':entries,'g5':row['g5_status'],'g5_spread_percent':row['g5_spread_percent']}
def link(path):return str(pathlib.Path(path).relative_to(J))
def rate(stat):return f"[{stat['median_msg_s']:,.0f}]({link(stat['median_raw'])})"
summary={'built_commit':manifest['commit'],'criterion':.95,'created_utc':datetime.datetime.now(datetime.timezone.utc).isoformat(),'matrix':[],'cross':{},'incomplete':[]}
lines=['| 패턴 | 크기 | 횟수 | L0 msg/s | binding msg/s | L0÷binding · 0.95 | G5 L0 / binding |','|---|---|---:|---:|---:|---:|---|']
for pattern in ['send-saturation','request-serial','request-backpressure']:
 for size in [1024,4096]:
    root=R/'L0'/f'{pattern}-{size}';agg=R/'aggregates'/f'{pattern}-{size}.json'
    if not agg.exists():summary['incomplete'].append(str(root));continue
    cell={'pattern':pattern,'payload_size':size}
    for key,impl in [('l0','zlink-framework-cpp'),('binding','zlink-cpp')]:
        cell[key]=stats(list(root.glob(f'run*/{impl}-{pattern}-{size}/results.json')),agg,impl)
    cell['ratio']=cell['l0']['median_msg_s']/cell['binding']['median_msg_s']
    cell['pass_0_95']=cell['ratio']>=.95;cell['g5_pass']=all(cell[k]['g5']=='pass' for k in ['l0','binding'])
    summary['matrix'].append(cell)
    g5=' / '.join(f"{cell[k]['g5']} {cell[k]['g5_spread_percent']:.2f}%" for k in ['l0','binding'])
    lines.append(f"| {pattern} | {size//1024}K | [{len(cell['l0']['runs'])}]({link(agg)}) | {rate(cell['l0'])} | {rate(cell['binding'])} | [{cell['ratio']:.4f}]({link(agg)}) · {'통과' if cell['pass_0_95'] else '미달'} | [{g5}]({link(agg)}) |")
lines+=['','| send-saturation 1K 교차 조합 | 횟수 | 중앙값 msg/s | G5 |','|---|---:|---:|---|']
for combo,impl in [('raw-raw','zlink-cpp'),('framework-raw','zlink-cpp'),('framework-framework','zlink-framework-cpp')]:
    root=R/'cross/L0'/combo;paths=list(root.glob(f'run*/{impl}-send-saturation-1024/results.json'))
    if len(paths)<3:summary['incomplete'].append(str(root));continue
    agg=R/'aggregates'/f'cross-{combo}.json'
    with (R/'aggregates'/f'cross-{combo}.md').open('w') as log:
        subprocess.run([sys.executable,str(AGG),'--lang','cpp','--min-runs','3','--baseline','zlink-cpp','--runs-glob',str(root/'run*'),'--json-out',str(agg)],stdout=log,check=True)
    st=stats(paths,agg,impl);summary['cross'][combo]=st
    lines.append(f"| {combo} | [{len(st['runs'])}]({link(agg)}) | {rate(st)} | [{st['g5']} {st['g5_spread_percent']:.2f}%]({link(agg)}) |")
if len(summary['cross'])==3:
    a,b,c=[summary['cross'][x]['median_msg_s'] for x in ['raw-raw','framework-raw','framework-framework']]
    summary['target_retention']=b/a;summary['source_retention']=c/b
    lines+=['',f"Target 유지율 [{b/a:.4f}](raw/summary.json), source 유지율 [{c/b:.4f}](raw/summary.json). 각각 FW/raw ÷ raw/raw, FW/FW ÷ FW/raw다."]
(R/'summary.json').write_text(json.dumps(summary,ensure_ascii=False,indent=2)+'\n')
(R/'tables.md').write_text('\n'.join(lines)+'\n')
print('\n'.join(lines))
