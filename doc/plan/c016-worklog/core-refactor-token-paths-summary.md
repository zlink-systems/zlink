# Core token-path 리팩토링 요약

기준: detached `5e26e7280698`. 공개 계약과 동작은 유지하고, SEND/REQUEST가 공유하는 payload-free WRITABLE 대기 토큰 이후에 남은 비동기 pending 명칭과 기계를 정리했다.

| 종류 | 파일:행 | 근거 | 검증 |
|---|---|---|---|
| dead code 제거 | `core/src/runtime/core/command.hpp:35`, `core/src/runtime/core/object.cpp:84`, `core/src/runtime/sockets/common/socket_send_complete.cpp:477` | payload 없이 발행되고 수신 시 아무 작업도 하지 않던 `command_t::send_pending`과 두 terminal 발행부를 제거했다. blocking waiter의 상태 변경 뒤 기존 `notify_submit_progress()`가 CV와 public mailbox owner를 깨우므로 별도 dummy command가 필요 없다. | 변경 suite 5회, 전체 ctest 141/141 PASS |
| dead code 제거 | `core/src/runtime/sockets/common/socket_base.hpp:920`, `core/src/runtime/sockets/router/router.hpp:161`, `core/src/runtime/sockets/router/router_admission.cpp:142` | 호출자가 0개인 `send_pending_target_mutex()` / `xsend_pending_target_current_locked()` 가상 훅과 ROUTER 구현을 제거했다. 제거된 pending publication fence의 잔재였다. | `rg` 호출 0건 확인, dev build PASS |
| dead code 제거 | `core/tests/unittest/unittest_socket_runtime.cpp:545`, `core/tests/integration/test_flow_state_c_api.cpp:708` | 폐기된 runtime map이 비어 있다는 구조 종속 테스트와 pending pool을 전제로 한 0.15.1-era 설명을 제거했다. 통합 테스트의 public API 동작 assertion은 유지했다. | `unittest_socket_runtime` 및 `test_flow_state_c_api` 각 5회 PASS |
| 책임 분리 | `core/src/runtime/sockets/common/socket_send_complete.cpp:161`, `core/src/runtime/sockets/common/socket_base.hpp:534` | retryable errno 4종(`EAGAIN`, `ENOTCONN`, `EHOSTUNREACH`, `ECONNREFUSED`) 판정, WRITABLE wait 등록, 성공 시 `errno=EAGAIN` 계약을 socket-owned helper 한 곳이 소유한다. | SEND/REQUEST 변경 suite 5회, 전체 ctest PASS |
| 책임 분리 | `core/src/api/socket/socket_message_send_api.cpp:387`, `core/src/api/socket/socket_message_send_api.cpp:473`, `core/src/api/socket/socket_request_reply_submit_api.cpp:243` | SEND 두 곳의 중복 판정/등록을 helper로 위임했다. REQUEST는 기존과 동일하게 정규화 결과가 `EAGAIN`인 경우에만 helper를 호출해 오류 계약을 넓히지 않았다. | `test_phase3_completion_contract`, `test_request_writable_contract` 각 5회 PASS |
| 이름 | `core/src/runtime/sockets/common/socket_runtime.hpp:660`, `core/src/runtime/sockets/common/socket_send_submit.cpp:36`, `core/src/runtime/sockets/common/socket_send_complete.cpp:477` | 실제 소유 책임에 맞춰 `send_pending` 계열 runtime/guard/fail 이름을 `blocking_send_wait`로 바꿨다. pending pool이나 재시도 FIFO로 오인시키는 주석도 현재 blocking retry 의미로 정리했다. | 잔재 패턴 `rg` 0건, dev build PASS |
| 이름 | `core/src/runtime/sockets/common/socket_send_submit.cpp:1`, `core/CMakeLists.txt:890` | SEND와 REQUEST admission을 함께 소유하는 파일을 `socket_send_pending_submit.cpp`에서 `socket_send_submit.cpp`로 이름 변경하고 빌드 목록을 갱신했다. | dev 및 Release+LTO build PASS |
| 성능 | `core/src/api/socket/socket_message_send_api.cpp:459` | multipart DONTWAIT 즉시 성공에서 사용되지 않는 TLS `errno` 읽기를 실패 분기 안으로 옮겼다. 공유 fallback helper 호출과 retryable 판정은 모두 기존 admission 실패 분기 뒤에 있어 성공 경로의 호출/분기를 늘리지 않는다. | hotpath 4셀 PASS; 전후 변화 -0.0145%~+0.0380% |
| 성능 | `core/src/runtime/sockets/common/socket_send_complete.cpp:504`, `core/src/runtime/sockets/common/socket_send_complete.cpp:527` | terminal blocking waiter wake에서 payload 없는 command 구성·mailbox send를 제거하고 기존 progress notification을 사용했다. wait-token publication의 atomic fence는 실패 전용 correctness boundary이므로 유지했다. | wake 관련 변경 suite 5회, 전체 ctest PASS |

