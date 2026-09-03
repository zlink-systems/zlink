# C perf REQREP latency correction

## Result

Single과 multi REQREP는 throughput·bandwidth를 기존 포화 구간에서 측정하고, mean/p95/p99 latency를 별도의 1초 구간에서 측정한다. Latency 구간에는 single 기준 request 1개, multi 기준 client socket마다 request 1개만 in-flight 상태로 둔다. RESULT metric 이름, 열 순서와 gate cell key는 유지했다.

## Design

- Single Phase 1은 request API가 backpressure를 반환할 때까지 submit하는 기존 포화 루프를 사용한다. Phase 1 완료 시 `in_flight == 0`이 될 때까지 모든 REQUEST completion을 drain하고, Phase 1의 deadline 안에 끝난 completion 수를 throughput 출처로 사용한다.
- Single Phase 2는 1초 동안 실행한다. 이전 REQUEST completion을 poller에서 받은 뒤 `in_flight`가 0이 된 경우에만 다음 request payload에 timestamp를 기록하고 제출한다. Mean/p95/p99에는 이 구간의 성공 completion만 들어간다.
- Multi의 기존 `run_active_window`는 client별 `outstanding` 값을 제한하지 않은 채 매 loop마다 request를 제출했으므로 포화 상태였다. 이를 공용 `run_measurement_window`로 분리했다. Phase 1은 제한 없이 제출하고 완전히 drain하며, Phase 2는 client별 `outstanding < 1`인 경우에만 제출한다.
- Main과 baseline은 모두 Core 0.15.1이지만 C 공개 request completion 표면이 다르다. Perf CMake가 `zlink_completion_t` 제공 여부를 compile-check한다. Main은 completion record API, baseline은 callback API를 선택하며 두 경로 모두 공개 API만 사용한다.
- C++ formatter와 single/multi `run_comparison.py`의 최종 RESULT 재출력은 latency, latency_p95, latency_p99에 소수 6자리를 사용한다. Throughput·bandwidth와 사람이 읽는 표의 표시 정밀도는 기존 값을 유지한다. 두 parser와 regression gate는 값을 Python `float`로 읽으므로 형식 변경이 필요하지 않았다.

## Changed files

- `bindings/c/perf/CMakeLists.txt`
- `bindings/c/perf/single/common/perf_single_reqrep.hpp`
- `bindings/c/perf/single/common/perf_single_monitor.hpp`
- `bindings/c/perf/single/src/perf_dealer_router_reqrep.cpp`
- `bindings/c/perf/single/src/perf_router_router_reqrep.cpp`
- `bindings/c/perf/single/run_comparison.py`
- `bindings/c/perf/single/tests/test_run_comparison_policy.py`
- `bindings/c/perf/single/tests/test_multi_run_comparison_policy.py`
- `bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp`
- `bindings/c/perf/multi/common/perf_multi_metrics.hpp`
- `bindings/c/perf/run_comparison.py`

같은 파일을 `/home/hep7hep7/project/zlink-perf-core-0.15.1`의 동일 경로에 통째로 복사했으며, `cmp`로 byte 일치를 확인했다. Multi client/server source는 공용 header만 포함하므로 별도 수정이 필요하지 않았다.

## Validation measurements

64B, tcp, `--duration 1 --runs 1`, `ZLINK_CORE_SOURCE=local` 조건이다. 각 benchmark 직전에 `ps -eo comm | grep -E '^(perf_|python3)$'` 결과가 비어 있음을 확인했다. 단위는 ms다.

| Worktree | Throughput (ops/s) | Mean | P95 | P99 |
|---|---:|---:|---:|---:|
| main | 447415.000 | 0.082576 | 0.116012 | 0.188143 |
| baseline Core 0.15.1 | 473626.000 | 0.098372 | 0.134834 | 0.213352 |

두 결과의 mean/p95/p99는 모두 1ms 미만이다. Raw runner output은 다음 파일에 있다.

- `/home/hep7hep7/project/zlink-work/c016/main-dealer-router-reqrep-short.txt`
- `/home/hep7hep7/project/zlink-work/c016/baseline-dealer-router-reqrep-short.txt`

## Other verification

- Main: `perf_dealer_router_reqrep`, `perf_router_router_reqrep`, multi DEALER_ROUTER/ROUTER_ROUTER REQREP client/server target을 `-j4`로 빌드했다.
- Baseline: 같은 target을 `-j4`로 빌드했다. CMake compile-check는 baseline의 callback 공개 API를 선택했고 모두 성공했다.
- `python3 -m unittest bindings/c/perf/tests/test_perf_regression_gate.py`: 12 tests passed.
- Gate test와 single/multi runner policy test를 함께 실행한 결과: 59 tests passed.
- `git diff --check -- bindings/c/perf`: 두 worktree에서 통과했다.
- 모든 build, test와 benchmark 명령은 `ulimit -v 16777216` 아래에서 실행했다. `scripts/local-package/**`와 `--core-version`은 사용하지 않았다.
- Benchmark가 사용한 runtime은 main `/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.15.1`, baseline `/home/hep7hep7/project/zlink-perf-core-0.15.1/core/build/lib/libzlink.so.0.15.1`이다.

## QUESTIONS

- `bindings/c/perf/README.md`의 “One-Way Latency Caveat”는 one-way RESULT가 포화 active phase latency를 보고하고 single-pass 계약을 따른다고 설명한다. 이전 one-way Phase 2 구현과 현재 코드에 맞지 않지만 이번 작업에서는 README 수정 금지 지시를 따라 그대로 두었다. 별도 승인 범위에서 현재 two-phase 계약으로 개정할 필요가 있다.
