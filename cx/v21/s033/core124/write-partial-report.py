from pathlib import Path
import importlib.util, json, re, sys

J = Path('/root/jobs/fx/cx/v21/s033/core124')
spec = importlib.util.spec_from_file_location('gate', J/'src-1.24/bindings/c/perf/perf_regression_gate.py')
gate = importlib.util.module_from_spec(spec)
sys.modules[spec.name] = gate
spec.loader.exec_module(gate)

def result(suite):
    b = gate.parse_report(J/f'results/B1-{suite}.txt')
    c = gate.parse_report(J/f'results/C-{suite}.txt')
    errors = list(gate.report_errors('B1', b)) + list(gate.report_errors('C', c))
    cells = gate.compare_reports(suite, b, c)
    aggs = gate.compare_aggregates(cells)
    return cells, aggs, errors

data = {s: result(s) for s in ('single', 'multi')}
def touch(p):
    return '아니요 (signature 변경만)' if p in ('STREAM', 'PUBSUB') else '예 (pipe write 또는 reply multipart send)'
def ratio(x):
    return '—' if x is None else f'{x:.4f}'

summary=[]
for s,(cells,aggs,errors) in data.items():
    fc=sum(not x.status.startswith('PASS') for x in cells)
    fa=sum(not x.status.startswith('PASS') for x in aggs)
    summary.append(f'| {s} | {len(cells)} | {fc} | {len(aggs)} | {fa} | {len(errors)} |')

cell_rows=['| Suite | 패턴 | 전송 | 크기 | 지표 | runs 3 C/B1 ratio | runs 5 재확인 | A/A runs 3 B2/B1 | pipe write 경로 |',
           '|---|---|---|---:|---|---:|---|---|---|']
agg_rows=['| Suite | 패턴 | 전송 | 지표 | runs 3 C/B1 geomean | runs 5 재확인 | A/A runs 3 B2/B1 | pipe write 경로 |',
          '|---|---|---|---|---:|---|---|---|']
for suite,(cells,aggs,_) in data.items():
    for v in sorted((x for x in cells if not x.status.startswith('PASS')), key=lambda x:x.key):
        p,t,size,metric=v.key
        cell_rows.append(f'| {suite} | {p} | {t} | {size} | {metric} | {ratio(v.ratio)} | 미실행 | 미실행 | {touch(p)} |')
    for v in sorted((x for x in aggs if not x.status.startswith('PASS')), key=lambda x:(x.pattern,x.transport,x.metric)):
        agg_rows.append(f'| {suite} | {v.pattern} | {v.transport} | {v.metric} | {ratio(v.geometric_mean)} | 미실행 | 미실행 | {touch(v.pattern)} |')

stage_rows=[]
for stage in ('B1-single','B1-multi','C-single','C-multi'):
    d=json.loads((J/f'results/{stage}.meta.json').read_text())
    stage_rows.append(f'| {stage} | {d["start_utc"]} | {d["end_utc"]} | {d["exit_code"]} | results/{stage}.log |')
stage_rows += [
    '| B2-single 첫 preflight | 2026-11-09T12:39:18Z | 2026-11-09T13:09:22Z | 3 | gate-run.log |',
    '| B2-single 재개 preflight | 2026-11-09T13:20:35Z | 2026-11-09T13:50:09Z | 3 | resume-b2-final.log |',
    '| B2-multi | 시작 안 됨 | 시작 안 됨 | — | gate-run.log |',
    '| hotpath_gate | 2026-11-09T08:37:44.028082909Z | 2026-11-09T08:37:53.433579827Z | 0 | logs/hotpath_gate.log |',
    '| wake_invariants | 2026-11-09T08:37:53.457855440Z | 2026-11-09T08:38:08.485267520Z | 0 | logs/wake_invariants.log |']

preflight=[]
for line in (J/'preflight.jsonl').read_text().splitlines():
    d=json.loads(line)
    if d.get('stage') in ('B2-single','B2-multi'):
        preflight.append(f'{d["captured_utc"]}: {d["stage"]}, MemAvailable {d["MemAvailable_bytes"]} bytes, load {d["load_1m"]}, 허용 {d["allowed"]}, 외부 build 프로세스 {len(d["heavy_processes"])}개')

