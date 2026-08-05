# 라운드 7: SPOT publish owned frame copy 확인

- goal: SPOT/PUBSUB 64B one-way hot path에서 fanout publish 비용을 줄인다.
- 완료 기준: targeted 64B one-way set 중앙값 +10% 이상, 평균 +8% 이상, 관련 core test 통과, 작업 로그 작성.
- 시작 시각: 2026-06-14 16:25:56 +0900
- 기준 commit: `95513d46e`
- 시작 git status: clean
- 과거 기준 report: `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
- 문제 report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 실패 0 full report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_151925.txt`
- 대상 pattern/transport/size: `MULTI_SPOT,MULTI_PUBSUB` / `tcp,tls,ws,wss` / `64B`

## 가설

- 가설 1: SPOT data-plane forward 경로는 이미 owned `zlink_msg_t` frames를 보관하고 있는데, `spot_publish_msg_parts()`가 전송 직전에 다시 `zlink_msg_copy()`로 send frame 배열을 만든다. pending이 없고 목적지가 하나인 성공 경로에서 consume/move 전송을 쓰면 SPOT 64B one-way 처리량이 오른다.
- 가설 2: data-plane main pass가 이미 `drain_*` 전에 runtime sockets를 한 번 pump하는데, `forward_local_fanout()`와 `forward_mesh_pub()`가 성공 hot path에서 매 메시지마다 다시 command pump를 수행한다. 정상 성공 경로의 중복 pump를 제거하면 SPOT 64B one-way 처리량이 오른다.
- 먼저 검증할 가설: 가설 2. frame consume은 local/mesh 재사용과 EAGAIN pending fallback 때문에 안전 조건이 좁다. 반면 per-message pump는 main pass의 선행 pump와 EAGAIN/poller fallback이 있어 임시 검증 범위가 명확하다.

## 읽은 코드

- `core/src/runtime/services/spot/common/spot_message_parts_internal.hpp`: `spot_publish_msg_parts()`는 owned frames를 copy해서 `logical_multipart_publish()`에 넘긴다. `spot_publish_msg_parts_consume()`는 frames를 move하지만 현재 호출자가 없다.
- `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`: `forward_local_fanout()`과 `forward_mesh_pub()`는 EAGAIN 시 pending queue에 원본 parts를 남길 수 있어 무조건 consume할 수 없다.
- `core/src/runtime/services/spot/data_plane/spot_data_plane_loop.cpp`: `service_runtime_sockets()`는 `drain_data_plane_queued_ingress()` 전에 `pump_data_plane_socket_commands()`로 data-plane sockets를 한 번 pump한다.
- `core/src/runtime/core/multipart_send_txn.cpp`: `logical_multipart_publish()`는 topic frame과 payload frames를 같은 send scope에서 보낸다.
- `core/src/runtime/sockets/pubsub/xpub.cpp`, `core/src/runtime/sockets/internal/dist.cpp`: PUB/XPUB send는 matching/fanout 후 `dist_t`를 통해 pipe로 보낸다. 단일 matching pipe fast path는 이미 존재한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목: 없음
- 보안 의미를 유지한 근거: WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard, maxmsgsize 정책을 건드리지 않는다. pending queue가 필요할 수 있는 경로에서 원본 frames를 소모하지 않는다.
- 추가로 실행한 회귀 테스트: SPOT data-plane, SPOT scenario, SPOT backpressure, transport matrix, pubsub focused set

## 변경

- 임시 변경 파일: `core/src/runtime/services/spot/data_plane/spot_data_plane_forwarding.cpp`
- 변경 내용: `forward_local_fanout()`와 `forward_mesh_pub()`의 정상 전송 직전 `pump_socket_commands()` 호출을 제거했다.
- perf 전용 변경이 아닌 이유: SPOT data-plane runtime의 실제 publish forward hot path에서 중복 command pump를 줄이는 변경이다.
- 최종 변경 상태: core 파일은 HEAD와 차이 없음. targeted perf에서 의미 있는 개선이 없어 임시 변경을 되돌렸다.

## 검증

- build:
  - 임시 변경 후 `cmake --build core/build -j$(nproc)` 통과
  - 임시 변경을 되돌린 뒤 `cmake --build core/build -j$(nproc)` 통과
- test:
  - `ctest --test-dir core/build --output-on-failure -R 'unittest_spot_data_plane_|test_spot_service_introspection_queue_|test_spot_pubsub_scenario|test_spot_poller|test_spot_runtime_activation|test_backpressure_(oneway_)?matrix_spot_regression|test_transport_matrix|test_pubsub$|test_pubsub_filter_xpub'`
  - 결과: 22/22 통과
- targeted perf:
  - `PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern MULTI_SPOT --transports tcp,tls,ws,wss --msg-sizes 64 --duration 5`
  - runtime: `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
  - result: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_162855.txt`
  - completion: success 4, fail 0, status complete
  - load_avg: 1.74 1.49 1.76
- 비교:
  - 문제 report `perf_c_multi_linux_20260614_103936.txt` 대비 공통 2개 평균 -8.20%, 중앙값 -8.20%
  - 실패 0 full report `perf_c_multi_linux_20260614_151925.txt` 대비 4개 평균 +1.03%, 중앙값 -1.24%
  - `MULTI_SPOT wss`는 실패 0 full 기준 +22.18%였지만 `tls`가 -15.58%라 transport 전반 반복 개선이 아니다.

## 결과

- 목표 달성 여부: 미달성
- 판정: per-message command pump 제거는 SPOT 64B one-way 회귀의 주된 병목이 아니다.
- 조치: 임시 core 변경은 되돌렸고, 되돌린 뒤 `core/build`를 다시 빌드했다.
- 다음 후보: SPOT owned frame copy consume은 안전 조건이 좁아 바로 변경하지 않는다. 다음 라운드는 PUBSUB 일반 XPUB matching/send-all 또는 message allocation/refcount 쪽을 우선 본다.
