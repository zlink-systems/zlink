# posddd-rf1 summary

## 결과

- 작업 브랜치: `perf/phase2-judge`
- 수정 범위: `core/src/api/socket/**`의 25개 파일만 수정했다.
- 공개 C API, ABI, enum, option, 제어점과 외부 계약은 바꾸지 않았다.
- 파일 이동은 하지 않았다. 책임상 runtime으로 이동해야 하는 항목은 범위 밖 `CMakeLists.txt`와 runtime 수정을 요구하므로 BLOCKERS로 남겼다.
- 줄 수: 추가 607줄, 삭제 1,478줄, 순감 871줄.
- `doc/**`, `core/doc/**`, `framework/**`, `bindings/**`, `hotpath_reference.json`은 수정하지 않았다.

## 1. 불필요 코드 제거

수정 전 `rg -n '<symbol>' . --glob '!core/build/**'`로 저장소 전체를 확인했다. 아래의 독립 심볼은 선언·정의 외 호출이 없었고, 종속 심볼은 호출자 0인 root 또는 항상 비활성인 분기에서만 참조됐다.

| 삭제 대상 | 호출자 0 근거 |
| --- | --- |
| `send_request_reply_message` | 전 트리에서 선언과 정의만 존재했다. 이 함수만 호출하던 `take_router_reply_target`, `commit_router_reply_target` wrapper도 함께 제거했다. |
| DEALER typed-receive 분기와 `dealer_next_reply_token`, `allocate_dealer_reply_token`, `take_dealer_reply_target`, `restore_dealer_reply_target`, `commit_dealer_reply_target` | `recv_dealer_message_direct`의 production 호출자는 `socket_message_recv_api.cpp`, `socket_message_api.cpp` 두 곳뿐이고 둘 다 기존부터 `typed_receive=false`였다. typed 전용 token/target 경로에는 도달 가능한 호출자가 없었다. |
| `fixed_routing_id_key_t::clear`, `fixed_routing_id_key_t::operator<`, `request_correlation_lease_t::accounted_bytes` | 전 트리에서 선언과 정의만 존재했다. |
| `send_request_payload_part` | 전 트리에서 선언과 정의만 존재했다. |
| `inline_msg_buffer_t`의 const `data`, `back`, `operator[]` overload | 모든 template instantiation과 호출부를 확인했으며 non-const overload만 사용됐다. |
| `reply_target_store_t`의 `const_iterator`, const `find/begin/end`, `count`, `erase(key)` | 저장소 전체의 store 사용부는 mutable iterator와 `erase(iterator)`만 사용했다. |

도달 불가 또는 중복 분기도 함께 정리했다.

- raw 전용 DEALER receive에서 typed admission·reply target 게시·message type/request sequence 출력과 관련 rollback을 삭제했다. raw payload의 metadata 제거와 후속 frame 검증은 유지했다.
- ROUTER receive 성공 뒤 helper mutex 밖에서 수행하던 `buffered_parts.empty()` 검사는 staging 성공 조건과 중복되어 삭제했다.
- direct public receive의 한 번만 호출되는 type helper와 이미 반환된 ROUTER 뒤의 도달 불가 분기를 제거했다.
- timeout scheduler의 write-only `completed`, 중복 보관 `deadline_ns`, 등록 불변상 불필요한 iterator end 검사를 제거했다.
- 사용하지 않는 include와 전이 include를 각 translation unit의 실제 의존성에 맞게 삭제했다.

## 2. 중복·pass-through 제거

