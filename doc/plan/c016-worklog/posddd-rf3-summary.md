# Core POSDDD RF3 결과

## 결과

담당 범위의 불필요한 내부 API·필드·분기를 제거하고, pipe lifecycle·write admission·mailbox wake 책임을 각각 한 곳으로 모았다. ROUTER receive 경로에는 pipe가 소유하는 write-once source RID snapshot을 연결했다. current pipe와 한 번이라도 current였던 standby pipe는 receive 시 route mutex나 map 조회 없이 acquire-load와 고정 크기 복사만 수행한다. 처음부터 standby가 된 pipe는 원본 RID를 아직 pipe에 게시하지 않으므로 기존 mutex/map 경로로 후퇴한다.

공개 `core/include/**`, ABI enum, public contract는 변경하지 않았다. `framework/**`, `bindings/**`, `doc/**`, `core/doc/**`, `hotpath_reference.json`은 이 작업에서 수정하지 않았다.

담당 변경은 9개 파일, `+217/-361`, 순감소 144줄이다. 파일 이동은 없다.

## 감독관 commit 분류

### 불필요 코드 제거

대상 파일:

- `core/src/runtime/core/pipe.cpp`
- `core/src/runtime/core/pipe.hpp`
- `core/src/runtime/core/pipe_stream_packet_state.hpp`
- `core/src/runtime/core/ypipe_base.hpp`
- `core/src/runtime/core/ypipe_conflate.hpp`
- `core/src/runtime/core/i_mailbox.hpp`

전 tracked tree를 `git grep -n -w <symbol> HEAD -- .`와 `rg`로 확인했다.

| 삭제 항목 | 호출자 0 근거 |
|---|---|
| `pipe_t::check_read_with_record_admission` | 코드에는 선언·정의만 있었다. 문서의 과거 계획 언급은 호출이 아니다. 이 함수만 쓰던 `probe_pipe_record_admission`도 함께 삭제했다. `pipe_read_admission_probe_t`와 `invoke_pipe_record_admission`은 `read_internal<true>`가 사용하므로 유지했다. |
| `send_hello_msg` | `pipe.hpp` 선언과 `pipe.cpp` 정의만 있었다. |
| `pipe_t::get_connected_time`, `_connected_time` | getter 선언·정의 외 읽기 0이었다. constructor 초기화와 `set_peer_routing_id`의 `time(NULL)` 쓰기도 함께 삭제했고 `<ctime>`을 제거했다. |
| `pipe_t::refresh_inbound_lwm_from_physical_queue` | 선언·정의만 있었다. |
| `pipe_t::reset_stream_connect_event_emitted` | 선언·정의만 있었다. mark 경로와 shared lifetime field는 유지했다. |
| `pipe_t::reset_connection_ready_event_emitted` | 선언·정의만 있었다. mark 경로와 field는 유지했다. |
| `pipe_t::reset_stream_packet_state` | 선언·정의만 있었다. 실제 `close_stream_route`의 state reset은 유지했다. |
| `pipe_t::stream_packet_state() const` | const overload는 선언·정의만 있었다. `stream.cpp`의 유일 호출은 non-const overload를 사용한다. |
| `test_set_stream_packet_allocation_failpoint` | test-only 선언·정의만 있고 호출자 0이었다. 범위 밖 consumer 잔재는 BLOCKERS에 적었다. |
| `ypipe_base_t::probe`, `ypipe_conflate_t::probe` | base pointer 또는 conflate 호출이 0이었다. mailbox는 concrete `ypipe_t::probe`를 사용하므로 그 함수는 유지했다. |

추가 정리:

