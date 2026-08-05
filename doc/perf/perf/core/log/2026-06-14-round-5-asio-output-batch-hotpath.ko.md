# 라운드 5: ASIO output batch hot path 확인

- goal: DEALER/PUBSUB/SPOT 공통 64B one-way hot path 회귀를 줄인다.
- 완료 기준: targeted 64B one-way set 중앙값 +10% 이상, 평균 +8% 이상, 관련 core test 통과, 작업 로그 작성.
- 시작 시각: 2026-06-14 16:12:39 +0900
- 기준 commit: `1886624ed`
- 시작 git status: `bindings/python/src/zlink/_runtime/sockets/socket_base.py` 변경과 round-4 로그 변경이 있음. core 소스 변경 없음.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 실패 0 full report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`
- 대상 pattern/transport/size: `MULTI_DEALER_DEALER,MULTI_PUBSUB,MULTI_SPOT` / `tcp,tls,ws,wss` / `64B`

## 기준 수치

- 과거 기준 대비 문제 report 공통 64B: 26개 평균 -15.62%, 중앙값 -14.86%
- 과거 기준 대비 문제 report one-way 64B: 평균 -27.36%
- 문제 report 대비 실패 0 full report 공통 64B: 평균 +1.50%, 중앙값 +1.58%
- 문제 report 대비 실패 0 full report one-way 64B: 평균 -2.29%
- 실패 0 full report는 `success 192`, `fail 0`이지만 load_avg가 `3.20 2.64 1.81`로 높아 성능 판단에는 targeted 반복 측정을 우선 사용한다.

## 가설

- 가설 1: ASIO 출력 경로가 작은 메시지를 batch로 채울 때 `process_output()`과 `prepare_output_buffer()`에 같은 로직을 중복으로 가지고 있어 stream/non-stream 정책 적용과 encoder fill 비용이 흩어져 있다. 이 경로를 하나로 모으면 공통 tcp/tls/ws/wss write hot path의 분기와 유지 비용이 줄 수 있다.
- 가설 2: pipe `write_and_flush()`에서 단일 메시지도 일반 `write_message_unlocked()`를 거치며 `more`와 routing-id 판정을 반복한다. DEALER/PUBSUB/SPOT 64B one-way는 대부분 단일 메시지라 이 비용을 줄이면 pipe enqueue/dequeue 비용이 줄 수 있다.
- 먼저 검증할 가설: 가설 1. ASIO output batch는 모든 transport one-way에 공통이고, perf 전용 조건 변경 없이 실제 runtime 중복 제거로 남길 수 있는지 판단한다.

## 읽은 코드

- `core/src/runtime/engine/asio/asio_engine.cpp`: `prepare_output_buffer()`와 `process_output()`가 encoder에서 메시지를 batch로 끌어오는 로직을 각각 갖고 있다.
- `core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp`: non-stream은 `options.out_batch_size`, stream은 동적 stream target을 output batch 목표로 쓴다.
- `core/src/runtime/core/options.cpp`: 기본 `out_batch_size`는 8192다.
- `core/src/runtime/core/pipe.cpp`: 단일 메시지 fast path와 일반 write/flush 경로가 나뉘어 있다.

## 변경

- 임시 변경 파일: `core/src/runtime/engine/asio/asio_engine.cpp`
- 변경 내용: `process_output()`의 encoder batch fill 중복 구현을 `prepare_output_buffer()` 호출로 합쳤다.
- perf 전용 변경이 아닌 이유: ASIO runtime의 실제 output 준비 경로 중복을 줄이는 구조 변경이다.
- 최종 변경 상태: core 파일은 HEAD와 차이 없음. targeted perf에서 의미 있는 개선이 없어 임시 변경을 되돌렸다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다.
- 추가로 실행한 회귀 테스트: transport matrix, pubsub, SPOT pubsub scenario, stream fastpath, backpressure regression focused set

## 검증

- build:
  - 임시 변경 후 `cmake --build core/build -j$(nproc)` 통과
  - 임시 변경을 되돌린 뒤 `cmake --build core/build -j$(nproc)` 통과
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'test_transport_matrix|test_pubsub$|test_pubsub_filter_xpub|test_spot_pubsub_scenario|test_stream_fastpath|test_multi_socket_contract_regressions|test_backpressure_(oneway_)?matrix_(pubsub|spot)_regression'`
  - 결과: 18/18 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_DEALER_DEALER,MULTI_PUBSUB,MULTI_SPOT --transports tcp,tls,ws,wss --msg-sizes 64 --duration 5`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - result: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_161426.txt`
  - completion: success 12, fail 0, status complete
  - load_avg: 0.50 1.07 1.83
- 비교:
  - 실패 0 full report `perf_c_multi_linux_20260614_151925.txt` 대비 12개 평균 +1.15%, 중앙값 -0.34%
  - 문제 report `perf_c_multi_linux_20260614_103936.txt` 대비 공통 10개 평균 -3.15%, 중앙값 -3.57%
  - 개선이 반복 기준인 10%에 미달했고, `MULTI_PUBSUB ws`는 실패 0 full 기준 -9.64%로 악화했다.

## 결과

- 목표 달성 여부: 미달성
- 판정: ASIO output batch 중복 제거는 구조적으로 가능하지만 이번 64B one-way 회귀의 주된 병목이 아니다.
- 조치: 임시 core 변경은 되돌렸고, 되돌린 뒤 `core/build`를 다시 빌드했다.
- 다음 후보: pipe 단일 메시지 write/flush 경로를 보되, 이전 라운드의 dist/lb 단일 fast path 악화 결과를 감안해 HWM/flush 의미를 바꾸지 않는 후보만 검토한다.
