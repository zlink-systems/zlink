#!/usr/bin/env python3
import json, pathlib, statistics
JOB=pathlib.Path('/root/jobs/fx/cx/v21/s033/lad-node-s2')
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
    if not q: return None,None
    return val(q[0]),[q[0]]

def start_time(path):
    schedule=(path.parent/'scheduling.txt').read_text()
    for line in schedule.splitlines():
        if line.startswith('run_start_utc='): return line.split('=',1)[1]
    raise SystemExit(f'missing run_start_utc: {path.parent/"scheduling.txt"}')
def pair_series(prev,new,p,s):
    prev_count=paths(prev,p,s,'counted')
    new_count=paths(new,p,s,'counted')
    if not prev_count or not new_count: return None
    # Previous step's latest run and new step's first run belong to this hold.
    prev_base,new_base=prev_count[-1],new_count[0]
    hold_start=start_time(new_base)
    next_step={'L1':'L2','L2':'L3','L3':'L4','L4':'L5_7'}.get(new)
    next_paths=paths(next_step,p,s,'counted') if next_step else []
    hold_end=start_time(next_paths[0]) if next_paths else None
    sets=[]
    for v,base in ((prev,prev_base),(new,new_base)):
        group=[base]
        for phase in ('repeat2','repeat3'):
            ps=[x for x in paths(v,p,s,phase)
                if start_time(x)>hold_start and (hold_end is None or start_time(x)<hold_end)]
            if ps: group.extend(ps)
        sets.append(group)
    values=[[val(x) for x in group] for group in sets]
    meds=[statistics.median(x) for x in values]
    repeated=(len(values[0])==3 and len(values[1])==3)
    asymmetric=(len(values[0])!=len(values[1]) and (len(values[0])>1 or len(values[1])>1))
    g5=[max(abs(x-m) for x in series)/m for series,m in zip(values,meds)] if repeated else [0,0]
    loss=1-meds[1]/meds[0]
    budget=.21 if new=='L5_7' else .07
    indeterminate=asymmetric or (repeated and any(x>.05 for x in g5))
    return loss,budget,indeterminate,sets,meds,g5

rows_a=[]
for p,s in CELLS:
    cell=f'{p}-{s}'
    b= float(binding[cell]['msg_s']); bp=pathlib.Path(binding[cell]['raw']); bs=pathlib.Path(binding[cell]['scheduling'])
    parts=[f'{p}-{s}',f'{link(f"{b:,.0f}",bp)} {link("sched",bs)}']
    fw,_=step_series('L5_7',p,s)
    ratio=(fw/b) if fw is not None else None
    for v in VARIANTS:
        x,ps=step_series(v,p,s)
        if x is None:
            invalid=[]
            for phase in ('warm','counted'):
                impl='zlink-framework-node'
                invalid += sorted((ROOT/v/f'{p}-{s}'/phase).glob(f'attempt*/run1/{impl}-{p}-{s}/results.json'))
            if invalid:
                parts.append(f'N/A {link("invalid raw",invalid[-1])} {link("sched",invalid[-1].parent/"scheduling.txt")}')
            else: parts.append('N/A')
        else:
            parts.append(f'{link(f"{x:,.0f}",ps[0])} {link("sched",ps[0].parent/"scheduling.txt")}')
    parts.append(f'{ratio:.3f}' if ratio is not None else 'N/A')
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
        result=pair_series(prev,new,p,s)
        if result is None:
            invalid=[]
            if new=='L5_7':
                for phase in ('warm','counted'):
                    invalid += sorted((ROOT/new/f'{p}-{s}'/phase).glob(f'attempt*/run1/zlink-framework-node-{p}-{s}/results.json'))
            evidence=refs(invalid[-1:]) if invalid else ''
            vals.append(f'N/A (유효 counted 없음) {evidence}')
            continue
        loss,budget,ind,sets,meds,g5=result
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

## L5~L7 미완료 cell

- send-saturation-1K warm은 제출·완료 75,055건, target 수신 0으로 무효 처리했다. [raw 결과](/root/jobs/fx/cx/v21/s033/lad-node-s2/raw/node/L5_7/send-saturation-1024/warm/attempt1/run1/zlink-framework-node-send-saturation-1024/results.json) · [affinity/scheduling](/root/jobs/fx/cx/v21/s033/lad-node-s2/raw/node/L5_7/send-saturation-1024/warm/attempt1/run1/zlink-framework-node-send-saturation-1024/scheduling.txt)
- request-serial-1K warm은 두 번 모두 source stats endpoint가 30초 안에 준비되지 않아 active 전에 끝났다. [시도 1 log](/root/jobs/fx/cx/v21/s033/lad-node-s2/raw/node/L5_7/request-serial-1024/warm/attempt1/driver.log) · [시도 2 log](/root/jobs/fx/cx/v21/s033/lad-node-s2/raw/node/L5_7/request-serial-1024/warm/attempt2/driver.log)
- L5~L7의 유효 counted 결과는 없다. 나머지 cell은 warm 실패 뒤 시작하지 않아 표에 N/A로 표시했다. Framework 코드 변경은 하지 않았다.

## 측정 기록

[Progress / affinity·scheduling evidence per run](/root/jobs/fx/cx/v21/s033/lad-node-s2/progress.md) · [call-shape](/root/jobs/fx/cx/v21/s033/lad-node-s2/call-shape.md). 모든 warm raw도 보존했으며 판정에는 사용하지 않음.
'''
(JOB/'report.md').write_text(report)