- `pipe_write_status_t`와 `check_write_status`는 `check_write`와 `check_write_admission` 사이에서만 값을 다시 매핑하던 중간 표현이었다. `check_write_admission`이 기존 순서대로 state를 판정한 뒤 HWM admission을 수행하도록 합쳤다.
- `pipe_t::peer_weight`의 `connection_id_out_`은 모든 tracked caller가 전달하지 않았다. 매개변수와 쓰기 분기를 삭제했다.
- `ypipe_conflate_t::reader_awake`는 `true` 쓰기가 전 트리 0건이어서 `flush()`가 항상 `false`였다. field와 `false` 쓰기를 삭제하고 `flush()`의 기존 결과를 직접 반환한다.
- unsigned `lwm_hint_`에 있던 `> 0 ? value : 0` identity 분기를 제거했다.
- `i_mailbox.hpp`의 미사용 `utils/stdint.hpp`를 제거하고 `command_t`를 전방 선언해 header가 필요한 타입을 직접 밝히게 했다.
- `pipe.hpp`의 미사용 `utils/config.hpp` 의존을 제거하고 config 상수를 실제 사용하는 `pipe.cpp`가 직접 include한다.

### 책임 분리와 hot path 정리

대상 파일:

- `core/src/runtime/core/pipe.cpp`
- `core/src/runtime/core/pipe.hpp`
- `core/src/runtime/core/mailbox.cpp`
- `core/src/runtime/core/mailbox.hpp`
- `core/src/runtime/core/ypipe_conflate.hpp`
- `core/src/runtime/sockets/router/router_recv_path.cpp`

변경 내용:

- `admit_write_unlocked`가 state/transport/remote-flow 판정 뒤 byte-HWM 판정을 수행한다. `write`, `write_and_flush`, `write_no_recursive_hwm_check`, `write_message_observed`가 같은 admission 순서를 공유한다.
- `admit_owner_started_write_unlocked`가 ROUTER가 routing ID를 소비한 뒤 첫 payload admission을 재사용하는 규칙을 소유한다. PAUSE/HWM은 이미 시작한 message의 다음 경계부터 적용하고, termination과 초기 pair hold는 다시 확인하는 기존 동작을 보존했다.
- `transition_to_inactive_state_unlocked`가 `_state` 변경과 `_state_active=false` release 게시를 함께 수행한다. `acknowledge_peer_termination_unlocked`는 반복되던 state 전이, `_out_pipe = NULL` 전환, sink callback, peer ack 순서를 한 곳에서 수행한다.
- `publish_router_route_source_unlocked`가 physical pipe의 application-facing source RID를 `_out_sync` 아래 한 번 복사하고 atomic flag로 게시한다. receive는 flag acquire 뒤 immutable blob을 복사하고 route token을 acquire-load한다. message마다 heap allocation, socket table 조회, pipe lock 획득을 추가하지 않았다.
- `router_t::copy_router_pipe_source_rid`는 pipe snapshot hit를 먼저 사용한다. snapshot miss만 기존 `_out_pipes_sync`와 `_standby_pipes` 조회로 후퇴하므로 first-ever standby의 원본 RID와 token 0 동작을 보존한다.
- `mailbox_t::activate_if_command_pending`가 `recv`와 `probe_command`의 pipe-authoritative activation을 공유한다. public poller가 primary descriptor edge를 먼저 소비해도 command pipe를 다시 확인하는 순서는 유지했다.
- `signal_registered_pollers_unlocked`가 `send`, `signal`, `signal_pollers`의 secondary signaler 순회를 소유한다. `schedule_if_needed_unlocked`는 외부 호출자 0이므로 private으로 이동했다.

Wake 계약 보존 근거:

