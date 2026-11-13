#!/usr/bin/env python3
import json, pathlib, statistics
JOB=pathlib.Path('/root/jobs/fx/cx/v21/s033/lad-node-s1')
ROOT=JOB/'raw/node'
BINDING=pathlib.Path('/root/jobs/fx/cx/v21/s033/binding-daily/node-2026-11-13b.json')
CELLS=[('send-saturation',1024),('send-saturation',4096),('request-serial',1024),('request-serial',4096),('request-backpressure',1024),('request-backpressure',4096)]
VARIANTS=['L0','L1','L2','L3','L4','L5_7']
LABEL={'L5_7':'L5~L7'}
TRANSITIONS=[('L0','binding'),('L1','L0'),('L2','L1'),('L3','L2'),('L4','L3'),('L5_7','L4')]

binding=json.loads(BINDING.read_text())['cells']
def paths(v,p,s,phase='counted'):
    impl='zlink-node' if v=='binding' else 'zlink-framework-node'
    base=ROOT/v/f'{p}-{s}'/phase
    return sorted(base.glob(f'attempt*/run1/{impl}-{p}-{s}/results.json'))
def val(path): return float(json.loads(path.read_text())['cells'][0]['throughput_per_second'])
def link(text,path): return f'[{text}]({path})'
def refs(items):
    out=[]
    for p in items:
        out.append(link('raw',p))
        sched=p.parent/'scheduling.txt'
        out.append(link('sched',sched))
    return ' '.join(out)
def read_number(v,p,s,phase='counted',which='first'):
    ps=paths(v,p,s,phase)
    if not ps: raise SystemExit(f'missing result: {v} {p}-{s} {phase}')
    q=ps[0] if which=='first' else ps[-1]
    return val(q),q
def step_series(v,p,s,newhold_prev=None):
    # Table A reports the first single counted result for each step; paired confirmation
    # medians and G5 are reported separately in Table B.
    q=paths(v,p,s,'counted')
    if not q: raise SystemExit(f'missing counted result: {v} {p}-{s}')
    return val(q[0]),[q[0]]

def pair_series(prev,new,p,s):
    prev_count=paths(prev,p,s,'counted')
    new_count=paths(new,p,s,'counted')
    if not prev_count or not new_count: raise SystemExit(f'missing pair results {prev}->{new} {p}-{s}')
    # The latest previous-step count and earliest new-step count share this hold.
    sets=[]
    for v,base in ((prev,prev_count[-1]),(new,new_count[0])):
        group=[base]
        for phase in ('repeat2','repeat3'):
            ps=paths(v,p,s,phase)
            if ps: group.extend(ps)
        sets.append(group)
    values=[[val(x) for x in group] for group in sets]
    meds=[statistics.median(x) for x in values]
    g5=[max(abs(x-m) for x in series)/m for series,m in zip(values,meds)] if len(values[0])==3 else [0,0]
    loss=1-meds[1]/meds[0]
    budget=.21 if new=='L5_7' else .07
    indeterminate=any(x>.05 for x in g5) if len(values[0])==3 else False
    return loss,budget,indeterminate,sets,meds,g5

rows_a=[]
for p,s in CELLS:
    cell=f'{p}-{s}'
    b= float(binding[cell]['msg_s']); bp=pathlib.Path(binding[cell]['raw']); bs=pathlib.Path(binding[cell]['scheduling'])
    parts=[f'{p}-{s}',f'{link(f"{b:,.0f}",bp)} {link("sched",bs)}']
    fw,_=step_series('L5_7',p,s)
    ratio=fw/b
    for v in VARIANTS:
        x,ps=step_series(v,p,s)
        # For an explicit 3-run confirmation, the representative table value is the median.
        parts.append(f'{link(f"{x:,.0f}",ps[0])} {link("sched",ps[0].parent/"scheduling.txt")}')
    parts.append(f'{ratio:.3f}')
    rows_a.append('| '+' | '.join(parts)+' |')

rows_b=[]
for p,s in CELLS:
    cell=f'{p}-{s}'
    b=float(binding[cell]['msg_s']); bp=pathlib.Path(binding[cell]['raw']); bs=pathlib.Path(binding[cell]['scheduling'])
    l0,l0p=read_number('L0',p,s)
    ratio=l0/b
    status='PASS' if ratio>=.95 else 'FAIL'
    vals=[f'{ratio:.3f} {status} ({link("L0 raw",l0p)} {link("L0 sched",l0p.parent/"scheduling.txt")} {link("binding raw",bp)} {link("binding sched",bs)})']
    for new,prev in TRANSITIONS[1:]:
        loss,budget,ind,sets,meds,g5=pair_series(prev,new,p,s)
        status='판정 불가' if ind else ('PASS' if loss<=budget else 'FAIL')
        a,bset=sets
        evidence=refs(a+bset)
        vals.append(f'{loss*100:+.2f}% {status} ({evidence})')
    rows_b.append('| '+' | '.join([cell]+vals)+' |')

header='| cell | binding msg/s | L0 | L1 | L2 | L3 | L4 | L5~L7 | Framework L5~L7 / binding |'
sep='|---|---:|---:|---:|---:|---:|---:|---:|---:|'
header_b='| cell | L0 / binding | L1 vs L0 | L2 vs L1 | L3 vs L2 | L4 vs L3 | L5~L7 vs L4 |'
sep_b='|---|---|---|---|---|---|---|'
report='''# Node ladder 측정 (현재 조건, quick method)

- 조건: C2·C3 off, `SCHED_OTHER`, source CPU 0–5 / target CPU 8–13, quiet guard v8, warmup 3초·active 5초. 모든 단계는 동일 tip build `7d1afa98a9`의 runtime switch로 측정.
- Binding: [node-2026-11-13b.json](/root/jobs/fx/cx/v21/s033/binding-daily/node-2026-11-13b.json). 수치 링크는 raw `results.json`, `sched` 링크는 같은 run의 모든 thread start/mid-active affinity·policy 기록.

## 표 A — throughput

'''+header+'\n'+sep+'\n'+'\n'.join(rows_a)+'''

## 표 B — 단계 손실 및 기준 판정

손실은 `1 − 새 단계/이전 단계`; L0 기준은 binding의 95%, 일반 단계 budget은 7%, L5~L7 budget은 21%. 3회 cell은 G5 >5%이면 판정 불가.

'''+header_b+'\n'+sep_b+'\n'+'\n'.join(rows_b)+'''

## 측정 기록

[Progress / affinity·scheduling evidence per run](/root/jobs/fx/cx/v21/s033/lad-node-s1/progress.md) · [call-shape](/root/jobs/fx/cx/v21/s033/lad-node-s1/call-shape.md). 모든 warm raw도 보존했으며 판정에는 사용하지 않음.
'''
(JOB/'report.md').write_text(report)
