# 라운드 1: SPOT/PUBSUB 64B one-way hot path

- goal: SPOT/PUBSUB와 공통 one-way 64B hot path 회귀를 줄인다.
- 완료 기준: 현재 문제 report 대비 공통 64B targeted set 중앙값 +10% 이상, 관련 core tests 통과, runner runtime 경로가 `core/build` 아래임을 확인한 targeted perf 결과 기록.
- 시작 시각: 2026-06-14 14:58:27 +0900
- 기준 commit: `82175d004`
- 시작 git status: clean
- 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 비교 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 대상 pattern/transport/size: `MULTI_SPOT`, `MULTI_PUBSUB`, `MULTI_DEALER_DEALER` / `tcp,tls,ws,wss` / `64B`

## report 비교

- 공통 64B 항목: 26개
- 항목별 delta 평균: -15.6%
- 항목별 delta 중앙값: -14.9%
- one-way 64B 항목별 delta 평균: -27.4%
- echo 64B 항목별 delta 평균: -8.3%
- throughput 평균 비율 기준 전체 64B: -28.3%
- throughput 평균 비율 기준 one-way 64B: -30.5%
- throughput 평균 비율 기준 echo 64B: -10.6%

큰 64B 하락 항목:

- `MULTI_SPOT tcp 64B`: 7379.815 -> 3896.079 Kmsg/s (-47.2%)
- `MULTI_SPOT tls 64B`: 6924.687 -> 3739.004 Kmsg/s (-46.0%)
- `MULTI_PUBSUB tls 64B`: 3333.681 -> 2446.708 Kmsg/s (-26.6%)
- `MULTI_PUBSUB tcp 64B`: 3518.023 -> 2628.105 Kmsg/s (-25.3%)
- `MULTI_DEALER_DEALER tcp 64B`: 3965.902 -> 3045.747 Kmsg/s (-23.2%)
- `MULTI_DEALER_DEALER tls 64B`: 4082.066 -> 3162.931 Kmsg/s (-22.5%)

실패 항목:

- `MULTI_SPOT ws/wss`: 모든 size 실패
- `MULTI_SPOT_REQREP ws/wss`: 모든 size 실패
- `MULTI_SPOT_SENDSEND ws/wss`: 모든 size 실패
- `MULTI_STREAM ws`: 1024B 이상 실패
- current report completion: success 152, fail 40, status partial

## 가설

- 가설 1: one-way 공통 경로에서 작은 메시지의 pipe enqueue/dequeue 또는 message 소유권 이동 비용이 늘어 `MULTI_DEALER_DEALER`, `MULTI_PUBSUB`, `MULTI_SPOT`에 함께 반영되었다.
- 가설 2: SPOT data plane fanout 경로가 제어 상태나 준비 상태 확인을 data hot path에 반복해서 섞어 `MULTI_SPOT` tcp/tls 64B를 크게 낮췄고, ws/wss 준비 실패도 같은 경로의 readiness 또는 endpoint 처리 문제와 연결되어 있다.
- 가설 3: ASIO write batching 또는 mailbox wakeup 정책이 64B one-way에서 batch 크기를 줄여 transport와 pattern 전반에 공통 회귀를 만들었다.
- 선택한 가설: 먼저 가설 1과 2를 함께 추적한다. SPOT 64B tcp/tls 하락폭이 가장 크지만 PUBSUB/DEALER도 함께 낮으므로, SPOT fanout만 보지 않고 공통 message/pipe 경로와 SPOT fanout 경계의 중복 비용을 확인한다.

## 읽은 코드

- `core/src/runtime/core/msg.*`: 64B payload는 `msg_t::max_vsm_size`보다 커서 LMSG/refcount 경로를 탄다.
- `core/src/runtime/core/pipe.*`: one-way send는 pipe write/flush와 HWM check를 지나며, 기존 단일 메시지 fast path는 STREAM 쪽에만 쓰인다.
- `core/src/runtime/sockets/internal/dist.*`: PUB/XPUB에는 단일 subscriber fast path와 VSM 분기가 이미 있다. 64B는 VSM이 아니므로 refcount/copy 비용이 남는다.
- `core/src/runtime/sockets/pubsub/xpub.cpp`: publish send는 subscription match 뒤 `dist_t`로 넘긴다. delivery-ready 계산은 attach/read/write 활성화 시점에 있고 매 publish마다 계산하지 않는다.
- `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`: `enqueue_publish_ingress`가 caller frames를 owned copy로 만든 뒤 `drain_publish_ingress_queue`가 `stage_message`를 통해 staged queue에 다시 copy하고 즉시 원본을 close한다.
- `core/src/runtime/services/spot/common/spot_message_parts_internal.hpp`: `spot_publish_msg_parts`는 fanout socket send 전에 send용 frames를 다시 copy한다. 이 경로는 pending/retry가 원본 ownership을 유지해야 하므로 별도 검증 없이 제거하지 않는다.
- `core/src/runtime/sockets/internal/lb.cpp`: DEALER one-way send는 `lb_t`를 통해 pipe에 쓴다. 단일 active pipe fast path는 있었지만 final single message 전용 pipe fast path는 쓰지 않았다.
- `core/src/runtime/sockets/internal/dist.cpp`: PUB/SUB fanout은 `dist_t::write_at`에서 pipe에 쓴다. VSM 분기는 있지만 64B는 LMSG라 final single message 전용 pipe fast path가 적용되지 않았다.