- HWM 대기는 기존 `arm_hwm_credit_wait_unlocked`의 waiter 게시 → `seq_cst` fence → peer credit 재확인 순서를 그대로 사용한다.
- `apply_remote_flow_state`의 원인별 wake ownership과 inbound credit 쪽 대응 fence는 변경하지 않았다.
- mailbox registered signaler, primary signaler, scheduling 순서를 바꾸지 않았다. `rearm_primary_signaler`와 command-owner detach 호출 경로도 수정하지 않았다.
- conflate pipe는 변경 전에도 `reader_awake=true` 쓰기가 없어 모든 publish가 wake를 요청했다. `flush() == false`를 직접 표현해 같은 lost-wake 방지 동작을 유지하면서 비원자 field의 thread 간 접근을 없앴다.
- route snapshot은 readiness나 command 소비 상태를 변경하지 않는다. demotion/termination과 경합해 이전 odd token을 읽어도 reply 경로가 token과 lifecycle을 다시 검사하고 canonical lookup으로 후퇴한다.

### 명명·가시성·주석

대상 파일:

- `core/src/runtime/core/pipe.cpp`
- `core/src/runtime/core/pipe.hpp`
- `core/src/runtime/core/mailbox.cpp`
- `core/src/runtime/core/mailbox.hpp`
- `core/src/runtime/core/ypipe_base.hpp`
- `core/src/runtime/core/ypipe_conflate.hpp`

| 이전 표현 | 변경 표현 | 이유 |
|---|---|---|
| anonymous pipe state enum | `lifecycle_state_t` | termination lifecycle 상태임을 이름에 표시한다. enum 값과 순서는 유지했다. |
| 분산된 state+HWM 검사 | `admit_write_unlocked` | 한 번의 write admission 책임과 검사 순서를 표시한다. |
| 두 owner-start write의 중복 분기 | `admit_owner_started_write_unlocked` | 이미 시작한 message의 예외 규칙을 일반 write admission과 분리한다. |
| 분산된 `_state_active` 게시 | `transition_to_inactive_state_unlocked` | lifecycle cache 게시 지점을 한 곳으로 제한한다. |
| 반복된 peer termination close/ack 사본 | `acknowledge_peer_termination_unlocked` | callback과 ack 순서를 한 책임으로 묶는다. |
| 반복된 registered signaler loop | `signal_registered_pollers_unlocked` | primary signaler와 secondary poller wake를 구분한다. |
| 반복된 mailbox receiver activation | `activate_if_command_pending` | command pipe가 readiness의 기준이라는 계약을 이름에 표시한다. |

conflate 구현을 되풀이하던 주석과 mailbox의 과거 plan 번호 주석은 제거하고, lost-wake와 publication lock이 필요한 이유만 남겼다. `mailbox_t`의 virtual destructor, `send`, `recv`에는 `ZLINK_OVERRIDE`를 표시했다.

## 줄 수 증감

| 파일 | 추가 | 삭제 |
|---|---:|---:|
| `core/src/runtime/core/i_mailbox.hpp` | 2 | 2 |
| `core/src/runtime/core/mailbox.cpp` | 28 | 30 |
| `core/src/runtime/core/mailbox.hpp` | 7 | 4 |
| `core/src/runtime/core/pipe.cpp` | 129 | 238 |
| `core/src/runtime/core/pipe.hpp` | 32 | 53 |
| `core/src/runtime/core/pipe_stream_packet_state.hpp` | 0 | 2 |
| `core/src/runtime/core/ypipe_base.hpp` | 1 | 4 |
| `core/src/runtime/core/ypipe_conflate.hpp` | 7 | 24 |
| `core/src/runtime/sockets/router/router_recv_path.cpp` | 11 | 4 |
| 합계 | 217 | 361 |

순변화는 `-144`줄이다. 작업 중 별도로 나타난 `core/tests/**` 변경은 이 통계에서 제외했다.

## Gate

모든 명령은 `/home/hep7hep7/project/zlink`에서 실행했다.

| Gate | 결과 |
|---|---|
| `ulimit -v 16777216; cmake --build core/build -j4` | green |
| 관련 test 9개: router handover/concurrent receive, polling, flow state, backpressure, request/reply backpressure, ypipe | 9/9 green |
| `ulimit -v 16777216; ctest --test-dir core/build -j2 --output-on-failure` | 실제 등록 135/135 green, 0 failed, 165.26s |
| `test_single_lane_flow_snapshot_accounting` | 첫 실행 green, 재실행 불필요 |
| `git diff --check` | green |
| raw mirror cmp 12 | green, 출력 없음 |