- `close_msg_frames`(API frame-buffer overload), `close_request_reply_parts`를 없애고 소유 함수인 `close_request_reply_frame_buffer`와 `zlink_multipart_close`를 직접 호출했다. `core/src/runtime/core/recv_internal.*`의 동명 vector helper는 별도 책임이므로 유지했다.
- `create_send_scope`, `find_handle_state`, `is_stream_type`, `s_sendmsg`, `validate_socket_type(void *)`, `send_sequence_active` 같은 한 줄 위임을 제거하고 표준 socket handle/lifecycle 경로를 직접 사용했다.
- `consume_checked_core_msg`, `release_socket_pending_request_correlation`, `queue_socket_pending_timeout_completion`, `pending_identity_matches`를 없애고 기존 frame consume, correlation lease, completion publish, identity 비교 소유 API를 직접 사용했다.
- MORE와 FINAL의 동일 append 로직을 `append_public_send_part_locked` 한 곳으로 합치고, 별도 `append_public_send_final_locked`를 제거했다.
- part-helper의 정상 abort, 현재-sequence abort, socket cleanup에 있던 scope resume/close-cleanup/rollback 사본을 `try_rollback_send_scope_locked`로 통합했다.
- endpoint와 RID별 pending `NOT_FOUND` 종료 루프를 `fail_matching_pending_requests` 하나로 합쳤다.
- `request_part_common`에서 공개 facade가 이미 보장한 flag, timeout/user context, socket, completion-id 초기화 검사를 반복하지 않도록 했다. message object의 내부 유효성 검사는 유지했다.
- bind/connect의 성공·실패 동일 반환 분기, submit wait의 no-op errno 저장/복원, 동일 조건의 abort 분기를 축약했다.

## 3. 책임 분리와 hot-path 정리

- `prepare_send_step_state_locked`에서 신규 sequence의 선택·admission을 `begin_send_sequence_locked`, commit된 sequence 재개를 `resume_send_sequence_locked`로 분리했다. marker 게시, scope 획득, spec commit, DONTWAIT 후퇴 순서는 유지했다.
- topic 복사 OOM에서는 acquired scope를 먼저 해제한 뒤 send-active marker를 내리도록 해 cache와 실제 lifecycle의 게시 순서를 맞췄다.
- receive-ready cache 게시를 `publish_buffered_recv_readiness` 한 곳으로 모았다. reset은 `active=false`를 먼저 commit한 뒤 cache를 게시한다.
- receive transport pair id/generation을 `stage_recv_sequence`의 helper mutex transaction 안에서 parts와 함께 게시하도록 바꿨다. 별도 unsynchronized setter는 삭제했다.
- ROUTER receive의 test failpoint, frame staging, pair metadata, 실패 시 close를 `stage_router_recv_sequence`로 모았다. target revoke와 receive abort 책임은 호출자에 유지했다.
- close 시 request/reply 상태 폐기를 `discard_request_reply_state_for_close`로 분리해 checkout seal → closing → timeout cancel → active reply abandon → pending drain → target clear 순서를 한 곳에서 소유하게 했다.
- completion queue `has_ready()`의 mutex 조회를 `ready_available` acquire-load로 바꿨다. true는 empty→non-empty, false는 마지막 dequeue와 close 전이에만 mutex 아래 release-store한다.
- completion reservation의 outstanding list, ready queue, recycle cache 책임을 명명으로 분리하고 cache-pop의 중복 reset을 제거했다. completion payload release는 queue 내부 함수 한 곳에서 소유한다.
- timeout scheduler는 deadline 정렬 container가 deadline과 iterator를 소유하게 하고, task는 cancel/firing fence만 소유하도록 축소했다.

## 4. 명명 변경

| 이전 | 변경 | 이유 |
| --- | --- | --- |
| `rid1`, `has_rid1` | `routing_id`, `has_routing_id` | 순번이 아닌 routing 의미를 드러냄 |
| ROUTER receive의 `request_seq` output/local | `reply_token` | wire sequence가 아니라 application capability임을 명시 |
| `reply_target_reservation_t` | `router_reply_target_reservation_t` | DEALER typed 경로 제거 뒤 실제 소유 socket을 명시 |
| `reply_target_receive_admission_t` | `router_reply_target_receive_admission_t` | ROUTER receive admission 책임 명시 |
| `reply_target_publish_guard_t` | `router_reply_target_publish_guard_t` | ROUTER target rollback guard임을 명시 |
| `router/dealer_reply_target_map_t` | `router/dealer_reply_target_store_t` | custom intrusive storage가 표준 map이 아님을 명시 |
| `router_reply_alias_key`, `router_reply_alias_bucket` | `make_router_reply_alias_key`, `router_reply_alias_bucket_for` | 생성과 조회 동작을 동사로 표현 |
| queue `all_*`, `live_*` | `outstanding_*` | reservation lifecycle count와 같은 용어로 통일 |
| `publish_locked` | `enqueue_ready` | lock 여부가 아닌 queue 전이를 표현 |
| `cache_released_locked` | `recycle_reservation_locked` | 반환 node가 재사용 또는 삭제 후보라는 의미를 표현 |
| scheduler `schedule_map/schedule/schedule_it` | `tasks_by_deadline/tasks_by_deadline/deadline_it` | 정렬 기준과 iterator 역할 명시 |
| `empty_routing_id`, `empty_completion`, `initialized_empty_message` | `is_zero_initialized_routing_id`, `is_initialized_empty_completion`, `is_initialized_empty_message` | 단순 empty와 초기화된 public object 상태를 구분 |
| `stage_public_send_part_locked` | `append_public_send_part_locked` | lifecycle staging이 아니라 buffer append 책임임을 명시 |