## Hot-path 전후

Release+LTO `core/build-gate`의 Valgrind instruction/call 값이다.

| cell | 전 | 후 | 후/전 변화 | reference 판정 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3437.307050 | 3438.160250 | +0.0248% | PASS |
| dealer_router_reqrep_inproc | 12015.504600 | 12013.766600 | -0.0145% | PASS |
| pair_inproc | 2681.633700 | 2682.652000 | +0.0380% | PASS |
| router_router_tcp | 2967.695500 | 2968.336000 | +0.0216% | PASS |

모든 변화가 ±0.04% 이내이며 ±5% 기준을 충족했다. REQUEST 조합은 소폭 개선됐고 나머지는 계측 노이즈 범위에서 유지됐다.

## 검증

- `JOBS=6 bash scripts/build-core.sh dev`: PASS
- 변경 suite 5회 반복: 5종 모두 PASS (`test_phase3_completion_contract`, `test_request_writable_contract`, `test_flow_state_c_api`, `test_stream_send_blocking_wakeup`, `unittest_socket_runtime`)
- `ctest --test-dir core/build-dev -j2 --output-on-failure -E '^hotpath_gate$'`: 141/141 PASS
- Release+LTO `hotpath_bench` 빌드 및 지정 `hotpath_gate.py`: 4/4 PASS
- 공개 헤더 mirror `cmp`: 8 headers × 4 bindings = 32/32 동일
- tracked diff와 새 `socket_send_submit.cpp` whitespace check: PASS
- 지정 pending/reservation/redrive 잔재 검색: 0건. `ZLINK_OPT_PENDING_MAX_*`의 옵션 저장·ABI 테스트, 현재 completion queue의 WRITABLE reservation, reply-token waiter redrive는 현행 계약이므로 유지했다.
- 공개 헤더, `core/doc`, bindings, 일반 doc 변경 없음. 파일 rename에 필요한 `core/CMakeLists.txt` 한 줄만 빌드 목록 예외로 변경했다.
- `manage_public_send_recovery_`는 completion-aware DONTWAIT가 legacy POLLOUT recovery를 arm/clear하지 않고 caller가 payload를 한 번만 소비하도록 single-record dispatch에서 false를 leaf까지 전달해야 하므로 제거하지 않았다.

## 통합 테스트 내부 심볼 사용(이번 범위에서는 미수정)

아래 19개는 public API만 사용하는 테스트가 아니며 내부 API/runtime/protocol/transport header를 include한다.

1. `core/tests/integration/monitoring/test_monitor_enhanced.cpp`
2. `core/tests/integration/monitoring/test_monitor_perf_contract.cpp`
3. `core/tests/integration/monitoring/test_monitor_socket_contract.cpp`
4. `core/tests/integration/test_asio_ws.cpp`
5. `core/tests/integration/test_ctx_destroy.cpp`
6. `core/tests/integration/test_dealer_router_single_lane_contract.cpp`
7. `core/tests/integration/test_flow_state_c_api.cpp`
8. `core/tests/integration/test_flow_state_paired.cpp`
9. `core/tests/integration/test_helper_recv_part_basic.cpp`
10. `core/tests/integration/test_multi_socket_contract_regressions.cpp`
11. `core/tests/integration/test_phase3_request_reply_contract.cpp`
12. `core/tests/integration/test_proxy.cpp`
13. `core/tests/integration/test_pubsub_filter_xpub.cpp`
14. `core/tests/integration/test_router_concurrent_routed_recv.cpp`
15. `core/tests/integration/test_router_multiple_dealers.cpp`
16. `core/tests/integration/test_stream_socket.cpp`
17. `core/tests/integration/test_zmp_metadata.cpp`
18. `core/tests/integration/test_zmp_request_reply_receive_transaction.cpp`
19. `core/tests/integration/test_zmp_ws_wss.cpp`
