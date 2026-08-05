# 라운드 4: send monitor counter 비용 확인

- goal: 공통 one-way 64B hot path에서 monitor 진단용 send counter 비용이 의미 있는지 확인한다.
- 시작 시각: 2026-06-14 16:03:44 +0900
- 기준 commit: `5e471eeca`
- 시작 git status: round-3 로그 변경만 있음. core 소스 변경 없음.
- 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 비교 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`
- 대상 pattern/transport/size: `MULTI_DEALER_DEALER,MULTI_PUBSUB,MULTI_SPOT` / `tcp,tls,ws,wss` / `64B`

## 가설

- 가설 1: `socket_base_t::send_direct_with_retry()`의 `_auto_hwm_send_attempts.fetch_add()`가 모든 public send 성공 경로에 들어가서 one-way 64B 처리량을 낮춘다.
- 가설 2: send command polling은 `process_commands(0, true)` 안에서 이미 TSC 기반 throttle을 적용하므로, 현재 회귀의 주된 원인은 command poll 자체가 아니라 pipe/write 또는 transport batching 쪽이다.
- 선택한 가설: 먼저 가설 1을 임시 변경으로 검증한다. 효과가 10% 이상 반복되면 monitor 계약을 유지하는 대안을 설계한다. 효과가 없으면 변경을 남기지 않는다.

## 읽은 코드

- `core/src/runtime/sockets/common/socket_base_msg.cpp`: 모든 public send 시도에서 auto-HWM send attempt counter를 증가한다. blocked counter는 EAGAIN 경로에서 증가한다.
- `core/src/runtime/sockets/common/socket_base_monitor.cpp`: monitor snapshot은 두 counter로 `auto_hwm_send_blocked_ratio_ppm`을 계산한다.
- `doc/guide/06-monitoring*.md`, `doc/spec/bindings/README*.md`: blocked ratio가 공개 진단 필드로 문서화되어 있다.
- `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`: `process_commands(0, true)`는 TSC 기반 throttle을 이미 적용한다.

## 변경

- 임시 변경 파일: `core/src/runtime/sockets/common/socket_base_msg.cpp`
- 변경 이유: send attempt atomic counter의 hot path 비용만 분리해서 측정한다.
- perf 전용 변경이 아닌 이유: 최종 변경으로 남기기 전 병목 확인을 위한 임시 실험이다. 효과가 있더라도 public monitor 계약을 유지하는 설계를 별도로 적용해야 한다.
- 최종 변경 상태: core 파일은 HEAD와 차이 없음. 효과가 없어 임시 변경을 되돌렸다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: 메시지 guard, decoder guard, WS/WSS pending message, mtrie, port parsing, IPC unlink, maxmsgsize 정책을 건드리지 않는다.
- 추가로 실행한 회귀 테스트: monitor, pubsub, backpressure, socket runtime focused set

## 검증

- build: `cmake --build core/build -j$(nproc)` 통과
  - 참고: 임시 변경 빌드 중 clock skew 경고가 있었다. 임시 변경을 되돌린 뒤 같은 명령을 다시 실행해 현재 `core/build` runtime을 소스와 맞췄다.
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'test_multi_socket_contract_regressions|test_pubsub$|test_pubsub_filter_xpub|test_monitor_perf_contract|unittest_socket_runtime|test_backpressure_(oneway_)?matrix_(pubsub|spot)_regression'`
  - 결과: 9/9 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_DEALER_DEALER,MULTI_PUBSUB,MULTI_SPOT --transports tcp,tls,ws,wss --msg-sizes 64 --duration 5`
    - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
    - result: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_160436.txt`
    - completion: success 12, fail 0, status complete
    - load_avg: 1.70 1.50 2.29
- 비교:
  - 비교 기준: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`
  - 공통 12개 64B 항목 평균 +1.04%, 중앙값 +0.17%, 평균 throughput 비율 +1.31%
  - 악화: `MULTI_PUBSUB ws` -5.93%, `MULTI_PUBSUB tls` -3.55%
  - 개선: `MULTI_SPOT wss` +16.39%였지만 단일 transport 상승이고 전체 평균/중앙값이 오르지 않았다.

## 결과

- 목표 달성 여부: 미달성
- 판정: send attempt atomic counter는 공통 one-way 64B 회귀의 주된 원인이 아니다.
- 조치: 임시 core 변경은 되돌렸고, 되돌린 뒤 `core/build`를 다시 빌드했다. monitor blocked ratio 계약도 변경하지 않는다.
- 다음 후보: pipe/write 또는 ASIO output batching처럼 DEALER/PUBSUB/SPOT one-way에 공통인 실제 송신 경로를 계속 본다.
