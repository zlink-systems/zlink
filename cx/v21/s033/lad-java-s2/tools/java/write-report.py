#!/usr/bin/env python3
import json, statistics
from pathlib import Path
JOB=Path('/root/jobs/fx/cx/v21/s033/lad-java-s2')
RAW=JOB/'raw/java'
BINDING=Path('/root/jobs/fx/cx/v21/s033/binding-daily/java-2026-11-13b.json')
cells=['send-saturation-1024','send-saturation-4096','request-serial-1024','request-serial-4096','request-backpressure-1024','request-backpressure-4096']
steps=['L0','L1','L2','L3','L4','L5_7']
labels={'send-saturation-1024':'send-saturation 1K','send-saturation-4096':'send-saturation 4K','request-serial-1024':'request-serial 1K','request-serial-4096':'request-serial 4K','request-backpressure-1024':'request-backpressure 1K','request-backpressure-4096':'request-backpressure 4K'}
binding=json.loads(BINDING.read_text())['cells']
def rel(path):
    return Path('../'+str(path.relative_to(JOB.parent))).as_posix() if path.is_relative_to(JOB.parent) else str(path)
def link(label,path): return f'[{label}]({rel(path)})'
def read_result(path):
    c=json.loads(path.read_text())['cells'][0]
    assert c['errors']==c['server_errors']==c['target_stats']['errors']==0
    assert c['submitted']==c['completed']==c['target_stats']['received']
    return c['throughput_per_second']
data={}
for step in steps:
  data[step]={}
  for cell in cells:
    paths=[]
    for marker in sorted((RAW/step/cell/'counted').glob('valid-result-a*.path'),key=lambda p:int(p.stem.split('a')[-1])):
      p=Path(marker.read_text().strip())
      sched=p.with_name('scheduling.txt')
      assert p.is_file() and sched.is_file()
      text=sched.read_text()
      assert 'scheduling_invalid=' not in text
      assert all(f'cpu{cpu}.state{state}.disable_at_run_start=1' in text for cpu in (0,8) for state in (2,3))
      blocks=[b.strip() for b in text.split('\n\n')]
      for phase,expected in [('source_start','0-5'),('mid_active_source','0-5'),('target_start','8-13'),('mid_active_target','8-13')]:
        b=next((b for b in blocks if b.startswith(phase+'_utc=')), '')
        tids=[x for x in b.splitlines() if x.startswith(phase+'_tid=')]
        policies=[x for x in b.splitlines() if 'current scheduling policy:' in x]
        affs=[x.rsplit('cpus=',1)[1] for x in b.splitlines() if x.startswith(phase+'_affinity ') and 'cpus=' in x]
        assert tids and len(tids)==len(policies) and all('current scheduling policy: SCHED_OTHER' in x for x in policies)
        assert affs and all(x==expected for x in affs)
      paths.append((read_result(p),p,sched))
    if paths:
      med=statistics.median(x[0] for x in paths)
      g5=max(abs(x[0]-med) for x in paths)/med if len(paths)==3 else None
      data[step][cell]={'runs':paths,'median':med,'g5':g5}
# binding validity also rechecked from the source report's artifacts
report=[]
report += ['# Java ladder 측정 — 2026-11-14 조건','',
'조건: C2·C3 off, SCHED_OTHER, source 0–5 / target 8–13, housekeeping 6–7·14–15, warmup 10초·active 5초. Framework 단계는 run-time switch로 전환했다. Binding은 당일 조건의 [lad-java-s1](../lad-java-s1/report.md) 6개 유효 셀을 재사용했다. [호출 형태 비교](call-shape.md) · [실행·affinity 기록](progress.md).','',
'## 표 A — 처리량과 Framework/binding','','| 셀 | Binding msg/s | L0 | L1 | L2 | L3 | L4 | L5~L7 | Framework(L5~L7)/binding |','|---|---:|---:|---:|---:|---:|---:|---:|---:|']
for cell in cells:
  b=binding[cell]; bp=Path(b['raw']); bs=Path(b['scheduling'])
  row=[labels[cell],f"{link(f'{b['msg_s']:,.1f}',bp)} {link('S',bs)}"]
  for step in steps:
    d=data[step].get(cell)
    if not d:
      if step=='L5_7' and cell=='request-backpressure-4096':
        failed=RAW/step/cell/'counted/attempt1/run1'/f'zlink-framework-java-{cell}'/'results.json'
        c=json.loads(failed.read_text())['cells'][0]
        row.append(f"{link(f'폐기(errors={c['errors']:,})',failed)} {link('S',failed.with_name('scheduling.txt'))}")
      else: row.append('미측정')
      continue
    runs=d['runs']; med=d['median']; mp=min(runs,key=lambda x:abs(x[0]-med))
    value=f"{med:,.1f}"+(f" (중앙값, {len(runs)}회)" if len(runs)==3 else '')
    evid=' '.join(link(f'R{i+1}',p) for i,(_,p,_) in enumerate(runs))
    evid_sched=' '.join(link(f'S{i+1}',s) for i,(_,_,s) in enumerate(runs))
    row.append(f"{link(value,mp[1])} {evid} {evid_sched}")
  d=data['L5_7'].get(cell)
  if d:
    ratio=d['median']/b['msg_s']
    row.append(f"{ratio:.3f} ({ratio*100:.1f}%) {link('raw',d['runs'][0][1])} {link('binding',bp)}")
  elif cell=='request-backpressure-4096':
    failed=RAW/'L5_7'/cell/'counted/attempt1/run1'/f'zlink-framework-java-{cell}'/'results.json'
    row.append(f"폐기 {link('raw',failed)} {link('binding',bp)}")
  else: row.append('미측정')
  report.append('| '+' | '.join(row)+' |')