report=f'''# Core 1.24.0 release comparison gate 진행 보고서

기준은 `core/v1.23.0` 커밋 `79567d379162e67476ba13e18cfe0f69925e63c9`이다. 후보는 `0005a4d6c07160bbf661a6f7aea3d8093880514e`이다. 후보에는 #3930과 #4135가 포함된다. tag와 baseline 커밋은 일치한다. 두 커밋의 `bindings/c/perf` 트리는 동일하다. 후보 소스의 `VERSION`은 1.23.0이어서 양쪽 harness 디렉터리는 `build-release-1.23.0`을 사용했다.

Core runtime SHA-256은 baseline `{(J/'core-sha256.tsv').read_text().splitlines()[0].split()[0]}`, candidate `{(J/'core-sha256.tsv').read_text().splitlines()[1].split()[0]}`이다. `verify-builds.txt`는 PASS이며 harness 42개를 검사했다. Core 의존 실행 파일은 각자 자기 prefix의 RUNPATH와 ldd 경로를 사용했다. 세부 내용은 `sha-ldd.tsv`, `ldd-all.txt`에 있다. 두 source worktree는 측정 전 clean이었다. Prefix별 로컬 provenance 파일은 runner 요구사항 때문에 작성했다.

| 단계 | 시작 UTC | 종료 UTC | 코드 | 로그 |
|---|---|---|---:|---|
{chr(10).join(stage_rows)}

측정 preflight 기록은 `preflight.jsonl`에 있다. B2-single은 원래 전체 gate 실행에서 30분 동안 통과하지 못했다. 재개 실행에서도 30분 안에 통과하지 못했다. 두 번 모두 `cpp-build-slot`의 외부 `cmake --install` 프로세스가 감지됐다. MemAvailable은 기준 12 GiB를 넘었지만 외부 빌드 감지 조건을 만족하지 못했다. 마지막 시점은 2026-11-09T13:50:09Z이다. B2 benchmark는 시작되지 않았고 B2-multi는 미실행이다. preflight 세부값은 다음과 같다.

{chr(10).join('- '+x for x in preflight[-8:])}

## C 대 B1 첫 gate 결과

candidate comparator의 첫 `--runs 3` 판정은 `gate-C-vs-B1.txt`에 있다. 최종 줄은 `Final: FAIL (cells=1400, failed_cells=222, aggregates=350, failed_aggregates=152, report_errors=0)`이다. B2 결과가 없어 C 대 B2 및 B2 대 B1 비교는 산출되지 않았다.

| Suite | 전체 cell | 실패 cell | 전체 aggregate | 실패 aggregate | 오류 |
|---|---:|---:|---:|---:|---:|
{chr(10).join(summary)}

아래 cell과 aggregate는 모두 첫 runs 3 gate 실패 항목이다. 요청된 runs 5 확인과 A/A ratio는 B2 단계가 시작되지 않아 산출할 수 없었다. 따라서 아래 표는 runs 5 뒤에도 실패한 결과를 뜻하지 않는다. 확인 대상 58개 조합은 `confirm/groups.tsv`, 전체 첫 gate 항목은 `gate-failures.json`에 있다.

### 첫 gate에서 실패한 cell

{chr(10).join(cell_rows)}

### 첫 gate에서 실패한 aggregate

{chr(10).join(agg_rows)}

## §5.1 candidate 결과

`hotpath_gate`는 PASS였다. 로그는 `logs/hotpath_gate.log`이다. `test_wake_invariant*`에 `--repeat until-fail:10`을 적용한 CTest는 세 테스트 모두 PASS였다. 로그는 `logs/wake_invariants.log`이다. build 및 test preflight는 각각 `build-preflight.jsonl`, `test-build-preflight.jsonl`, `test-preflight.jsonl`에 있다. 작업 디렉터리는 25 GB 제한 안이었다.

## 재사용 스크립트 중복 점검

Core build와 harness build의 메모리, 디스크, load preflight가 반복된다: `/root/jobs/fx/cx/core123-gate/build-core.sh:10-20`, `/root/jobs/fx/cx/core123-gate/build.sh:8-18`.

benchmark 실행, argv 구성, 시작·종료 시각 기록, 종료 코드 확인이 반복된다: `/root/jobs/fx/cx/core123-gate/run-release-gate.sh:69-137`, `/root/jobs/fx/cx/core123-gate/aa-run.sh:12-28`, `/root/jobs/fx/cx/core123-gate/confirm-run.sh:6-24`.

comparator 실행과 종료 코드 처리도 반복된다: `/root/jobs/fx/cx/core123-gate/run-release-gate.sh:138-145`, `/root/jobs/fx/cx/core123-gate/aa-run.sh:30-36`, `/root/jobs/fx/cx/core123-gate/confirm-run.sh:25-30`.

Core prefix의 ldd/RUNPATH 확인은 benchmark preflight와 별도 verifier에서 겹친다: `/root/jobs/fx/cx/core123-gate/run-release-gate.sh:75-80`, `/root/jobs/fx/cx/core123-gate/verify-builds.py:15-24`. 중복은 수정하지 않았다.

## 원시 결과

runner 내부 결과도 `raw-runner-results/1.23/` 및 `raw-runner-results/1.24/`에 복사해 보존했다. B2와 runs 5 확인은 preflight 조건 미통과로 미실행이다.
'''
(J/'report.md').write_text(report)
(J/'verdict.md').write_text('''# 판정

최종 판정: **미판정 — release gate 미완료**.

C 대 B1 첫 runs 3 gate는 FAIL이며, 실패 cell 222개와 aggregate 152개가 있다. 지정된 runs 5 재확인은 수행하지 못했다. B2-single preflight가 두 차례 30분 제한에 도달했고 B2-multi도 시작하지 못해 C 대 B2 및 B2 대 B1 A/A 비교가 없다.

§5.1 candidate 검사는 통과했다: `hotpath_gate` PASS, `test_wake_invariant*` 10회 PASS. §5.2의 최종 release 비교 verdict는 B2 baseline과 실패 재확인이 완료되기 전까지 산출할 수 없다. 세부 내용은 `report.md`에 있다.
''')
print(f'partial report written: C/B1 failed cells={sum(sum(not x.status.startswith("PASS") for x in data[s][0]) for s in data)}, failed aggregates={sum(sum(not x.status.startswith("PASS") for x in data[s][1]) for s in data)}')