## 변경

- 변경 파일: 없음
- 변경 이유: `core/src/runtime/sockets/internal/lb.cpp`, `core/src/runtime/sockets/internal/dist.cpp`에서 routing-id가 아니고 multipart가 아닌 final single message에 `pipe_t::write_single_message_and_flush_no_recursive_hwm_check` fast path를 적용하는 후보를 검증했으나 targeted perf가 current report보다 낮아 되돌렸다.
- 되돌린 후보: `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`에서 publish ingress queue -> staged queue 이동 시 payload copy를 `std::move`로 바꾸는 후보를 검증했으나 targeted perf 효과가 5% 미만이라 되돌렸다.
- 되돌린 후보: `core/src/runtime/sockets/internal/lb.cpp`, `core/src/runtime/sockets/internal/dist.cpp`에서 final single message pipe fast path를 DEALER/PUBSUB distributor에 적용했으나 targeted perf가 개선되지 않아 되돌렸다.
- perf 전용 변경이 아닌 이유: 실제 core socket distributor hot path 변경이며 perf runner/client/server와 측정 조건을 바꾸지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 수정하지 않았다. 변경은 SPOT data-plane 내부 queue 사이의 `zlink_msg_t` 소유권 이동이다.
- 추가로 실행한 회귀 테스트: 없음

## 검증

- build: `cmake --build core/build -j$(nproc)` 통과
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'unittest_spot_data_plane_|test_spot_service_introspection_queue_|test_backpressure_(oneway_)?matrix_spot_regression|test_spot_pubsub_scenario_peer_tcp'` 통과, 10/10
  - `ctest --test-dir core/build --output-on-failure -R 'test_pubsub$|test_pubsub_filter_xpub|test_backpressure_(oneway_)?matrix_(pubsub|spot)_regression|test_multi_socket_contract_regressions|test_spot_pubsub_scenario_peer_tcp|unittest_spot_data_plane_|unittest_socket_runtime'` 통과, 12/12
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_SPOT --transports tcp,tls --msg-sizes 64 --duration 5` 통과. Runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`. 결과: tcp 3876.720 Kmsg/s, tls 3899.024 Kmsg/s.
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_DEALER_DEALER,MULTI_PUBSUB,MULTI_SPOT --transports tcp,tls --msg-sizes 64 --duration 5` 통과. Runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`. 결과: DEALER_DEALER tcp 2982.051/tls 3095.303 Kmsg/s, PUBSUB tcp 2516.592/tls 2381.888 Kmsg/s, SPOT tcp 3346.090/tls 3406.861 Kmsg/s.
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_SPOT --transports ws,wss --msg-sizes 64 --duration 5` 통과. Runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`. 결과: ws 3606.783 Kmsg/s, wss 3928.487 Kmsg/s.
  - `PERF_FAIL_FAST=1 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_SPOT --transports ws,wss --duration 5` 통과. Runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`. Completion: success 12, fail 0, status complete. 결과 파일: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_150826.txt`.
- full perf: 미실행

## 결과

- 개선 전: current report 기준 공통 64B 항목별 delta 중앙값 -14.9%, one-way 항목별 delta 평균 -27.4%
- 개선 후: 남긴 core 변경 없음
- delta:
  - SPOT ingress queue move 후보: current report 대비 tcp -0.5%, tls +4.3%. 5% 미만이므로 오차로 판단하고 변경을 되돌렸다.
  - distributor final single fast path 후보: current report 대비 DEALER_DEALER tcp -2.1%, tls -2.1%, PUBSUB tcp -4.2%, tls -2.6%, SPOT tcp -14.1%, tls -8.9%. 개선이 없어 변경을 되돌렸다.
- 목표 달성 여부: 미달성

## 다음 작업

- 남은 위험: current report는 partial이며 SPOT ws/wss 실패가 섞여 있다. 실패 수정과 성능 개선을 분리해서 판단해야 한다.
- 실패 재현 상태: `MULTI_SPOT ws/wss` 실패는 현재 runtime에서 재현되지 않았다. 같은 pattern/transport 전체 size targeted perf가 fail 0으로 끝났다. current partial report의 나머지 실패인 `MULTI_SPOT_REQREP ws/wss`, `MULTI_SPOT_SENDSEND ws/wss`, `MULTI_STREAM ws 1024B+`는 아직 재확인하지 않았다.
- 다음 goal 후보: `MULTI_SPOT_REQREP ws/wss`, `MULTI_SPOT_SENDSEND ws/wss`, `MULTI_STREAM ws 1024B+` 실패 재현 확인 후 실패 0개 기준선 승격
