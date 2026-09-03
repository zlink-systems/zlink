# POSDDD refactor 2 summary

## 결과와 범위

- 수정은 지정된 `core/src/runtime/sockets/{common,dealer,router,internal}/**` 안에서만 수행했다.
- 공개 C API/header, ABI, enum, socket 계약은 변경하지 않았다.
- 28개 파일(기존 27개 + 신규 `router/router_debug.hpp`)에서 총 `+1304/-1689`, 순감 `-385`줄이다.
  - common: `+1021/-1169`, 순감 `-148`
  - dealer: `+50/-73`, 순감 `-23`
  - internal: `+74/-154`, 순감 `-80`
  - router: 신규 header 포함 `+159/-293`, 순감 `-134`

## 1. 불필요 코드 제거

호출 근거는 generated build tree가 아닌 tracked 전체 tree를 대상으로 `git grep -n -w <symbol> HEAD -- .`로 삭제 전 참조 수를 확인하고, 삭제 후 `rg`로 잔존 참조 0을 확인했다.

### 호출자 0 함수

- HEAD에 선언과 정의만 각 1개(총 2곳), 호출 0:
  - `application_pipe_for_completion`
  - `begin_public_api_scope`
  - `emit_socket_monitor_value_event`
  - `event_handshake_failed_auth`
  - `get_peer_weight`
  - `monitor_has_attached_pipes`
  - `prepare_auto_hwm_socket_plan`
  - `retain_completion_pipe_for_transport_pair`
  - `send_routed_transport_pair`
  - `set_auto_hwm_role`
  - `set_peer_weight`
  - `socket_bound_endpoints`
  - `socket_has_attached_pipes`
  - `socket_peer_remote_endpoints`
  - `term_transport_pair`
  - `test_fail_next_recv_pipe_pin`
  - `fq_t::arm_dispatch`
  - `fq_t::has_in_with_record_admission`
  - `routing_socket_base_t::try_erase_out_pipe`
  - `socket_close_ops_t::request_close_and_wait`
- HEAD에 선언만 1곳, 호출/정의 0:
  - `zlink_free_event`
  - `socket_base_t::receive_sync`
  - `socket_base_t::inprocs_t`
  - `monitor_delivery_ready_pump`
  - `claim_send_pending_head`
  - `routing_socket_base_t::any_of_out_pipes`
- 닫힌 dead chain:
  - `xterm_transport_pair`: 선언/정의와 호출자 0인 `term_transport_pair`의 호출만 총 3곳이었다.
  - `wait_until_closed`: 선언/정의와 호출자 0인 `request_close_and_wait`의 호출만 총 3곳이었다.
  - `socket_close_ops_t::request_close(socket_base_t *&)`: 위 dead chain의 유일한 실호출만 제거하고, 실제 API 호출자가 있는 timeout 인자 overload는 유지했다.
- 같은 TU에서 정의만 있고 호출이 없던 static `copy_routing_id`를 제거했다.
- 호출자 0인 receive pin test hook을 뿌리로 하는 `g_fail_next_recv_pipe_pin`/`consume_recv_pipe_pin_failpoint` 전체를 제거하고 실제 lifetime-ref 실패 처리는 유지했다.

### 읽히지 않는 필드와 구 경로

- `transport_pair_connect_intent_t::completion_generation`
- `transport_pair_connect_intent_t::completion_owner_connection_id`

두 필드는 HEAD에서 initializer/declaration/assignment 총 3곳씩이고 read가 0이었다.

- `socket_base_t::_auto_hwm_role`
- `socket_base_t::_auto_hwm_role_override`

유일한 override writer인 `set_auto_hwm_role`의 호출자가 0이므로 override 분기는 도달하지 않았다. dead cache와 setter를 제거하고 기존 실제 동작인 `auto_hwm_default_role_for_socket_type(options.type)`를 계획/queue policy에서 직접 사용한다.

### 미사용 선언/include

- common의 `c_api_copy_internal`, `likely`, `sleep`, `ctx` include와 dead forward declaration을 제거했다.
- dealer의 `session_base`, FQ의 `vector`/`blob`, DIST의 `vector` include를 제거했다.
- ROUTER 네 TU와 header에서 각자 복제된 debug 의존성과 실제로 사용하지 않는 `set`/`vector`/`session_base`/`stdint`/queue-registry 등을 제거했다.

## 2. 중복·pass-through 제거

