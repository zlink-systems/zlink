# C++ multi lifecycle 조사 결과

결과: BLOCKED — 정책 결정 필요. 사용자 지시의 정책 충돌 시 종료 조건을 적용했다. 저장소 코드 변경 없음. 조사 기준 HEAD: `711fe8a1e3` (detached). 기존 untracked `core/build`, `core/build-dev`는 유지했다.

## 정책과 변경 이력

- `doc/perf/PERF_MULTI_TEST_POLICY.md:594` (§3.4): “각 size 케이스는 반드시 **독립된 server/client 프로세스 쌍**으로 실행한다.”
- 같은 문서 `:596`: “여러 size를 하나의 server/client 생명주기에 묶어 실행하는 리팩토링은 정책 위반이다.” `:610` (§3.5)도 size마다 process 재시작을 요구한다.
- 정책 도입 commit `2c1ef551e4f` (2026-04-08)의 메시지: “Replace size drain sleep with process restart policy”. size 간 연결·ready·집계·control 상태 공유 금지가 명시된 근거다.
- C++ 주석 `bindings/cpp/perf/run_comparison.py:2862`는 “Multi policy invariant”를 직접 근거로 든다. blame상 `444525153dc` (2026-04-20)에서 도입됐다. commit은 perf runner 정책 갱신을 포함한다. 특정 장애가 직접 원인이었다는 추가 근거는 확인하지 않았다.
- 현재 C도 `bindings/c/perf/run_comparison.py:2909`에서 `run_sizes_test_split(..., [case_size], ...)`, `:2950`에서 size loop를 수행한다. `:2856` 주석의 commit `0ce816cebd3` (2026-08-10)는 `perf: align stream harness size lifecycle`이다. 따라서 현재 C 공식 하네스가 여러 size를 묶는다는 전제는 소스와 다르다.
- C PUBSUB 바이너리 `bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp:299`의 다중 phase/size 처리 능력은 존재하지만, 공식 하네스가 한 번에 넘기는 size 목록은 단일 원소다. 바이너리 능력과 wrapper 호출 lifecycle을 구분해야 한다.
- 계획서 §7.0.1 (`doc/perf/perf/bindings-0.17.0/bindings-library-performance-improvement-plan-core-0.17.0.ko.md:414`) 제목과 본문은 single parity gate이며, 정책을 반영한 C를 canonical로 지정한다. 이것은 multi §3.4의 batching 금지를 해제하지 않는다.
- `doc/perf/BINDINGS_OPTIMIZATION_GUIDE.ko.md:115` §5는 runner 효과와 library 효과를 합산하지 않도록 한다. 이번에는 성능 효과를 산출하지 않았다.

## Lifecycle 차이 표 (정적 조사)

| 대상 | 실제 공식 하네스 lifecycle | 근거 |
|---|---|---|
| C | size마다 process 쌍 재실행 | `bindings/c/perf/run_comparison.py:2909`, `:2950` |
| C++ | size마다 process 쌍 재실행; C와 같은 경계 | `bindings/cpp/perf/run_comparison.py:2862`, `:2939` |
| .NET | size loop 안에서 server/client 실행 | `bindings/dotnet/perf/multi/run_benchmarks.sh:1775`, `:1809` |
| Java | size/run마다 socket 또는 STREAM case 실행, case에서 process 생성 | `bindings/java/perf/multi/run_benchmarks.sh:1319`, `:1105`, `:1125`, `:1137` |
| Node | size마다 spawnMeasuredPair → spawnMultiPair | `bindings/node/perf/multi/run_benchmarks.ts:329`, `:114`; `perf_multi_orchestrator.ts:677`, `:706` |
| Go | size마다 run case; 단일 --msg-size로 process 실행 | `bindings/go/perf/run_benchmarks_multi.sh:1428`, `:1192`, `:1211`, `:1245` |
| Rust | size loop 안에서 server/client 실행 | `bindings/rust/perf/run_benchmarks_multi.sh:841`, `:897` |
| Python | size마다 _run_pattern_captured; 단일 --msg-size로 Popen | `bindings/python/perf/multi/run_benchmarks.py:1396`, `:1417`, `:884`, `:1036` |

다른 binding은 lifecycle만 조사했다. phase 프로토콜과 HWM 수치 전체의 parity를 검증한 표는 아니다.

## 변경

저장소 변경 없음. 요청된 외부 진행 로그와 이 요약 파일만 작성했다. core 아래 build/cmake/clean, branch 전환, commit/push/reset/checkout/stash를 실행하지 않았다.

## 검증 표

| 검증 | 결과 |
|---|---|
| 정책·주석·git history·공식 wrapper 호출 경로 | 확인: 현재 C와 C++ 모두 per-size isolation |
| 다른 binding lifecycle 정적 조사 | .NET/Java/Node/Go/Rust/Python 모두 size별 실행 |
| 지정 PUBSUB,DEALER_DEALER tcp 64,4096 duration 3 runs 1 | 미실행: 정책 충돌 시 수정하지 않고 종료하라는 조건 적용 |
| C 3-run 대비 size별 server/client SNDHWM/RCVHWM | 미검증: 새 paired 실행 없음; 기존 report 수치의 원인도 이번 조사로 확정하지 않음 |
| C++ perf test / ci_multi_smoke | perf 범위 파일명 검색에서 발견되지 않음; 코드 수정 없이 종료하므로 추가 탐색·실행 생략 |
| git diff --check | PASS (exit 0) |
| git status --short | 기존 untracked core/build, core/build-dev만 존재 |

## Spec/정책 gap 여부

Lifecycle에 관한 정책 공백은 없다. §3.4·§3.5가 명시적이다. 요청한 multi-size batching 방향과 현행 정책 사이의 충돌이 있다. 현재 C 소스에서도 batching 위반은 확인되지 않았다. 과거 report 생성 당시 revision·실행 경로·auto-HWM 입력 차이는 확인되지 않았으므로, HWM 불일치를 lifecycle 탓으로 확정할 수 없다.

## BLOCKERS

- **정책 결정 필요**: 여러 size를 한 process/context lifecycle로 묶는 C++ 변경은 현행 multi 정책 위반이다. 사용자 지시에 따라 변경 없이 종료한다.
- 현재 C 공식 하네스도 size별 isolation이다. 기존 paired report의 정확한 revision/실행 경로와 HWM 입력을 별도 조사해야 불일치 원인을 판단할 수 있다.

종료 코드: 2 (정책 blocker; test failure 아님).
