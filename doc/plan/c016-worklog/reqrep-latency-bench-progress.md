# REQREP latency benchmark progress

- 2026-09-03: `perf/phase2-judge` branch와 두 worktree의 기존 변경을 확인했다. main과 baseline 모두 이전 one-way 작업 파일 3개가 수정 상태이며 이번 작업에서 보존한다.
- 2026-09-03: 시작 시 `ps -eo comm | grep -E '^(perf_|python3)$'` 결과가 비어 있음을 확인했다.
- 2026-09-03: 지정한 summary, one-way Phase 2 구현, single/multi REQREP, formatter, README, gate와 test를 읽었다. single은 backpressure까지 연속 submit하고 multi도 client별 outstanding 제한 없이 반복 submit하므로 둘 다 포화 구간 latency를 기록하고 있음을 확인했다.
- 2026-09-03: `run_comparison.py`와 `perf_regression_gate.py`는 RESULT 숫자를 Python `float`로 읽으므로 latency 소수 6자리와 호환됨을 확인했다. README의 one-way latency 설명은 현재 Phase 2 정책과 불일치하지만 사용자 지시에 따라 수정하지 않고 QUESTIONS에 기록한다.
- 2026-09-03: single/multi REQREP에 포화 Phase 1과 1초·in-flight 1 Phase 2를 분리해 구현했다. RESULT latency 3종은 single 공용 formatter, single REQREP formatter, multi formatter에서 소수 6자리로 변경했다.
- 2026-09-03: main worktree에서 single REQREP 2개와 multi REQREP client/server 4개 target을 `-j4`로 빌드했고 모두 성공했다.
- 2026-09-03: baseline 0.15.1은 callback request API, main은 completion record API를 제공한다. CMake compile-check로 공개 API variant를 선택하도록 만들고, 같은 perf 파일을 두 worktree에 복사한 상태에서 양쪽 REQREP target 6개 빌드를 완료했다.
- 2026-09-03: gate 필수 test 12개와 single/multi runner policy test를 포함한 58개 test가 통과했다. runner가 집계 RESULT를 재출력할 때도 latency 3종만 소수 6자리를 유지하도록 수정했다.
- 2026-09-03: 각 실행 직전 활성 `perf_`/`python3` 프로세스가 없음을 확인하고 DEALER_ROUTER_REQREP tcp 64B `--duration 1 --runs 1`을 실행했다. main mean/p95/p99는 0.082576/0.116012/0.188143ms, baseline은 0.098372/0.134834/0.213352ms였다.
- 2026-09-03: 변경 파일 9개의 두 worktree byte 일치와 양쪽 `git diff --check -- bindings/c/perf` 통과를 확인했다.
- 2026-09-03: RESULT 재출력 정밀도를 고정하는 single/multi runner test를 보강했다. 첫 multi assertion은 report의 기존 display pattern 규칙(`MULTI_` 제거)을 반영하지 않아 실패했으며 기대값을 실제 계약에 맞춘 뒤 전체 59개 test 통과를 확인했다. 최종 변경 파일은 test 2개를 포함한 11개다.