- DEALER의 한 줄 `sendpipe` wrapper를 제거하고 `xsend`/`xsend_pipe`가 `lb_t`의 표준 경로를 직접 호출한다.
- DEALER의 `request_router_peer` 한 줄 predicate를 호출 위치에 합치고, lifecycle/connection/application-ready 공통 검사는 `active_submit_candidate`가 소유한다.
- configured endpoint와 preselected pipe 전송의 동일한 request-type, weight, ready 검사를 `send_selected_pipe` 한 곳으로 모았다. `pipe_out` 게시 시점은 기존처럼 검사 후·write 전에 유지했다.
- DEALER request-route weight history 갱신 두 사본을 `update_request_route_weight`로 통합했다.
- FQ/LB의 반복 active-partition swap/current 보정을 helper로 모았다. FQ read miss는 기존의 `publish -> partition rotate` 순서가 다른 경로와 달라 `deactivate_current_after_read_miss`로 분리해 그대로 보존했다.
- LB request-limit pipe 복원 네 사본을 `restore_request_limited_pipes`로 통합했고, DIST `match`의 동일 index 조회 세 번을 한 번으로 줄였다.
- ROUTER의 `_current_out`/connection-id pair 대입과 receive-record 종료 다섯 사본을 각각 `select_current_out_pipe`/`clear_current_out_pipe`, `finish_current_in_record`로 통합했다.
- ROUTER 네 TU의 debug flag와 routing-id formatter 사본을 신규 `router_debug.hpp`로 이동했다. flag storage는 `router.cpp` 한 곳만 소유한다.
- send complete의 part copy/close 사본을 `copy_send_part_array`/`close_send_parts`로 모으고, 단 한 번 호출되던 frame-attempt lambda를 단일 authoritative loop로 펼쳤다.
- 공개 send 성공 시 `completion_capacity_blocked=false`와 dispatch recovery clear를 함께 게시하던 여섯 사본을 `clear_public_send_recovery_state` 한 곳으로 모았다. 서로 한쪽만 바꾸는 기존 경로는 의미가 달라 유지했다.

## 3. 책임 분리와 핫패스 정리

### command/progress wait

- `wait_timeout_budget_t`: entry timeout snapshot, deadline, remaining/expired 계산만 소유한다.
- `submit_progress_wait_scope_t`: waiter 등록, 임시 public command-owner 선출, mailbox observation, TLS 설치/복원, owner retire epoch 게시와 broadcast를 한 lifecycle로 소유한다.
- `prepare_pair_submit_command_progress`: PAIR의 기존 async owner 대여와 다른 public owner 대기를 결정한다.
- `prepare_retained_submit_command_progress`: non-PAIR async owner 유지/대여와 zero-I/O-thread synchronous fallback을 결정한다.
- `wait_submit_progress`: 위 결정을 조율하고 commit된 epoch를 기다리는 역할만 남겼다.

`public_command_wait_owner_socket_tls` 성격의 socket별 TLS는 유지했다. owner가 command를 처리하는 동안 `notify_submit_progress`가 재진입할 수 있어 자기 socket wake만 억제해야 하고, 같은 thread의 다른 socket 또는 중첩 owner는 구분해야 한다. TLS 설치/복원은 이제 owner RAII 안에 있다.

### completion/request blocking submit

- 공통 `submit_timeout_budget_t`가 SNDTIMEO entry snapshot과 남은 예산 갱신을 소유한다.
- completion 경로:
  - `completion_submit_wait_context_t`
  - `try_dealer_completion_submit_fast`
  - `wait_for_dealer_completion_submit_target`
  - `wait_for_completion_submit_admission`
- request 경로:
  - `request_submit_selection_t`
  - `try_request_admission_submit_fast`
  - `prepare_request_submit_target`
  - `wait_for_request_submit_admission`

따라서 두 coordinator는 검증 → fast path/target selection → commit된 endpoint admission wait만 읽힌다. lifecycle pin, logical waiter 게시, async completion-owner 대여, errno normalization, multipart input consume 시점은 기존 순서로 유지했다.

### PAIR whole-record와 cache/fallback

- PAIR whole-record는 성공만 반환하는 shortcut으로 한정하고, 모든 refusal은 기존 frame admission/rollback/errno owner인 fallback loop로 보낸다.
- 단일-part는 source를 직접 넘기고 multipart에서 rollback 가능성이 있는 frame만 shallow copy한다. 도달 불가였던 single-part `i > 0` EAGAIN→ECONNABORTED 분기는 제거했다.
- `drive_send_pending`의 호출별 `std::set` node allocation을 admission-gate owner 전용 `blocked_targets_scratch` sorted active prefix로 바꿨다. capacity는 drive 간 재사용하며 allocation failure의 기존 record handoff/terminal 경로는 유지했다.
- LB의 `_ordered`, `_select_scratch`, `_request_limited_scratch` capacity는 per-send가 아니라 pipe `attach` topology 시점에 확보한다.

## 4. 명명·주석 정합