구현을 그대로 반복하던 주석은 제거하고 admission gap, callback 재진입, close discard, transport correlation처럼 순서나 정책의 이유를 설명하는 주석만 유지했다.

## 감독관 commit 분리용 파일 묶음

파일 안에서 항목이 겹치므로 일부 파일은 hunk 단위 분리가 필요하다.

### 불필요 코드·pass-through 제거

- `inline_msg_buffer_internal.hpp`
- `part_helper_api.cpp`, `part_helper_internal.hpp`, `part_helper_state.cpp`
- `request_reply_frame_buffer_internal.hpp`, `request_reply_protocol_internal.hpp`
- `socket_api.cpp`, `socket_api_internal.hpp`
- `socket_message_api.cpp`, `socket_message_recv_api.cpp`, `socket_message_send_api.cpp`
- `socket_request_reply_api.cpp`, `socket_request_reply_internal.cpp`, `socket_request_reply_internal.hpp`
- `socket_request_reply_pending_api.cpp`, `socket_request_reply_router_control.cpp`, `socket_request_reply_runtime_io.cpp`
- `socket_request_reply_submit_api.cpp`, `socket_request_reply_submit_internal.hpp`

### 책임 분리·hot-path 정리

- `part_helper_api.cpp`, `part_helper_internal.hpp`, `part_helper_state.cpp`
- `socket_message_api.cpp`, `socket_message_send_api.cpp`
- `socket_request_reply_dispatch.cpp`, `socket_request_reply_router_api.cpp`, `socket_request_reply_runtime_io.cpp`
- `socket_completion_queue_internal.cpp`, `socket_completion_queue_internal.hpp`, `socket_message_handler_api.cpp`
- `request_timeout_scheduler_internal.cpp`

### 명명·주석 정합

- `part_helper_api.cpp`, `part_helper_internal.hpp`
- `socket_completion_queue_internal.cpp`, `socket_completion_queue_internal.hpp`, `socket_message_handler_api.cpp`
- `request_timeout_scheduler_internal.cpp`
- `socket_request_reply_dispatch.cpp`, `socket_request_reply_internal.cpp`, `socket_request_reply_internal.hpp`
- `socket_request_reply_pending_api.cpp`, `socket_request_reply_router_api.cpp`, `socket_request_reply_runtime_io.cpp`, `socket_request_reply_submit_api.cpp`

## Gate 결과

- `ulimit -v 16777216 && cmake --build core/build -j4`: 성공, 최종 증분 86/86.
- 관련 테스트 15개: 15/15 통과.
- `ulimit -v 16777216 && ctest --test-dir core/build -j2 --output-on-failure`: 134/134 통과, 164.64초.
- 알려진 flake `test_single_lane_flow_snapshot_accounting`: 첫 실행 통과, 단독 재실행 불필요.
- `git diff --check`: 성공.
- raw header mirror `c/cpp/go/rust × zlink_enum.h, zlink/socket/api.h, zlink/eventing/api.h`: 12/12 동일.
- 지시대로 perf와 binding 테스트는 실행하지 않았다.

## BLOCKERS

