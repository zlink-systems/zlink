# Round 8: multipart publish message frame 검토

## 범위

- 대상: `core/src/runtime/core/multipart_send_txn.cpp`,
  `core/src/runtime/core/msg.*`,
  `core/src/runtime/sockets/pubsub/xpub.*`
- 목표: PUBSUB/SPOT 64B hot path에서 반복되는 topic frame 생성, message copy,
  subscription match 비용 중 core 내부에서 안전하게 줄일 수 있는 후보를 찾는다.
- 제외: perf runner, perf client/server, benchmark policy 변경.

## 기준

- 작업 시작 시 `git status --short` 는 깨끗했다.
- 기준 HEAD: `f3659d25c`
- 직전 zero-fail 전체 기준: `perf_c_multi_linux_20260614_151925.txt`
- 직전 targeted SPOT 기준: `perf_c_multi_linux_20260614_162855.txt`

## 가설

1. `logical_multipart_publish()` 는 publish마다 topic 문자열 길이를 구하고
   작은 topic frame을 만든 뒤 payload frame을 이어 보낸다. topic이 `bench`처럼
   짧아도 모든 PUBSUB/SPOT publish에서 반복되므로 비용 후보가 될 수 있다.
2. 64B payload는 `msg_t::max_vsm_size` 를 넘어서 long message가 된다. 따라서
   topic frame 자체보다 payload message allocation/refcount, XPUB subscription
   matching, distributor fan-out 비용이 더 클 가능성이 높다.

## 확인 내용

- `msg_t::max_vsm_size` 는 `64 - (3 + 16 + sizeof(uint32_t))` 이다. 64B
  payload는 long message이고, `bench` topic은 VSM frame이다.
- multi PUBSUB와 multi SPOT benchmark 모두 topic 문자열로 `bench` 를 사용한다.
- public PUBSUB send는 `perf_zlink_publish_parts()` 에서
  `logical_multipart_publish()` 로 들어간다.
- SPOT publish도 `spot_publish_msg_parts()` 에서
  `logical_multipart_publish()` 로 들어간다.
- `logical_multipart_publish_frame()` 은 이미 topic frame을 받는 API가 있지만,
  호출자가 같은 topic frame을 재사용할 수 없으면 topic copy 자체는 남는다.

## 다음 판단

- topic frame만 줄이는 변경은 10% 이상 개선 가능성이 낮다.
- 우선 XPUB subscription match 쪽에 일반 동작을 깨지 않는 fast path가 있는지
  확인한다. 안전한 작은 변경이 없으면 이 후보는 제외하고 되돌린다.

## 실험 A: distributor match index lookup 축소

- 변경: `dist_t::match()` 에서 `pipe_t` 의 array index를 한 번만 읽고 같은
  값을 eligibility 검사와 swap에 사용한다.
- 근거: XPUB가 empty prefix subscriber를 매 publish마다 root set에서 순회할 때
  각 pipe마다 `dist_t::match()` 를 호출한다. 기존 코드는 같은 pipe index를
  세 번 읽는다.
- 계약 영향: 없음. matching/eligible 판정과 swap 대상은 기존과 같다.

### 검증

- Build: `cmake --build core/build -j$(nproc)` 통과.
- Focused test:
  `ctest --test-dir core/build --output-on-failure -R 'unittest_spot_data_plane_|test_spot_service_introspection_queue_|test_spot_pubsub_scenario|test_spot_poller|test_spot_runtime_activation|test_backpressure_(oneway_)?matrix_spot_regression|test_transport_matrix|test_pubsub$|test_pubsub_filter_xpub|test_xpub_nodrop'`
  - 23/23 통과.
- Targeted PUBSUB:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_PUBSUB --transports tcp,tls,ws,wss --msg-sizes 64 --duration 5`
  - Report: `perf_c_multi_linux_20260614_163551.txt`
  - success 4, fail 0
  - 기준 `perf_c_multi_linux_20260614_151925.txt` 대비 throughput:
    tcp +4.50%, tls +0.46%, ws -1.08%, wss -0.03%
  - 평균 +0.97% 수준으로 10% 개선 기준에 못 미친다.
- Targeted SPOT:
  `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_SPOT --transports tcp,tls,ws,wss --msg-sizes 64 --duration 5`
  - Report: `perf_c_multi_linux_20260614_163637.txt`
  - success 4, fail 0
  - 직전 targeted 기준 `perf_c_multi_linux_20260614_162855.txt` 대비 throughput:
    tcp -3.51%, tls +1.17%, ws -13.23%, wss -14.84%

### 판단

- 실험 A는 PUBSUB tcp에서만 5% 미만 개선을 보였고, SPOT에서는 주요 전송에서
  손실이 컸다.
- 10% 이상의 재현 가능한 개선으로 볼 수 없어 source 변경은 제거했다.
- topic frame 생성과 단순 match index lookup은 현재 기준에서 핵심 병목으로
  보기 어렵다.