| 이전 | 변경 | 근거 |
|---|---|---|
| `public_command_wait_owner_epoch` | `public_command_wait_owner_retirement_epoch` | 증가 원인이 일반 progress가 아니라 임시 owner retire임을 드러냄 |
| `pair_commands_observed` | `submit_commands_processed` | fallback 전반에서 실제 의미는 PAIR 여부가 아니라 submit command 처리 여부임 |
| `pair_fast_record_valid` | `pair_complete_record_eligible` | record 자체의 validity가 아니라 whole-record shortcut 자격임 |
| local `request_limited` | `_request_limited_scratch` | 호출 간 capacity를 재사용하는 socket-owned scratch임 |
| local `blocked` set | `blocked_targets_scratch` + `blocked_target_count` | 저장 capacity와 이번 drive의 논리적 active prefix를 구분함 |
| TU별 `format_*routing_id_debug` | `router_debug::format_routing_id` | 동일한 ROUTER debug 책임을 한 이름/namespace로 통합함 |

구현을 그대로 설명하던 주석은 줄이고, PAIR shortcut/fallback, TLS self-wake, FQ publication ordering처럼 순서가 필요한 이유만 남겼다.

## 커밋 분리용 파일 목록

동일 파일에 여러 성격의 hunk가 있으므로 아래 중복 파일은 hunk 단위 staging이 필요하다.

### 불필요 코드 제거

- `common/socket_base.cpp`
- `common/socket_base.hpp`
- `common/socket_base_api.cpp`
- `common/socket_base_control.cpp`
- `common/socket_base_endpoint.cpp`
- `common/socket_base_monitor.cpp`
- `common/socket_base_msg.cpp`
- `common/socket_base_routing.cpp`
- `common/socket_close_ops.cpp`
- `common/socket_close_ops.hpp`
- `common/socket_runtime.hpp`
- `dealer/dealer.hpp`
- `internal/dist.hpp`
- `internal/fq.cpp`
- `internal/fq.hpp`
- `router/router.cpp`
- `router/router.hpp`
- `router/router_admission.cpp`
- `router/router_recv_path.cpp`
- `router/router_send_path.cpp`

### 책임 분리·중복 제거·핫패스 cache/fallback

- `common/socket_base.hpp`
- `common/socket_base_lifecycle.cpp`
- `common/socket_base_msg.cpp`
- `common/socket_runtime.hpp`
- `common/socket_send_complete.cpp`
- `common/socket_send_pending_submit.cpp`
- `dealer/dealer.cpp`
- `dealer/dealer.hpp`
- `internal/dist.cpp`
- `internal/fq.cpp`
- `internal/fq.hpp`
- `internal/lb.cpp`
- `internal/lb.hpp`
- `router/router.cpp`
- `router/router.hpp`
- `router/router_admission.cpp`
- `router/router_recv_path.cpp`
- `router/router_send_path.cpp`
- `router/router_debug.hpp` (신규)

### 명명·주석

- `common/socket_base.hpp`
- `common/socket_base_lifecycle.cpp`
- `common/socket_runtime.hpp`
- `common/socket_send_complete.cpp`
- `common/socket_send_pending_submit.cpp`
- `internal/fq.cpp`
- `internal/lb.hpp`

## Gate

- `ulimit -v 16777216; cmake --build core/build -j4`: 성공 (`238/238`)
- `ulimit -v 16777216; ctest --test-dir core/build -j2`: 성공 (`134/134`, 실패 0, real 164.39초)
  - 알려진 `test_single_lane_flow_snapshot_accounting`: 첫 실행에서 통과, 단독 재실행 불필요
- `git diff --check`: 성공, 출력 없음
- raw mirror cmp 12 (`c/cpp/go/rust` × 3 headers): 성공, `MIRROR DIFF` 없음
- perf: 사용자 지시에 따라 실행하지 않음
- binding cpp/python test: 감독관 범위이므로 실행하지 않음

## BLOCKERS

- `core/src/runtime/core/pipe.{hpp,cpp}: pipe_t route-binding cache` — `router_recv_path.cpp:router_t::copy_router_pipe_source_rid`의 receive hot path에 남은 `_out_pipes_sync`와 `_standby_pipes.find`를 topology-time cache로 대체하려면 pipe-owned snapshot field/게시 API가 필요하나 담당 범위 밖이다.
- `core/src/runtime/core/pipe.{hpp,cpp}: pipe_t::check_read_with_record_admission` — `fq_t::has_in_with_record_admission` 제거 후 tracked runtime caller가 0이고 선언/정의만 남았으나 담당 범위 밖이라 삭제하지 않았다.
- `core/CMakeLists.txt: common socket source registration` — 1,929줄인 `socket_send_pending_submit.cpp`의 completion-blocking/request-blocking 책임을 별도 TU로 물리 분리하려면 신규 source 등록 변경이 필요하나 담당 범위 밖이라 이번에는 같은 TU 내부 함수 경계까지만 분리했다.
- 동작 변경이 필요한 것으로 판단해 보류한 항목은 없다.