- `core/src/api/socket/socket_completion_queue_internal.cpp:recycle_reservation_locked/release/recv` — callback 재진입 사이 cache가 찰 때 embedded inline reservation이 delete 후보로 반환될 수 있는 기존 race가 남아 있다. `heap_owned` node만 delete 후보가 되게 보장하고 동시 reserve/release race test를 추가해야 하며, correctness 동작 변경과 범위 밖 test 수정이 필요해 보류했다.
- `core/src/api/socket/socket_request_reply_dispatch.cpp:move_completion_payload` — 정상 reply completion마다 독립 `reply_parts` 배열을 할당한다. 할당 제거에는 public completion close 소유권을 보존하는 pooled/inline owner 설계와 계약 검증이 필요하다.
- `core/src/api/socket/socket_request_reply_runtime_io.cpp:collect_multipart_payload_parts` — 8개를 넘는 receive part는 heap spill한다. 제거하려면 socket/runtime 소유 reusable receive scratch와 동시 receive 규칙이 필요하다.
- `core/src/runtime/sockets/common/socket_runtime.hpp:socket_runtime_t::completion_runtime` — runtime이 API 계층의 completion queue를 포함한다. queue를 `runtime/sockets/common`으로 옮기고 `core/CMakeLists.txt`와 include 방향을 함께 수정해야 한다.
- `core/src/api/socket/request_timeout_scheduler_internal.cpp:request_timeout::*` — worker와 timer map은 runtime 책임이다. 파일 이동에는 범위 밖 runtime 경로와 `core/CMakeLists.txt` 수정이 필요하다.
- `core/src/api/socket/socket_api.cpp:zlink_has` — capability facade는 source-layout상 `api/core` 책임이다. 이동에는 `core/CMakeLists.txt` 수정이 필요하다.
- `core/src/api/socket/socket_request_reply_runtime_io.cpp:recv_router_message_direct/recv_dealer_message_direct/send_completion_staged_frames_on_pipe` — routing, correlation, transport wait semantics를 socket API 파일이 소유한다. `runtime/sockets`로 옮기려면 범위 밖 runtime header와 build 목록 변경이 필요하다.
- `core/tests/unittest/unittest_zmp_contract_edges.cpp:test_pending_aggregate_wrap_and_stale_cookie_are_fenced` — production 호출자 0인 `schedule_socket_pending_timeout`, legacy timeout callback context와 `record_socket_pending_transport_pair_identity`를 직접 호출한다. aggregate generation 경로를 통한 test로 재작성한 뒤 dead helper를 삭제해야 한다.
- `core/tests/unittest/unittest_zmp_contract_edges.cpp:test_error_reply_*` — production 호출자 0인 `decode_reply_completion`을 직접 호출한다. public/transport completion 경로 기반 test로 바꾼 뒤 helper를 삭제해야 한다.
- `core/tests/unittest/unittest_zmp_contract_edges.cpp:test_missing_completion_pipe_is_not_connected` — production 호출자 0인 `send_completion_staged_frames`와 `retain_reply_completion_pipe` wrapper chain을 직접 호출한다. owner 경로 test로 바꾼 뒤 wrapper를 삭제해야 한다.
- `core/src/runtime/sockets/common/socket_base_api.cpp:forget_dealer_reply_targets_for_pipe` — runtime 호출과 범위 밖 unit test가 obsolete DEALER reply-target store를 유지시킨다. raw DEALER 계약 기준으로 runtime 호출과 test를 제거한 뒤 store/target type 전체를 삭제해야 한다.
- `core/tests/integration/test_phase3_request_reply_contract.cpp:test_router_reply_final_oom_does_not_consume_token` — fixed inline RID에는 더 이상 allocation이 없는 `request_reply_allocation_reply_key` failpoint를 직접 요구한다. test와 enum 보존 조건을 재검토한 뒤 obsolete hook을 제거해야 한다.
- `core/tests/unittest/unittest_zmp_contract_edges.cpp:resolve_timeout_ms` — 세 번째 socket timeout 인자는 production에서 사용되지 않지만 test가 직접 계약화한다. test 기대를 public timeout 우선순위로 옮긴 뒤 인자를 제거해야 한다.
- `core/tests/integration/test_zmp_request_reply_receive_transaction.cpp:max_reply_target_slots` — 상수는 pending capacity에도 쓰이지만 private 이름을 test가 직접 참조한다. 의미에 맞는 공통 capacity 이름으로 바꾸려면 범위 밖 test 수정이 필요하다.
- `core/src/api/core/zlink.cpp:create_socket_handle` — socket API internal의 `is_send_only_socket_type` 호출자가 API core에 남아 있다. socket-type 분류 책임을 core facade 쪽으로 옮기려면 범위 밖 파일 수정이 필요하다.
- `core/src/api/socket/request_timeout_scheduler_internal.cpp:monotonic_now_ns` — monitoring scheduler와 동일한 clock 변환이 중복된다. 공용 runtime clock utility로 합치려면 범위 밖 monitoring/runtime 파일 수정이 필요하다.
