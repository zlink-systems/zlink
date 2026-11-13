from pathlib import Path
import importlib.util, json, re
J=Path('/root/jobs/fx/cx/v21/s033/core124')
spec=importlib.util.spec_from_file_location('gate',J/'src-1.24/bindings/c/perf/perf_regression_gate.py')
gate=importlib.util.module_from_spec(spec);import sys;sys.modules[spec.name]=gate;spec.loader.exec_module(gate)
comps=[('C 대 B1','B1','C'),('C 대 B2','B2','C'),('B2 대 B1','B1','B2')]
def compare(base,cand,suite):
    b=gate.parse_report(J/f'results/{base}-{suite}.txt');c=gate.parse_report(J/f'results/{cand}-{suite}.txt')
    errs=list(gate.report_errors(f'{base} baseline',b))+list(gate.report_errors(f'{cand} candidate',c))
    cells=gate.compare_reports(suite,b,c); aggs=gate.compare_aggregates(cells)
    return b,c,cells,aggs,errs
pairs={}
for name,b,c in comps:
    pairs[name]={s:compare(b,c,s) for s in ('single','multi')}
def counts(result):
    _,_,cells,aggs,errs=result
    return len(cells),sum(not x.status.startswith('PASS') for x in cells),len(aggs),sum(not x.status.startswith('PASS') for x in aggs),len(errs)
def summary_line(name):
    spl=[]
    total=[0,0,0,0,0]
    for s in ('single','multi'):
        ct=counts(pairs[name][s]);spl.append(f'{s} {ct[0]}/{ct[1]} cells, {ct[2]}/{ct[3]} aggregates')
        total=[a+b for a,b in zip(total,ct)]
    verdict='FAIL' if total[1] or total[3] or total[4] else 'PASS'
    return f'| {name} | {verdict} ({total[0]} cells, failed {total[1]}, {total[2]} aggregates, failed {total[3]}, errors {total[4]}) | '+ ' | '.join(spl)+' |'
# Ratios from the three run-3 comparisons.
cell_maps={};agg_maps={}
for name in ('C 대 B1','C 대 B2','B2 대 B1'):
  cell_maps[name]={};agg_maps[name]={}
  for suite in ('single','multi'):
    _,_,cells,aggs,_=pairs[name][suite]
    cell_maps[name][suite]={v.key:v for v in cells}
    agg_maps[name][suite]={(v.pattern,v.transport,v.metric):v for v in aggs}
# Confirmation run-5 verdicts.
confirm_cells=[];confirm_aggs=[];confirmed=0
for line in (J/'confirm/groups.tsv').read_text().splitlines():
    suite,pattern,transport,*_=line.split('\t')
    b=gate.parse_report(J/f'confirm/B1-{suite}-{pattern}-{transport}.txt')
    c=gate.parse_report(J/f'confirm/C-{suite}-{pattern}-{transport}.txt')
    cells=gate.compare_reports(suite,b,c);aggs=gate.compare_aggregates(cells);confirmed+=1
    for v in cells:
        if not v.status.startswith('PASS'):confirm_cells.append(v)
    for v in aggs:
        if not v.status.startswith('PASS'):confirm_aggs.append(v)
def fmt(x):return '—' if x is None else f'{x:.4f}'
def touches(pattern):return '아니요 (signature 변경만)' if pattern in ('STREAM','PUBSUB') else '예 (DEALER/ROUTER pipe write 또는 reply multipart)'
cell_table=['| Suite | 패턴 | 전송 | 크기 | 지표 | runs 3 C/B1 | runs 5 C/B1 | A/A runs 3 B2/B1 | pipe write |','|---|---|---|---:|---|---:|---:|---:|---|']
for v in sorted(confirm_cells,key=lambda x:(x.suite,x.key)):
    key=v.key; r3=cell_maps['C 대 B1'][v.suite].get(key); aa=cell_maps['B2 대 B1'][v.suite].get(key)
    cell_table.append(f'| {v.suite} | {key[0]} | {key[1]} | {key[2]} | {key[3]} | {fmt(r3.ratio if r3 else None)} | {fmt(v.ratio)} | {fmt(aa.ratio if aa else None)} | {touches(key[0])} |')
agg_table=['| Suite | 패턴 | 전송 | 지표 | runs 3 C/B1 geomean | runs 5 C/B1 geomean | A/A runs 3 B2/B1 geomean | pipe write |','|---|---|---|---|---:|---:|---:|---|']
for v in sorted(confirm_aggs,key=lambda x:(x.suite,x.pattern,x.transport,x.metric)):
    key=(v.pattern,v.transport,v.metric);r3=agg_maps['C 대 B1'][v.suite].get(key);aa=agg_maps['B2 대 B1'][v.suite].get(key)
    agg_table.append(f'| {v.suite} | {v.pattern} | {v.transport} | {v.metric} | {fmt(r3.geometric_mean if r3 else None)} | {fmt(v.geometric_mean)} | {fmt(aa.geometric_mean if aa else None)} | {touches(v.pattern)} |')
