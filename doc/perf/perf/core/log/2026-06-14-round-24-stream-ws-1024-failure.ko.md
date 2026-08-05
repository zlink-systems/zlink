# Round 24: STREAM ws 1024B failure 재현 확인

- goal: current full multi gate에서 발생한 `MULTI_STREAM ws 1024B` 실패를 단독 반복해 실패 안정화가 필요한지 확인한다.
- 완료 기준: `MULTI_STREAM ws 1024B` 단독 perf를 실행해 재현 여부를 기록한다. 재현되면 core STREAM/WS 경로와 perf failure output을 분리한다. 재현되지 않으면 full sweep 순서/부하 artifact로 기록하고 다음 gate 후보를 정한다.
- 시작 시각: 2026-06-14 19:16:53 +0900
- 기준 commit: `b787430c4`
- failing report commit: `84e10b266`
- 시작 git status: `bindings/node/dist-tools/tests/optimization_guard.test.js`, `bindings/node/tests/optimization_guard.test.ts`, `bindings/node/tests/run_tests.sh` 변경이 있음. perf/core 작업과 무관하므로 건드리지 않는다. round 9-23 로그 파일이 untracked 상태다.
- failing report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_184516_round23_full_failure_gate.txt`
- 대상 pattern/transport/size: `MULTI_STREAM` / `ws` / `1024B`

## 가설

- 가설 1: STREAM ws 1024B는 current core runtime에서 안정적으로 실패하며, STREAM/WS send/read 또는 shutdown path에 core 안정화 후보가 있다.
- 가설 2: full sweep 후반부의 상태, 순서, 또는 load 때문에 생긴 one-off failure이며, 단독 repeat에서는 재현되지 않는다.
- 선택한 가설: 먼저 가설 2를 확인한다. 실패 안정화는 중요하지만, source 수정 전 단독 재현성을 확인해야 한다.

## 변경

- core 소스 변경: 없음
- perf 소스 변경: 없음
- 변경 이유: 실패 원인 분리 전 단독 재현성을 확인한다.
- perf 전용 변경이 아닌 이유: perf 코드는 수정하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: 측정 라운드이며 WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
- 추가로 실행한 회귀 테스트: 소스 변경이 없으면 별도 test는 실행하지 않는다.

## 검증 예정

- build:
  - `cmake --build core/build -j$(nproc)`
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=1024 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round24_stream_ws_1024_repeat`

## 결과

- build:
  - `cmake --build core/build -j$(nproc)`
  - 결과: 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=1024 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round24_stream_ws_1024_repeat`
  - 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_191724_round24_stream_ws_1024_repeat.txt`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - `META,commit`: `b787430c4`
  - load_avg: `2.13 4.57 4.48`
  - completion: success 1, fail 0, status complete
  - throughput: `210,413.8 ops/s`

## 판정

- full multi gate의 `MULTI_STREAM ws 1024B` failure는 단독 3-run repeat에서 재현되지 않았다.
- 아직 full failure 0으로 볼 수는 없다. 실패가 full sweep 후반부의 STREAM ws size sequence나 장시간 실행 상태와 관련될 수 있으므로, 다음은 `STREAM/ws` 전체 size set을 단독 반복한다.
- core 소스 변경 없음. perf 소스 변경 없음.

## STREAM ws size-set repeat

- command:
  - `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round24_stream_ws_all_sizes_repeat`
- 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_191759_round24_stream_ws_all_sizes_repeat.txt`
- runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- `META,commit`: `b787430c4`
- completion: success 0, fail 6, status partial
- 실패:
  - `MULTI_STREAM current ws 64B: non_zero_exit_2_size_64`
  - `256B`, `1024B`, `4096B`, `65536B`, `131072B`도 모두 result line이 없고 `non_zero_exit_2_size_64`로 귀결됐다.

## 갱신 판정

- `STREAM/ws/1024B` 단독 repeat는 통과했지만, `STREAM/ws` 전체 size set은 첫 size부터 실패한다.
- 실패는 특정 1024B payload 문제가 아니라 `STREAM/ws` multi-size 실행의 초기 64B 단계에서 발생한 것으로 본다.
- 다음 단계는 perf artifact와 STREAM/WS startup/connect-ready path를 읽어 core runtime 실패인지, perf runner가 이전 실패 상태를 크기별로 전파하는지 분리한다.

## 추가 재현성 확인

- 확인 중 HEAD가 `33821a70f`, 이후 `5054e4062`로 이동했다. dirty tree에는 Node/JavaScript sample/test 변경이 있었고 core/perf 소스 변경은 없었다.
- `git diff -- core/src core/include --stat` 결과는 비어 있었다.

### 단독 64B

- command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round24_stream_ws_64_repeat`
- 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_192018_round24_stream_ws_64_repeat.txt`
- `META,commit`: `33821a70f`
- completion: success 1, fail 0, status complete
- throughput: `247,607.0 ops/s`
- 판정: `64B` 자체는 단독 실행에서 실패하지 않았다.

### 64B,256B size set

- command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round24_stream_ws_64_256_repeat`
- 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_192030_round24_stream_ws_64_256_repeat.txt`
- `META,commit`: `33821a70f`
- completion: success 0, fail 2, status partial
- 실패:
  - `MULTI_STREAM current ws 64B: non_zero_exit_2_size_64`
  - `MULTI_STREAM current ws 256B: non_zero_exit_2_size_64`
- 판정: 같은 runner에서 64B 단독은 통과하고 `64,256`은 실패했다. 이 시점에는 size set 또는 실행 timing 영향을 의심했다.

### 64B,256B debug repeat

- command:
  - `PERF_DEBUG_TRANSITIONS=1 PERF_CAPTURE_MAX_BYTES=8388608 PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round24_stream_ws_64_256_debug`
- 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_192112_round24_stream_ws_64_256_debug.txt`
- `META,commit`: `5054e4062`
- completion: success 2, fail 0, status complete
- throughput:
  - `64B`: `250,205.0 ops/s`
  - `256B`: `246,668.8 ops/s`

### 64B,256B non-debug repeat

- command:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64,256 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round24_stream_ws_64_256_repeat2`
- 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_192137_round24_stream_ws_64_256_repeat2.txt`
- `META,commit`: `5054e4062`
- completion: success 2, fail 0, status complete
- throughput:
  - `64B`: `240,430.0 ops/s`
  - `256B`: `252,474.2 ops/s`

### full STREAM/ws size set repeat

- command:
  - `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports ws --duration 5 --runs 2 --connect-ready-timeout-ms 5000 --results-tag round24_stream_ws_all_sizes_repeat2`
- 결과 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_192154_round24_stream_ws_all_sizes_repeat2.txt`
- `META,commit`: `5054e4062`
- completion: success 6, fail 0, status complete
- throughput:
  - `64B`: `250,055.2 ops/s`
  - `256B`: `247,078.2 ops/s`
  - `1024B`: `237,793.0 ops/s`
  - `4096B`: `138,077.6 ops/s`
  - `65536B`: `14,104.4 ops/s`
  - `131072B`: `6,638.8 ops/s`

## 최종 판정

- `STREAM/ws` 실패는 현재 HEAD에서 즉시 반복 재현되지 않았다.
- core runtime 또는 STREAM/WS payload 처리 결함으로 볼 안정적인 증거가 아직 없다.
- failure 0 요구를 만족하려면 current HEAD에서 full multi gate를 다시 실행해야 한다.
- core 소스 변경 없음. perf 소스 변경 없음.
