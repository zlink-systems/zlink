# 라운드 6: pipe single flush flags 재사용 확인

- goal: DEALER/PUBSUB/SPOT 공통 64B one-way hot path에서 pipe write/flush 비용을 줄인다.
- 완료 기준: targeted 64B one-way set 중앙값 +10% 이상, 평균 +8% 이상, 관련 core test 통과, 작업 로그 작성.
- 시작 시각: 2026-06-14 16:21:00 +0900
- 기준 commit: `1886624ed`
- 시작 git status: round-5 로그만 새 파일로 있음. core 소스 변경 없음.
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 실패 0 full report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`
- 대상 pattern/transport/size: `MULTI_DEALER_DEALER,MULTI_PUBSUB,MULTI_SPOT` / `tcp,tls,ws,wss` / `64B`

## 가설

- 가설 1: `pipe_t::write_and_flush()`와 `write_and_flush_no_recursive_hwm_check()`는 이미 `more` 값을 계산한 뒤 `write_message_unlocked()`에서 flags와 routing-id를 다시 확인한다. 단일 64B 메시지가 대부분인 경로에서 이 중복을 없애면 pipe write/flush 비용이 줄 수 있다.
- 가설 2: 병목은 pipe 내부 분기보다 reader wakeup 또는 ASIO transport write 쪽에 있어, flags 재사용은 수치에 거의 영향을 주지 않을 수 있다.
- 먼저 검증할 가설: 가설 1. HWM, flush, routing-id count 의미는 그대로 두고, 이미 계산한 `more`만 재사용한다.

## 읽은 코드

- `core/src/runtime/core/pipe.cpp`: `write_and_flush()` 계열은 HWM 확인 뒤 `more`를 계산하고, `write_message_unlocked()`가 다시 `more`와 routing-id를 확인한다.
- `core/src/runtime/core/pipe.hpp`: `_msgs_written`과 `_peers_msgs_read`는 HWM과 activate-write 의미에 묶인 상태라 count 의미를 바꾸면 안 된다.
- `core/src/runtime/sockets/dealer/dealer.cpp`, `core/src/runtime/sockets/internal/lb.cpp`, `core/src/runtime/sockets/internal/dist.cpp`: one-way send가 pipe write/flush 계열을 통과한다.

## 변경

- 검토 파일: `core/src/runtime/core/pipe.cpp`
- 확인 내용: 현재 HEAD는 `write_and_flush()`와 `write_and_flush_no_recursive_hwm_check()`가 이미 계산한 `more` 값을 사용해 `_out_pipe->write()`를 직접 호출하고, 단일 메시지일 때만 routing-id count 확인과 flush를 수행한다.
- perf 전용 변경이 아닌 이유: 이미 core runtime hot path에 들어간 실제 pipe write/flush 구현이다.
- 최종 변경 상태: core 파일은 HEAD와 차이 없음. 이 라운드는 현재 HEAD 상태의 효과를 측정했고 추가 core 변경을 남기지 않았다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다. pipe HWM과 routing-id count 의미도 유지한다.
- 추가로 실행한 회귀 테스트: ypipe, transport matrix, pubsub, SPOT pubsub scenario, backpressure regression focused set

## 검증

- build:
  - `cmake --build core/build -j$(nproc)` 통과
  - 라운드 정리 중 파일을 HEAD와 다시 맞춘 뒤 같은 명령을 재실행해 현재 `core/build` runtime을 소스와 맞췄다.
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'unittest_ypipe|test_multi_socket_contract_regressions|test_pubsub$|test_pubsub_filter_xpub|test_spot_pubsub_scenario|test_backpressure_(oneway_)?matrix_(pubsub|spot)_regression|test_transport_matrix'`
  - 결과: 18/18 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_DEALER_DEALER,MULTI_PUBSUB,MULTI_SPOT --transports tcp,tls,ws,wss --msg-sizes 64 --duration 5`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - result: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_161940.txt`
  - completion: success 12, fail 0, status complete
  - load_avg: 1.90 1.70 1.90
- 비교:
  - 실패 0 full report `perf_c_multi_linux_20260614_151925.txt` 대비 12개 평균 -0.42%, 중앙값 -0.59%
  - 문제 report `perf_c_multi_linux_20260614_103936.txt` 대비 공통 10개 평균 -4.45%, 중앙값 -4.19%
  - `MULTI_PUBSUB tls`는 실패 0 full 기준 -13.74%로 악화했다.

## 결과

- 목표 달성 여부: 미달성
- 판정: 현재 HEAD의 pipe flags 재사용 경로만으로는 공통 64B one-way 회귀가 해결되지 않는다.
- 조치: 추가 core 변경은 남기지 않았다. `core/build`는 현재 소스와 맞춰 다시 빌드했다.
- 다음 후보: PUBSUB와 SPOT one-way가 주로 낮으므로 fanout/matching 쪽의 data path와 control path 혼합 여부를 다시 본다.