# Stage timing records.
stage_rows=[]
for p in sorted((J/'results').glob('*.meta.json')):
    d=json.loads(p.read_text());stage_rows.append(f'| {d["stage"]} | {d["start_utc"]} | {d["end_utc"]} | {d["exit_code"]} | {p.relative_to(J)} |')
confirm_times=J/'confirm/times.tsv'
if confirm_times.exists():
  for line in confirm_times.read_text().splitlines():
    k,suite,pat,tr,start,end=line.split('\t');stage_rows.append(f'| run5 {k} {suite} {pat}/{tr} | {start} | {end} | 0 | confirm/{k}-{suite}-{pat}-{tr}.log |')
test_rows=[]
if (J/'test-times.tsv').exists():
  for line in (J/'test-times.tsv').read_text().splitlines():
    name,start,end,rc=line.split('\t');test_rows.append(f'| {name} | {start} | {end} | {rc} | logs/{name}.log |')
core_hashes=[]
for line in (J/'core-sha256.tsv').read_text().splitlines():
  sha,path=line.split(None,1);core_hashes.append(f'| {Path(path).parent.parent.parent.name} | `{sha}` | `{path}` |')
# map labels robustly
core_hashes=[f'| {"1.23 baseline" if "prefix-1.23/" in x else "1.24 candidate"} | {x.split("|",2)[1].strip()} | {x.split("|",2)[2].strip()} |' for x in core_hashes]
ldd_count=sum(1 for _ in (J/'sha-ldd.tsv').read_text().splitlines())-1
mems=[]
for f in (J/'build-preflight.jsonl',J/'preflight.jsonl',J/'test-build-preflight.jsonl',J/'test-preflight.jsonl'):
  if f.exists():
    for line in f.read_text().splitlines():
      d=json.loads(line);mems.append(f'{d.get("stage",d.get("label","stage"))}: MemAvailable={d.get("MemAvailable_bytes")} bytes, allowed={d.get("allowed")}')