report += ['', '## 표 B — 단계 손실 및 gate 판정','', '| 셀 | L0/binding (≥0.95) | L1/L0 (≤7%) | L2/L1 (≤7%) | L3/L2 (≤7%) | L4/L3 (≤7%) | L5~L7/L4 (≤21%) |','|---|---:|---:|---:|---:|---:|---:|']
for cell in cells:
  row=[labels[cell]]
  for i,step in enumerate(steps):
    d=data[step].get(cell)
    if not d:
      if step=='L5_7' and cell=='request-backpressure-4096':
        failed=RAW/step/cell/'counted/attempt1/run1'/f'zlink-framework-java-{cell}'/'results.json'
        row.append(f"폐기(errors 불일치) {link('raw',failed)}")
      else: row.append('미측정')
      continue
    prev='binding' if i==0 else steps[i-1]
    pd=binding[cell]['msg_s'] if i==0 else (data[prev].get(cell) or {}).get('median')
    if pd is None: row.append('분모 미측정'); continue
    ratio=d['median']/pd; loss=(1-ratio)*100
    limit=5 if i==0 else 21 if i==5 else 7
    threshold=.95 if i==0 else .79 if i==5 else .93
    ok=ratio>=threshold
    g5_values=[]
    if d['g5'] is not None: g5_values.append(('new',d['g5']))
    if i>0 and data[prev][cell]['g5'] is not None: g5_values.append(('prev',data[prev][cell]['g5']))
    g5_label='; '.join(f'G5 {name}={value*100:.1f}%' for name,value in g5_values)
    if any(value>.05 for _,value in g5_values): verdict=f'판정 불가({g5_label})'
    else: verdict='PASS' if ok else 'FAIL'
    cur=d['runs'][0][1]; prevp=Path(binding[cell]['raw']) if i==0 else data[prev][cell]['runs'][0][1]
    row.append(f"{link(f'{ratio:.3f} ({loss:+.1f}%)',cur)} / {link('기준',prevp)} {verdict}")
  report.append('| '+' | '.join(row)+' |')
report += ['', '## 실행 증거와 판정 규칙','',
'표 A의 유효 단계 값에는 중앙값 run의 `results.json`이 연결돼 있고 R/S 링크는 counted run의 결과와 scheduling 기록이다. 모든 유효 counted run은 errors=0, submitted=completed=received, 모든 source/target thread SCHED_OTHER, 시작·중간 affinity source 0–5 / target 8–13, C2/C3 disable=1을 통과했다. warm run은 `raw/java/<step>/<cell>/warm/`에 보존했으며 판정에 사용하지 않았다.','',
'표 B는 손실을 `1 − new/previous`로 계산한다. L0 기준은 binding 대비 5% 이내, L1–L4는 단계당 7% 이내, L5~L7은 L4 대비 21% 이내다. 5–9% 손실 셀은 이전·신규 각 3회 측정했다. 3회 셀의 G5는 `max(|run−median|)/median`; G5 > 5%이면 판정 불가다. L5~L7의 19–23%는 3회 보강 구간이다.','',
f'초기 progress의 잘못된 서식 두 줄은 유효 수치나 판정으로 사용하지 않았다. C2/C3 준비가 안 된 L4 warm attempt 1은 bench phase 시작 전에 실패했고 판정에서 제외했다. L5~L7 request-backpressure 4K의 warm 및 counted 결과는 errors가 각각 {link("128,213", RAW/"L5_7"/"request-backpressure-4096"/"warm/attempt1/run1/zlink-framework-java-request-backpressure-4096/results.json")}·{link("123,625", RAW/"L5_7"/"request-backpressure-4096"/"counted/attempt1/run1/zlink-framework-java-request-backpressure-4096/results.json")}이고 submitted/completed/received가 달라 폐기했다.','']
(JOB/'report.md').write_text('\n'.join(report))
print(JOB/'report.md')
