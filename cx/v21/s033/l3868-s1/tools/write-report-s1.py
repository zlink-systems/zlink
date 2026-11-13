import datetime,json,pathlib
J=pathlib.Path('/root/jobs/fx/cx/v21/s033/l3868-s1'); S=json.loads((J/'raw/summary.json').read_text()); C=json.loads((J/'raw/comparisons.json').read_text())
levels=['binding','0','1','2','3','4','7']; cells=[(p,s) for p in ['send-saturation','request-serial','request-backpressure'] for s in [1024,4096]]
def v(l,p,s): return S.get(f'{l}|{p}|{s}')
def rel(p):
    try: return str(pathlib.Path(p).relative_to(J))
    except ValueError: return p
def rate(x): return f'[{x["rate"]:,.0f}]({rel(x["raw"])})'+(' (3회)' if x['n']==3 else '') if x else '미완료'
rows=[]
for p,s in cells:
    vals=[v(l,p,s) for l in levels]; a=vals[0]; z=vals[-1]
    ratio=f'[{z["rate"]/a["rate"]:.3f}](raw/summary.json)' if a and z else '—'
    rows.append('| '+f'{p} {s//1024}K'+' | '+' | '.join(rate(x) for x in vals)+' | '+ratio+' |')
brows=[]
for p,s in cells:
    entries=[]
    for cur in levels[1:]:
        c=next((x for x in C if x['current']==cur and x['pattern']==p and x['size']==s),None)
        entries.append(f'[{100*c["loss"]:.2f}% {c["status"]}](raw/comparisons.json)' if c else '미완료')
    brows.append('| '+f'{p} {s//1024}K'+' | '+' | '.join(entries)+' |')
reps=[]
for k,x in S.items():
    if x['n']>=3:
        reps.append(f'| {k} | {x["g5"]*100:.2f}% | '+', '.join(f'[run{i+1}]({rel(p)})' for i,p in enumerate(x['raw_paths']))+' |')
runs=[json.loads(s) for s in (J/'raw/runs.jsonl').read_text().splitlines()]; counted=[r for r in runs if r['kind'].startswith('counted')]
holds=[]
for p in sorted((J/'raw/holds').glob('*/duration.json')):
    d=json.loads(p.read_text()); holds.append(f'[{d["seconds"]/60:.2f}분]({rel(p)})')
fail=sum(c['status']=='실패' for c in C); unknown=sum(c['status']=='판정 불가' for c in C)
text=f'''# C++ ladder 현재 조건 측정

유효 셀 {len(S)}/42, 비교 {len(C)}/36: 통과 {len(C)-fail-unknown}, 실패 {fail}, 판정 불가 {unknown}.
HEAD `62e766f88dc6d1feefdff76e6132185019db0bb6`, branch `exp/3868-cpp-ladder-next`, 코드 변경 없음.
C2·C3 off, SCHED_OTHER, source 0–5 / target 8–13 직접 pin, guard v8, performance governor.
Warmup 3초·active 5초. 셀 또는 단계가 바뀔 때 버린 warm run 1회 후 counted 1회.
5–9% 단계 손실(묶음 19–23%)의 양쪽만 3회로 늘리고 중앙값을 사용한다. 3회 G5>5%는 판정 불가다.

**표 A — 처리량(msg/s)과 binding 비율**

| 셀 | binding | L0 | L1 | L2 | L3 | L4 | L5~L7 | L5~L7÷binding |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
'''+ '\n'.join(rows)+'''

**표 B — 손실률(1−새 단계/이전 단계)과 판정**

L0는 binding 대비 ≥95%, L1~L4는 이전 단계 대비 손실 ≤7%, L5~L7은 L4 대비 ≤21%다.
음수 손실은 처리량 증가다. 각 링크에 양쪽 원시 경로와 계산 입력을 기록했다.

| 셀 | binding→L0 (5%) | L0→L1 (7%) | L1→L2 (7%) | L2→L3 (7%) | L3→L4 (7%) | L4→L5~L7 (21%) |
|---|---:|---:|---:|---:|---:|---:|
'''+ '\n'.join(brows)+f'''

**근거**

- [binding 기준](/root/jobs/fx/cx/v21/s033/binding-daily/cpp-2026-11-13b.json), [모든 counted·warm 원시 경로](raw/runs.jsonl), [수치·반복 전체 경로](raw/summary.json), [호출 대조](call-shape.md), [실행별 commit/base/hash·수신/응답 대조](progress.md).
- [빌드 종료 코드](raw/build-success.json), [바이너리 전체 freshness·SHA256](raw/freshness.json), [동일 tree의 source binding/Release 근거](raw/binding-build-source.json). 지정 impl3 configure script를 cpp-build-slot.sh로 실행했다(Release, source binding ON, Core 1.25.0, -j3, ≤25 GiB). 빌드와 freshness가 모두 rc=0인 뒤 측정했다.
- [configure](/root/jobs/fx/cx/v21/s033/l3868-impl3/raw/configure-s1-20261113T1856.log), [build](/root/jobs/fx/cx/v21/s033/l3868-impl3/raw/build-s1-20261113T1856.log). 코드 변경이 없어 추가 unit/E2E는 실행하지 않았다.
- 유효 counted {len(counted)}회 모두 errors=0, submitted=completed=received. 각 results.json 옆 scheduling.txt에 source/target 모든 thread의 시작·mid-active chrt 정책과 Cpus_allowed_list를 기록했다. 원시 목록의 scheduling 필드로 연결한다.
- 락 보유 시간: {', '.join(holds)}. 각 hold의 before/after.json에 governor와 16 CPU C2/C3 상태를 보존했다. 빌드·집계는 락 밖에서 수행했다.
- `build-l3868-next` 및 기존·이번 원시 파일을 유지한다. commit·push·GitHub·sub-agent·internal-docs 실행 없음.
'''
if reps:
    text+='\n**3회 셀 근거**\n\n| 단계·셀 | G5 | 원시 경로 |\n|---|---:|---|\n'+'\n'.join(reps)+'\n'
(J/'report.md').write_text(text)
print(str(J/'report.md'))