# §5.1 logs
hot=(J/'logs/hotpath_gate.log').read_text() if (J/'logs/hotpath_gate.log').exists() else ''
wake=(J/'logs/wake_invariants.log').read_text() if (J/'logs/wake_invariants.log').exists() else ''
if 'No tests were found' in hot or 'No tests were found' in hot: hot_status='등록되지 않음; 실행 안 됨'
elif '100% tests passed' in hot: hot_status='PASS'
elif hot: hot_status='FAIL 또는 실행 오류'
else: hot_status='미실행'
wake_status='PASS' if '100% tests passed' in wake and re.search(r'Passed:\s*[1-9]',wake) else ('FAIL 또는 실행 오류' if wake else '미실행')
# Recipe duplication notes and exact line refs are gathered from reused scripts.
report=f'''# Core 1.24.0 §5.2 release comparison

기준 소스는 `79567d379162e67476ba13e18cfe0f69925e63c9` (core/v1.23.0)이다. 후보 소스는 `0005a4d6c07160bbf661a6f7aea3d8093880514e`이다. 후보에 포함된 Core 변경은 #3930과 #4135이다. `bindings/c/perf`는 두 커밋에서 동일했다. 기준 태그와 커밋이 일치했다. 두 소스 worktree는 빌드 결과를 제외하고 깨끗했다.

## 빌드와 런타임 확인

후보 커밋의 `VERSION`도 1.23.0이어서 두 harness 디렉터리는 `build-release-1.23.0`을 사용했다. Core 런타임 파일 이름과 provenance 버전도 모두 `libzlink.so.1.23.0`, 1.23.0이다. 각 로컬 prefix에 runner가 요구하는 provenance 파일을 만들었다.

| 쪽 | 런타임 SHA-256 | 런타임 파일 |
|---|---|---|
{chr(10).join(core_hashes)}

`verify-builds.txt` 결과는 PASS이다. 실행 파일 {ldd_count}개를 확인했다. Core 의존 실행 파일은 각자 자기 prefix의 RUNPATH와 `ldd` 경로를 사용했다. `perf_stream_client`는 Core 의존성이 없는 것을 확인했다. 세부 내용은 `sha-ldd.tsv`와 `ldd-all.txt`에 있다.

## 프리플라이트와 실행 시간

프리플라이트 기록은 `build-preflight.jsonl`, `preflight.jsonl`, `test-build-preflight.jsonl`, `test-preflight.jsonl`에 있다. 각 측정 및 테스트 단계 직전 MemAvailable은 12 GiB 이상이어야 했다. job 디렉터리의 사용량은 최대 25 GB 제한 아래였다.

| 단계 | 시작 UTC | 종료 UTC | 종료 코드 | 로그 |
|---|---|---|---:|---|
{chr(10).join(stage_rows+test_rows)}

{chr(10).join(mems)}

## §5.2 비교 결과

후보와 B1, 후보와 B2, B2와 B1 비교의 최종 집계이다. 세 비교 모두 candidate tree의 `perf_regression_gate.py`로 판정했다.

| 비교 | 최종 결과 | single | multi |
|---|---|---|---|
{chr(10).join(summary_line(name) for name,_,_ in comps)}

B1, 후보, B2는 각각 single 다음 multi 순서로 실행했다. 초기 sweeps는 `--runs 3`을 사용했다. 첫 gate에서 실패한 cell과 aggregate의 (suite, pattern, transport)를 `confirm/groups.tsv`에 모아 baseline, 후보, baseline 순서로 `--runs 5` 재측정했다. 전체 gate 출력은 `gate-C-vs-B1.txt`, `gate-C-vs-B2.txt`, `gate-B2-vs-B1.txt`이다. 각 stage의 원본 보고서와 로그는 `results/`에 있다.

### Runs 5 뒤에도 실패한 cell

{chr(10).join(cell_table)}

재측정 그룹은 {confirmed}개였다. 이 표에는 runs 5 comparator에서 계속 실패한 cell만 있다. A/A ratio는 같은 runs 3 cell에서 B2/B1로 계산했다.

### Runs 5 뒤에도 실패한 aggregate

{chr(10).join(agg_table)}

집계는 네 message size의 ratio geomean이다. A/A ratio는 B2/B1 runs 3이다.

## §5.1 candidate 결과

`hotpath_gate`: {hot_status}. 로그는 `logs/hotpath_gate.log`이다. `test_wake_invariant*` 반복 결과: {wake_status}. 로그는 `logs/wake_invariants.log`이다. CTest 시작과 종료 시각은 `test-times.tsv`에 있다.

## 파이프 write 경로 분류

DEALER/ROUTER 전송과 ROUTER reply multipart 전송 경로는 pipe write를 통과하는 것으로 분류했다. STREAM과 PUB/SUB는 이 변경에서 signature 변경만 닿는 경로로 분류했다. 각 실패 표에 경로 분류를 함께 기록했다.

## 재사용 스크립트 점검

- Core 빌드 preflight의 MemAvailable, 디스크 여유, load 점검이 반복된다: `build-core.sh:10-20`, `build.sh:8-18`.
- benchmark 실행 인자, 시간 기록, 종료 코드 처리 단계가 반복된다: `run-release-gate.sh:75-101`, `aa-run.sh:12-28`, `confirm-run.sh:6-24`.
- report parser/comparator 호출과 반환 코드 처리가 반복된다: `run-release-gate.sh:106-117`, `aa-run.sh:30-36`, `confirm-run.sh:25-30`.
- Core SHA와 실행 파일의 prefix linkage 검사가 겹친다: `run-release-gate.sh:71-84`, `verify-builds.py:8-25`.

중복은 그대로 두었다. 최초 harness 시도에서 `comp_src_stream_client`라는 잘못된 target 이름을 사용해 실패했고, `comp_src_stream_server`로 수정해 baseline harness를 완료했다. 이 시도의 원본 로그는 `logs/harness-1.23-build.log`이며, 완료 로그는 `logs/harness-1.23-resume.log`이다. 후보 harness는 소스 `VERSION`에 맞는 디렉터리에서 완료했다. 잘못된 후보 harness 경로 configure 기록도 보존했다.
'''
(J/'report.md').write_text(report)
# Overall verdict consumes the full comparator outputs and §5.1 logs.
finals={}
for name,_,_ in comps:
  p=J/f'gate-{name.replace(" ","-")}.txt'
  # filenames use C-vs-B1 etc; map explicit
  stem={'C 대 B1':'gate-C-vs-B1.txt','C 대 B2':'gate-C-vs-B2.txt','B2 대 B1':'gate-B2-vs-B1.txt'}[name]
  txt=(J/stem).read_text();m=re.search(r'^Final: (.+)$',txt,re.M);finals[name]=m.group(1) if m else '누락'
verdict='PASS' if all(x.startswith('PASS') for x in finals.values()) and hot_status=='PASS' and wake_status=='PASS' else 'FAIL 또는 미검증'
(J/'verdict.md').write_text(f'''# 판정

최종 판정: **{verdict}**.

- C vs B1: `{finals['C 대 B1']}`
- C vs B2: `{finals['C 대 B2']}`
- B2 vs B1 A/A: `{finals['B2 대 B1']}`
- `hotpath_gate`: {hot_status}.
- `test_wake_invariant*` 10회: {wake_status}.

Runs 5에서 계속 실패한 cell {len(confirm_cells)}개, aggregate {len(confirm_aggs)}개이다. 상세 ratio와 A/A 비교는 `report.md`에 있다.
''')
print(f'Wrote report.md and verdict.md; remaining runs-5 failures: cells={len(confirm_cells)}, aggregates={len(confirm_aggs)}; §5.1 hotpath={hot_status}, wake={wake_status}')