지시의 기준은 134개였으나 작업 시작 때 clean이던 worktree에 범위 밖 `core/tests/CMakeLists.txt`와 `core/tests/perf/**`가 작업 중 별도로 나타나 `hotpath_gate`가 추가되었다. full CTest는 이 Callgrind instruction-count perf gate까지 포함해 135개를 실행했다. 별도 perf benchmark 명령을 직접 실행하지는 않았지만, 지시된 full CTest를 수행하는 과정에서 외부 추가된 perf gate 1건이 실행됐다.

## Worktree 주의

다음 변경은 이 작업의 `apply_patch` 대상이 아니며 담당 통계·commit 목록에 포함하지 않았다. 감독관은 RF3 commit에 stage하지 않아야 한다.

- `core/tests/CMakeLists.txt`
- `core/tests/perf/**` (untracked, `hotpath_reference.json` 포함)

RF3 변경은 위 줄 수 표의 9개 파일에만 있다. `router_recv_path.cpp`는 사용자가 허용한 단일 범위 밖 교체 지점이다.

## BLOCKERS

- `core/src/runtime/sockets/router/router_admission.cpp:router_t::adopt_peer_routing_id` — first duplicate-loser 분기에서 synthetic RID를 설정하기 전에 `pipe_->publish_router_route_source(routing_id_)`로 원본 RID를 topology 시점에 게시해야 한다. 이 호출이 들어가면 first-ever standby도 receive에서 atomic snapshot만 사용한다.
- `core/src/runtime/sockets/router/router_recv_path.cpp:router_t::copy_router_pipe_source_rid` — 위 topology 게시가 완료되면 `_out_pipes_sync`/`_standby_pipes.find` fallback을 삭제할 수 있다. 현재 fallback은 동작 보존에 필요하다.
- `core/src/runtime/sockets/stream/stream.cpp:stream_t::decode_packet_bytes` — `test_set_stream_packet_allocation_failpoint` 호출자 0 제거 뒤 두 `test_consume_stream_packet_allocation_failpoint` 분기가 영구 false다. 두 consumer를 삭제한 뒤 `pipe.cpp`의 atomic/consumer와 `pipe_stream_packet_state.hpp`의 failpoint enum을 함께 삭제해야 한다.
- `core/src/runtime/utils/dbuffer.hpp:dbuffer_t::probe` — scoped `ypipe_conflate_t::probe` 삭제 뒤 tracked caller가 0이다. 범위 밖 파일에서 삭제해야 한다.
- `core/src/runtime/core/session_base.hpp:session_base_t::reset`, `core/src/runtime/core/session_base.cpp:session_base_t::reconnect/start_transport_pair_reconnect` — `session_base_pipe_io.cpp`의 empty virtual `reset()`을 완전히 삭제하려면 선언과 두 호출을 함께 변경해야 한다.
- `core/src/runtime/core/session_base_pipe_io.cpp:trace_router_session_push` — file-local `ZLINK_DEBUG_ROUTER_ROUTE`는 per-frame ROUTER 진단 trace/logging을 core session I/O에 둔다. 환경변수 진단 동작의 외부 사용 여부를 확인하지 않고 삭제하면 동작이 바뀌므로 보류했다. 제거하거나 ROUTER observability 소유 모듈로 옮기려면 별도 승인이 필요하다.
- `core/src/runtime/core/options.hpp:options_t::hello_msg/can_send_hello_msg` — scoped `send_hello_msg`는 caller 0이어서 삭제했지만 HELLO option field와 owner metadata는 `options.cpp`, `options_owner.*`, DEALER/ROUTER, test에 걸쳐 있다. 계약 확인 뒤 별도 범위에서 정리해야 한다.
