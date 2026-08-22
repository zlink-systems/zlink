# Stage 3: Paired completion lane 흐름 상태 (계획 §7 4-6단계)

계획: `doc/plan/autohwm/core-byte-hwm-flow-control-plan.ko.md` §4, §5, §8.1
설계 의도: `doc/plan/autohwm/00-hwm-backpressure-design-intent.ko.md` §4, §5

이 단계는 Core 내부 계층만 구현한다. Public C API, event와 metric은 7단계가 소유하므로
`core/include`와 `bindings/`에는 변경이 없다.

## 1. 전달 경로 선택

Completion lane에는 이미 request/reply completion frame이 흐르고, 그 소비 경로는
`socket_reqrep_internal::process_completion_pipe ()`다. 그러나 이 경로는 completion drain
소유권(`completion_drain_permitted ()`)을 가진 thread에서만 동작한다. 소유자가 없으면
frame이 pipe에 남아 있으므로, PAUSE 적용 시점을 application의 poll 여부에 묶게 된다.

대신 이미 존재하는 **ZMP command frame** 경로를 사용한다. `session_base_t::push_msg_internal ()`
(`core/src/runtime/core/session_base_pipe_io.cpp:165`)은 `msg_t::command` frame을 pipe에 넣지
않고 `socket_base_t::peer_command_from_io () -> xpeer_command ()`로 넘긴다. 즉 어떤 pipe queue에도
들어가지 않으므로 application recv 경로에 노출될 수 없고, 기존 `WEIGHT` peer command와 같은
구조를 그대로 재사용한다. 새 lane, 새 socket, 새 registry를 만들지 않았다.

Inproc paired 연결에는 session이 없어 command frame이 상대 socket의 completion pipe queue로
들어간다. 그 경우를 위해 `process_completion_pipe ()`에도 같은 분류기를 두어, flow frame이
reply dispatcher나 application part로 넘어가지 않게 했다.

## 2. Frame layout

`core/src/runtime/core/flow_state_frame.hpp`

```text
offset  size  field
0       9     command name "FLOWSTATE"
9       1     protocol version (현재 1)
10      1     state (0 RUNNING, 1 PAUSED)
11      8     transport pair id      (big endian)
19      8     connection generation  (big endian)
27      8     flow epoch             (big endian)
총 35 byte, msg_t::command flag
```

Decode 결과는 네 가지다.

| 결과 | 의미 |
|---|---|
| `decode_not_flow_frame` | 우리 frame이 아니다. 호출자가 기존 처리를 계속한다. |
| `decode_unsupported_version` | 지원하지 않는 version이다. 소비하고 버린다. |
| `decode_malformed` | 크기·state·식별자가 계약과 다르다. 소비하고 버린다. |
| `decode_ok` | 적용 후보다. |

`decode_unsupported_version`과 `decode_malformed`도 "소비"로 취급한다. 거절한 frame이 다른
frame handler로 흘러가면 안 되기 때문이다.

## 3. Socket-wide local state와 동기화

`socket_base_t::set_local_receive_flow_state (int)` (내부 C++ 진입점)

- 범위 밖 state는 `EINVAL`, completion lane이 없는 socket 유형은 `ENOTSUP`,
  context 종료는 `ETERM`을 반환한다. Close와의 경쟁은 기존
  `socket_public_api_scope_t` 승인으로 결정한다.
- 저장한 state와 fanout 대상 수집은 `_transport_pairs_sync` 한 개 mutex 안에서 수행한다.
  새 pair가 ready가 될 때의 동기화(`sync_local_receive_flow_state_to_pair ()`)도 같은 mutex로
  같은 state를 읽으므로 fanout과 새 pair가 서로 다른 state를 볼 수 없다.
- 같은 state를 반복 호출하면 성공하고 아무것도 보내지 않는다(절대 상태, counter 아님).
  Epoch는 실제 상태 변화에서만 증가한다.
- 한 번도 state를 설정하지 않은 socket(`epoch == 0`)은 새 pair에 frame을 보내지 않는다.
  RUNNING은 상대가 이미 가정하는 기본값이므로, 이 기능을 쓰지 않는 socket의 연결 경로에
  비용을 추가하지 않는다.

수신 측 검증(`consume_receive_flow_state_frame ()`)

1. Version이 지원 대상이 아니면 버린다.
2. Frame의 pair id·generation이 수신 completion pipe의 것과 다르면 버린다. 이전 physical
   connection의 늦은 frame은 generation이 달라 여기서 걸린다.
3. 같은 generation 안에서 epoch가 전진하지 않으면(중복·역전) 버린다.
4. 상태가 이미 같으면 idempotent no-op이다.
5. 그 외에는 pair 기록을 갱신하고 application pipe에 `flow_state` pipe command를 보낸다.

Frame은 transport I/O thread에서 decode되지만 pipe의 write 상태는 socket thread 소유다.
그래서 상태 적용은 새 `command_t::flow_state` pipe command로 넘긴다(`object.cpp`의 기존
pipe command retain/release 수명 관리를 그대로 사용). 같은 mailbox이므로 연속된 상태의 순서도
보존된다. 같은 이유로 I/O thread가 socket thread보다 먼저 frame을 볼 수 있어,
pair 기록이 아직 없으면 기록만 만들어 두고 `attach_pipe ()`가 application lane을 붙일 때
적용한다.

## 4. Send 차단 원인 합성

`pipe_t`에 `_remote_flow_paused` 원인을 추가했다. 기존 원인과 완전히 독립이다.

```text
send blocked when
  _state != active                (termination)
  OR _transport_pair_write_held   (transport wait)
  OR remote_flow_blocked_unlocked()   (remote PAUSED)
  OR !_out_active / HWM credit 부족   (local byte HWM)
```

- `core/src/runtime/core/pipe.cpp:1071` `write_state_admission_unlocked ()` — write 경로의 합성
- `core/src/runtime/core/pipe.cpp:1116` `check_write_status ()` — writable 판정의 합성
- `core/src/runtime/core/pipe.cpp:1086` `remote_flow_blocked_unlocked ()` — multipart 예외
- `core/src/runtime/core/pipe.cpp:1175` `release_writes_for_transport_pair ()` — transport 원인만 제거
- `core/src/runtime/core/pipe.cpp:1200` `process_flow_state ()` — remote 원인만 제거
- `core/src/runtime/core/pipe.cpp:1374` `process_activate_write ()` — byte credit 원인만 제거
- `core/src/runtime/sockets/common/socket_base_api.cpp:723` `write_activated ()` — 두 wake marker

Remote PAUSE는 `_out_active`, `_bytes_written`, `_peers_bytes_read` 등 byte HWM counter를
전혀 건드리지 않는다. 각 전이는 자기 원인만 지우고, 모든 원인이 사라진 전이에서만
`write_activated ()`(그리고 그 안의 routed send-ready edge)를 발생시킨다. Byte credit이
돌아와도 remote PAUSE가 남아 있으면 `process_activate_write ()`가 `_out_active`만 복구하고
edge는 내지 않는다. 반대 방향도 같다.

Multipart atomicity: `remote_flow_blocked_unlocked ()`는 `_out_incomplete_bytes == 0`일 때만
참이다. 이미 시작한 message의 나머지 frame은 그대로 통과하고, 차단은 다음 message의 첫
frame부터 적용된다.

보고 status: remote PAUSE는 기존 `pipe_write_transport_wait` /
`pipe_message_admission_transport_wait`로 보고한다. 이 값은 이미 모든 호출부에서 `EAGAIN`으로
매핑되므로 새 public send status를 만들지 않는다는 계획 §4.3 제약을 지킨다.
`pipe_message_admission_too_large`는 별도 판정이라 영향이 없다.

## 5. 변경 파일

| 파일 | 변경 |
|---|---|
| `core/src/runtime/core/flow_state_frame.hpp` | 신규 157줄. Frame 계약과 codec |
| `core/src/runtime/core/command.hpp` | `+9 -0`. `flow_state` command 유형과 인자 |
| `core/src/runtime/core/object.hpp` | `+4 -0`. `send_flow_state`, `process_flow_state` |
| `core/src/runtime/core/object.cpp` | `+22 -0`. Command 발행·분배·기본 handler |
| `core/src/runtime/core/pipe.hpp` | `+16 -2`. 원인 flag, 접근자, override |
| `core/src/runtime/core/pipe.cpp` | `+56 -4`. 합성, 전이와 command handler |
| `core/src/runtime/sockets/common/socket_base.hpp` | `+50 -3`. 내부 진입점, pair 기록 field, 상태 member |
| `core/src/runtime/sockets/common/socket_base.cpp` | `+4 -1`. 초기화와 include |
| `core/src/runtime/sockets/common/socket_base_flow_state.cpp` | 신규 223줄. 진입점, fanout, 수신 검증 |
| `core/src/runtime/sockets/common/socket_base_api.cpp` | `+25 -3`. 새 pair 동기화, 보류 상태 적용, wake marker |
| `core/src/runtime/sockets/common/socket_base_dispatch.cpp` | `+6 -0`. `xpeer_command` 분류 |
| `core/src/api/socket/socket_request_reply_dispatch.cpp` | `+11 -0`. 로컬 pair용 분류 |
| `core/CMakeLists.txt` | `+1 -0`. 새 source |
| `core/tests/unittest/unittest_flow_state_frame.cpp` | 신규 181줄 |
| `core/tests/integration/test_flow_state_paired.cpp` | 신규 562줄 |
| `core/tests/unittest/CMakeLists.txt`, `core/tests/CMakeLists.txt` | 각 `+1 -0`. Test 등록 |

## 6. Test 결과

모두 `core/build-tests`(정적 링크, 판정 전 재빌드)에서 실행했다.

`unittest_flow_state_frame` — 9 tests, 0 failures

- state 값이 계약(RUNNING=0, PAUSED=1)과 같다
- 모든 field가 round-trip된다 / RUNNING도 round-trip된다
- command flag가 없거나 다른 command 이름이면 flow frame이 아니다
- 지원하지 않는 version은 거절하되 소비한다
- 범위 밖 state, 잘림, pair 식별자 0은 malformed다

`test_flow_state_paired` — 8 tests, 0 failures (단독 10회 반복 10/10)

- `test_unsupported_socket_types_report_not_supported` — PAIR·PUB·SUB·XPUB·XSUB·STREAM이
  `ENOTSUP`이고, 거절 뒤에도 PAIR 송수신이 그대로 동작한다
- `test_invalid_state_is_rejected` — 범위 밖 `EINVAL`, 같은 state 반복 호출 성공
- `test_remote_pause_blocks_sender_and_resume_releases_it` — 실제 TCP completion lane으로
  PAUSE가 전달되어 sender가 막히고 RUNNING이 풀어준다
- `test_local_hwm_and_remote_pause_are_independent` — HWM만 해제해도, remote PAUSE만
  해제해도 writable이 되지 않는다(양방향)
- `test_pause_mid_multipart_preserves_atomicity` — 시작한 2-part message는 완료되고 다음
  message부터 막히며, 수신 측은 온전한 2-part message를 본다
- `test_duplicate_and_stale_frames_are_ignored` — 같은 epoch, 역전 epoch, 이전 generation,
  다른 pair, 지원하지 않는 version이 모두 안전하게 무시된다
- `test_new_and_reconnected_pairs_receive_the_latest_state` — pair가 생기기 전에 저장한
  PAUSED가 새 연결과 재연결 pair에 동기화되고, RUNNING도 재연결 pair에 도달한다
- `test_no_application_recv_returns_a_flow_frame` — PAUSE/RUNNING을 반복해도 application
  recv에는 아무것도 오지 않고, 정상 traffic만 그대로 흐른다

계획 §8.1 focused 8개 + `unittest_poller` + `test_timer_poller` + 새 test 2개:

```text
ctest -R '^(test_zmp_request_reply|unittest_auto_hwm_policy|unittest_zmp_decoder|test_ctx_options
  |test_retained_hwm_credit|test_router_handover|test_connect_rid|test_router_mandatory_hwm
  |unittest_poller|test_timer_poller|unittest_flow_state_frame|test_flow_state_paired)$'
=> 100% tests passed, 0 tests failed out of 12 (57.74s)
```

`test_retained_hwm_credit`은 같은 round에 별도로 고친 뒤 단독 20회 20/20이며 이 실행에서도
통과했다(`stage2-retained-credit-fix.md` 참고).

### 6.1 전체 sweep과 기존 실패

`ctest --test-dir core/build-tests` 전체 89개: 85 passed, 4 failed.

| Test | 판정 |
|---|---|
| `test_xpub_nodrop` (`:327`) | 기존 실패 |
| `test_zmp_metadata` (`:506`) | 기존 실패 |
| `test_router_multiple_dealers` (`:714`) | 기존 실패 |
| `test_backpressure_matrix_pubsub_regression` | 전체 실행에서만 timeout, 단독 실행 통과(host 부하) |

앞 세 개는 이 단계 직전 commit `07bb04fc4f`을 별도 worktree로 빌드해 같은 위치·같은 메시지로
재현했다. 이 단계의 변경과 무관한 기존 실패다.

## 7. 남은 범위(이번 단계 아님)

- Public `zlink_socket_set_receive_flow_state` C API, config result 매핑, event와 metric은
  계획 §7의 7단계가 소유한다. 그래서 §12.3의 "C API, event와 metric" 행은 열어 둔다.
- 이번 단계는 perf를 실행하지 않았다. "항상 RUNNING인 paired perf" 행도 열어 둔다.
  Perf runtime(`core/build`)은 이 round에서 건드리지 않았다.
- Inproc paired 연결의 flow frame은 completion drain이 도는 시점에 적용된다. Network
  transport(`xpeer_command`)는 이 제약이 없다. 첫 계약 범위가 paired DEALER/ROUTER의 실제
  transport이므로 test도 TCP로 작성했다.

## 8. Codex review 지적 사항 수정 (HIGH 3 + MEDIUM 3 + LOW 1)

각 항목은 **먼저 재현 test를 작성해 red를 확인한 뒤** 수정하고 green을 확인했다. 한 항목이
한 commit이며 test와 수정이 같은 commit에 들어 있다. 모든 실행은 `core/build-tests`이고,
판정 전에 항상 재빌드했다. Perf는 실행하지 않았고 `core/build`는 건드리지 않았다.

| # | 심각도 | 지적 | Test | Red 증거 | Commit |
|---|---|---|---|---|---|
| 1 | HIGH | Attach replay가 더 새로운 epoch를 stale 상태로 덮어씀 | `test_stale_flow_state_command_cannot_override_a_newer_epoch` | `:539 Expected FALSE Was TRUE` (socket 기록은 RUNNING인데 pipe는 PAUSED) | `a88fb98b28` |
| 2 | HIGH | HWM full 중 RESUME이 writable wakeup을 영구 상실 | `test_resume_while_hwm_full_still_recovers_through_byte_credit` | `:567 Expected TRUE Was FALSE` (drain 뒤에도 route 복귀 실패) | `1b51426885` |
| 3 | HIGH | Classic ROUTER routing-ID part의 multipart atomicity 파손 | `test_router_routing_id_part_holds_message_atomicity_across_pause` | `:590 Expected 7 Was -1` (수락된 message의 payload part가 EAGAIN) | `4e16bd78b6` |
| 4 | MEDIUM | Transport hold 해제 전에 PAUSE가 적용되지 않음 | `test_ready_pair_with_pending_pause_publishes_no_writable_edge` | `:552 Expected 0 Was 1` (writable edge 1건 발생) | `7dbb711d75` |
| 5 | MEDIUM | Application lane의 FLOWSTATE가 수락됨 | `test_flow_frame_on_the_application_lane_is_rejected` | `:549 Expected FALSE Was TRUE` (data lane frame이 pause 적용) | `318e5e64d4` |
| 6 | MEDIUM | Inproc에서 잘못 놓인 FLOWSTATE가 reply callback에 도달 | `test_flow_frame_after_envelope_parts_is_still_consumed_on_a_local_pair` | `:567 Expected TRUE Was FALSE` (frame이 reply payload로 흡수됨) | `0689fb01c7` |
| 7 | LOW | Peer readiness가 remote flow 상태를 무시 | `test_router_peer_state_reports_remote_pause` | `:587 Expected 0 Was 2` (PAUSED인데 POLLOUT 보고) | `8bff19fbef` |

### 8.1 수정 내용

1. **Epoch를 pipe command에 실어 stale replay를 버린다.** `command.hpp`의 `flow_state` 인자에
   `epoch`를 추가하고 `pipe.cpp:1248`에서 전진하지 않는 epoch를 무시한다. Attach replay와
   I/O thread의 수락이 어느 쪽이 나중에 queue되든 결과가 같다.
   (`socket_base_api.cpp:258`가 snapshot한 epoch를 함께 전달한다.)
2. **RESUME이 남은 HWM 차단을 byte-credit 원인에 넘긴다.** `pipe.cpp:1264` — HWM이 여전히
   full이면 `_out_active`를 내리고 `_waiting_for_byte_credit`를 세워, credit 복귀 경로가
   edge를 발행할 수 있게 한다. Credit이 이미 있으면 `_out_active`를 복구하고 edge를 낸다.
3. **수락했지만 아직 쓰지 않은 part를 message 시작으로 선언한다.** `pipe.cpp:1098`
   `mark_out_message_started ()`를 `router_send_path.cpp:115`에서 호출하고, marker는 message
   commit(`pipe.cpp:2344`), rollback(`pipe.cpp:2406`), hiccup(`pipe.cpp:1531`)에서 pipe가
   스스로 지운다. Router의 어떤 exit 경로도 marker를 흘릴 수 없다.
4. **보류 상태를 hold 해제 전에 동기 적용한다.** `pipe_t::apply_remote_flow_state ()`
   (`pipe.cpp:1237`)를 `socket_base_api.cpp:258`에서 직접 호출한다. Queue된 command가 돌
   thread와 같은 thread이므로 안전하고, 이후 `release_writes_for_transport_pair ()`가 remote
   원인을 보고 edge를 내지 않는다.
5. **Lane을 검증한다.** `socket_base_flow_state.cpp:187`에서 수신 pipe가 completion lane이
   아니면 버리고, `:214`에서 pair가 이미 completion pipe를 알고 있으면 그 pipe만 허용한다.
   Pair admission을 추월한 frame은 pipe 자신의 lane 속성이 출처를 증명하므로 계속 수락된다.
6. **위치와 무관하게 분류한다.** `socket_request_reply_dispatch.cpp:112` — 어느 위치의
   FLOWSTATE도 소비하고, 그 frame이 message를 끝냈다면 쌓인 part를 dispatcher에 넘겨
   거절·해제시킨다.
7. **Readiness를 admission과 일치시킨다.** `router_recv_path.cpp:523`에서
   `remote_flow_blocks_next_message ()`를 함께 본다.

### 8.2 Test 전용 hook

모두 `#ifdef ZLINK_BUILD_TESTS`이며 hot path에 없다(`core/build`는 `ZLINK_BUILD_TESTS=OFF`라
컴파일되지 않는다).

- `socket_base_t::test_pair_pipe ()` — pair의 한쪽 lane pipe
- `socket_base_t::test_application_pipe_flow_probe ()` — 어떤 원인도 평가하지 않고 읽기만
  하므로 관찰이 상태를 바꾸지 않는다
- `socket_base_t::test_deliver_flow_state_command ()` — epoch를 지정해 상태 command를 queue
- `socket_base_t::test_transport_write_release_edges ()` — hold 해제로 발행된 writable edge 수
- `socket_base_t::test_flow_frame_accepted_before_pair_ready ()` — test가 의도한
  frame-먼저/admission-나중 순서를 실제로 만들었는지 확인하는 precondition
- `pipe_t::test_flow_probe ()` — 위 probe의 pipe 측 구현

Counter member `_test_transport_write_release_edges`는 class layout이 build 설정에 따라
달라지지 않도록 모든 build에 존재하고(같은 class의 기존 규칙), 쓰기·읽기만 test build로
제한했다(`112b12a1f7`).

### 8.3 수정 뒤 실행 결과

- `test_flow_state_paired` 15 tests 단독 10회 → 10/10 통과
- `unittest_flow_state_frame` 9 tests 단독 10회 → 10/10 통과
- Focused 8 + `unittest_poller` + `test_timer_poller` + 새 test 2개 →
  `100% tests passed, 0 tests failed out of 12` (58.96s)
- 전체 sweep 89개 → 86 passed. 실패 3개는 §6.1과 같은 기존 실패
  (`test_xpub_nodrop:327`, `test_router_multiple_dealers:714`, `test_zmp_metadata:506`)이며
  이 작업 범위 밖이다(다른 작업자가 isolated worktree에서 담당).
- 한 번 batch 실행에서 `test_zmp_request_reply`가 180s timeout으로 실패했으나 단독 3회
  38.66/38.52/38.85s로 재현되지 않았다. 같은 host에서 perf 측정이 병행 중인 부하 영향이다.

## 9. Codex 재검토 round 2 (MEDIUM 4 + 검증 1 + 문서 1)

Round 1과 같은 절차다. 항목마다 **재현 test를 먼저 작성해 red를 확인**하고 수정한 뒤 green을
확인했다. 한 항목이 한 commit이고 test와 수정이 같은 commit에 있다. 모두 `core/build-tests`이며
perf는 실행하지 않았다. Registry dual-ledger(`_registry_accounting`) 경로는 다른 작업자가
별도 worktree에서 담당하므로 건드리지 않았다.

| # | 지적 | Test | Red 증거 | Commit |
|---|---|---|---|---|
| R1 | RESUME이 stale cached credit으로 판단해 wakeup을 영구 상실 | `test_resume_rereads_credit_published_before_the_waiter_was_armed` | `:624 Expected TRUE Was FALSE` (drain 없이 route 복귀 불가) | `17f18aaf33` |
| R2 | Attach의 상태 적용이 linearize되지 않아 stale snapshot이 hold를 해제 | `test_pause_accepted_inside_the_attach_window_blocks_the_writable_edge` | `:605 Expected 0 Was 1` (writable edge 1건) | `505264dabe` |
| R3 | Overtake 허용이 미검증 pipe의 frame을 적용하고 epoch를 소모 | `test_overtaking_flow_frame_is_buffered_until_the_pair_is_validated` | `:560 Expected TRUE Was FALSE` (buffer 없이 즉시 적용) | `0367cebca4` |
| R4 | 소비된 flow frame이 자른 message가 유효한 빈 reply로 완료됨 | `test_flow_frame_cannot_complete_a_truncated_reply` | `:649` handler가 `ZLINK_REQUEST_OK`로 호출됨 | `bea276f957` |
| R5 | Epoch edge case 검증 | `test_flow_state_epoch_edge_cases`, `test_generation_change_resets_the_epoch_sequence` | `:596 Expected TRUE Was FALSE` (socket epoch가 0으로 wrap) | `eb0b9a5c65` |
| R6 | `socket_runtime.hpp`의 조건부 field | 문서화만 (아래 §9.2) | — | 이 문서 |

### 9.1 수정 내용

**R1** `pipe.cpp:1257-1275`. RESUME이 남은 HWM 차단을 byte-credit 원인에 넘길 때 **먼저 waiter를
세우고 그 다음** `check_hwm_with_peer_snapshot_unlocked ()`로 peer가 실제로 publish한 credit을
다시 읽는다. 고전적인 lost-wakeup 규율이다. Arm 이후에 credit을 publish하는 reader는 armed
waiter를 보고 스스로 activation을 보내고, arm 이전에 publish한 reader는 이 재확인이 잡는다.
Cached snapshot으로 판단하면 waiter가 없던 동안의 sub-LWM read — 마지막으로 자격이 있던 read —
를 놓친다.

**R2** `socket_base_api.cpp:253-283`. 상태 적용과 transport hold 해제를 `_transport_pairs_sync`
아래 한 단계로 묶었다. Transport I/O thread는 frame을 수락할 때 같은 mutex로 record를 갱신한
뒤에야 자기 command를 queue하므로, 이 자리에서 record를 다시 읽는 것이 두 생산자를
linearize한다. 두 호출 모두 pipe의 lock만 잡고 결정값만 반환하며, edge는 mutex를 놓은 뒤
발행한다.

**R3** `socket_base_flow_state.cpp:214-232`, `promote_pending_flow_state_locked ()`. Pair가 ready가
되기 전에 도착한 frame은 pair마다 최신 것 하나만 보관하고 accepted state와 epoch를 건드리지
않는다. Pair가 ready가 될 때(`socket_base_api.cpp:262`) 승격하되, frame이 도착한 pipe가 등록에
성공한 pipe와 같을 때만 적용하고 아니면 버린다. Passive transport에서 peer가 metadata로 pair
식별자를 제공하므로, 등록에 실패할 연결이 상태를 세우거나 epoch를 태우지 못한다.

**R4** `socket_request_reply_dispatch.cpp:104-146`. 소비된 flow frame이 message를 끝냈고 앞에
쌓인 part가 있으면 그 message를 malformed로 표시해 dispatcher에 넘기지 않고 닫는다.
`parse_envelope`는 more-flag를 검사하지 않으므로 그대로 두면 4개 control part가 유효한 빈
reply로 해석되어 대기 중인 request를 성공 완료시킨다. 같은 자리에서 command flag 검사를
inline으로 끌어올려 일반 reply part는 분류 호출 비용을 내지 않는다.

**R5** `socket_base_flow_state.cpp:66-71`. Socket 전역 epoch가 `UINT64_MAX`에서 0으로 wrap하지
않고 1로 넘어간다. 0은 "설정된 적 없음" 표식이고 frame 계약이 거부하는 값이라, 0으로 wrap하면
그 socket의 흐름 상태가 영구히 침묵한다. Transport pair generation이 이미 쓰는 규칙과 같다.

나머지 epoch edge는 이미 성립함을 test로 확인했다: epoch가 없는 pipe는 어떤 값이든 첫 상태로
받고, 같은 epoch와 낮은 epoch는 무시하며, `UINT64_MAX`도 전진값으로 받는다. 그 위로는 해당
generation에서 더 전진하지 않는데, 이는 의도한 trade-off다 — 순서는 generation 안에서만
단조롭고, generation이 바뀌면 새 pipe가 새 sequence로 시작한다는 것을
`test_generation_change_resets_the_epoch_sequence`가 검증한다.

### 9.2 알려진 한계 (R6, 문서화만)

`core/src/runtime/sockets/common/socket_runtime.hpp:228-250`의 `socket_receive_runtime_t`는
`public_mailbox_drains`, `async_mailbox_drains`, `wait_hook`, `wait_hook_userdata`를
`#ifdef ZLINK_BUILD_TESTS`로 감싸고 있어 build 설정에 따라 struct layout이 달라진다. In-tree
build는 `core/CMakeLists.txt:1426`의 `add_compile_definitions`가 library와 test 모든 target에
같은 값을 주므로 일관적이다. 서로 다른 macro 설정으로 빌드된 object를 out-of-tree에서 섞으면
ODR 문제가 될 수 있다. 이번 작업 범위가 아니므로 구조를 바꾸지 않고 한계로만 기록한다. 이번에
추가한 counter(`_test_transport_write_release_edges`)는 같은 위험을 만들지 않도록 모든 build에
존재한다(§8.2).

### 9.3 Round 2 이후 실행 결과

- `test_flow_state_paired` 21 tests 단독 10회 → 10/10 통과
- `unittest_flow_state_frame` 9 tests 단독 10회 → 10/10 통과
- Focused 8 + `unittest_poller` + `test_timer_poller` + flow test 2개 →
  `100% tests passed, 0 tests failed out of 12` (60.42s)
- 전체 sweep 89개 → 86 passed. 실패 3개는 §6.1·§8.3과 같은 기존 실패이며 다른 작업자 소관이다.
- `git diff --check` 통과.

### 9.4 Round 2에서 추가한 test 전용 hook

모두 `#ifdef ZLINK_BUILD_TESTS`다.

- `pipe_t::test_flow_probe ()`에 byte-credit waiter와 in-flight byte를 추가 — 어떤 원인도
  평가하지 않으므로 관찰이 상태를 바꾸지 않는다
- `socket_base_t::test_set_attach_flow_window_hook ()` — attach의 pair admission과 hold 해제
  사이 창에서 실행되어, 경쟁 생산자가 그 창을 이기게 만든다
- `socket_base_t::test_pending_flow_buffered ()`, `test_any_pair_accepted_flow_state ()`
- `socket_base_t::test_set_local_receive_flow_epoch ()`, `test_local_receive_flow_epoch ()` —
  2^64회 상태 변경 없이 wraparound 경계에 도달하기 위한 것

## 10. 3차 검토: 검증 계층을 늘리는 대신 구조를 단순화한다

1·2차 수정은 경쟁을 하나씩 막느라 검증 장치를 세 겹으로 쌓았다: command에 실은 epoch,
overtake buffer와 promotion, 그리고 publish 시점의 sequence 검증. 사용자가 지적한 대로 이는
우발적 복잡도다. 구현 전에 두 단순화를 먼저 평가했다.

### 10.1 평가: 순서 보장이 실제로 성립하는가

두 단순화는 모두 "frame 적용을 socket thread로 넘기면 순서가 저절로 성립한다"는 한 가지
사실에 기댄다. 코드로 확인했다.

1. **Session은 frame보다 먼저 `bind`를 socket mailbox에 넣는다.**
   `asio_zmp_engine.cpp:260`에서 handshake가 끝나면 `session()->engine_ready()`가 먼저
   호출되고, 그 안에서 `session_base.cpp:395`/`:399`가 `send_bind (_socket, ...)`로 socket
   mailbox에 `bind`를 넣는다. 그 뒤에야 같은 I/O thread가 `push_msg`로 frame을 밀어 넣는다
   (`:275`, `:521`, `:589`). Handshake stage가 없는 engine은 `process_attach`
   (`session_base.cpp:305-311`)에서 `plug()`보다 먼저 `engine_ready()`를 부른다. 어느
   경로든 `bind`가 먼저다.
2. **Socket의 command 처리는 직렬화되어 있다.** `process_commands`는
   `socket_base_lifecycle.cpp:111`의 `receive.command_owner_sync`로 단일 소유자만 진행한다.

따라서 flow frame의 **적용**을 socket mailbox로 넘기면, mailbox FIFO에 의해 socket thread는
언제나 `bind`(= completion pipe 등록)를 먼저 처리한 뒤에 flow state를 적용한다.

### 10.2 A: overtake buffer 삭제 — 성립하되 이유가 다르다

Coordinator가 제안한 근거(§4.2의 ready-resync가 뒤늦게 상태를 다시 보낸다)는 **성립하지
않는다**. Resync는 *보내는 쪽* pair가 ready가 될 때 한 번 발생하고, 받는 쪽 pair가 나중에
ready가 되는 것을 보내는 쪽은 알지 못한다. 그러므로 등록 전 frame을 그냥 버리면 그 상태는
영구히 사라진다.

그러나 10.1의 순서 보장 때문에 **애초에 등록 전 frame이라는 상황이 발생하지 않는다.** 적용을
socket thread로 넘기면 completion pipe는 항상 이미 등록되어 있다. Overtake는 사라지고,
buffer·promotion·per-source slot·teardown 정리는 전부 불필요해진다. 결론: **A 채택**,
단 근거는 "drop 후 resync가 수습한다"가 아니라 "overtake 자체가 불가능해진다"이다.

### 10.3 B: 모든 적용과 edge 발행을 socket thread로 직렬화 — 창이 사라진다

현재 이탈 지점은 두 곳이다. (a) I/O thread가 `consume_receive_flow_state_frame`에서 직접
record를 바꾸고 pipe command를 queue한다. (b) `attach_pipe`가 mutex를 놓은 뒤 edge를
발행한다.

수락을 socket thread로 옮기면 flow state의 생산자는 하나가 된다. `attach_pipe`의 ready 경로도
`process_bind`, 즉 같은 command 처리 안에서 돈다. 결정과 발행 사이에 다른 수락이 끼어들 수
없으므로 **sequence 검증 counter가 불필요**하다. 같은 이유로 **pipe command의 epoch도
불필요**하다: 단일 writer가 언제나 record의 최신값을 pipe에 반영하므로 stale replay가 생기지
않는다. Record 수준의 epoch 검사는 wire frame의 중복·역전을 거르는 본래 역할로 남는다.

`connect_internal`이 application thread에서 `attach_pipe`를 부르는 경로는 남지만, 그 시점의
pair에는 completion pipe가 없어 수락이 향할 수 없다. Ready 전이는 언제나 `process_bind`에서
일어난다.

### 10.4 C: epoch wrap

단일 writer가 되어도 wire epoch는 남으므로 `UINT64_MAX` 처리 규칙은 필요하다. 2차에서 넣은
규칙(wrap이면 generation을 새로 만들고, epoch 0은 어디서도 유효하지 않음)이 가장 단순한
정의이고 이미 test가 있으므로 그대로 둔다. 다만 pipe 계층이 사라지므로 "모든 수신 계층"은
decode와 record 적용 두 곳으로 줄어든다.

### 10.5 삭제 대상과 추가 대상

삭제: `pending_flow_slot_t`와 slot 배열, `buffer_pending_flow_state_locked`,
`promote_pending_flow_state_locked`, `discard_pending_flow_state_locked`와 그 호출,
`_flow_state_sequence`와 bump·검증, `pipe_t::_remote_flow_epoch`와
`pipe_t::process_flow_state`, `flow_state`의 pipe command 성격, 그리고 사라진 장치를 재던
test hook들(`test_pending_flow_buffered`, `test_any_pair_accepted_flow_state`,
`test_buffer_flow_frame`, `test_flow_frame_accepted_before_pair_ready`, publish-window hook).

추가: socket을 목적지로 하는 `flow_state` command 인자 세 개와
`socket_base_t::process_flow_state ()`/`apply_receive_flow_state ()`.

계약을 주장하던 test는 남긴다. 사라진 장치를 직접 재던 test는 같은 계약을 새 구조에서
확인하는 형태로 바꾼다.

## 11. 설계상 허용하는 transient

판정 기준은 `00-hwm-backpressure-design-intent.ko.md` §2.3이다. Control state가 늦거나
유실돼도 finite byte HWM과 TCP backpressure가 memory burst를 결국 제한한다. 즉 flow state는
hint 계층이고, 늦거나 유실되거나 잠시 어긋난 PAUSE는 설계가 허용한다. 장치를 정당화하는 것은
아래 다섯 개의 hard contract뿐이다.

(a) 영구 정지 없음(writable은 반드시 결국 회복) (b) flow frame이 application에 절대 보이지
않음 (c) 시작한 multipart는 완료됨 (d) malformed·truncated reply가 성공으로 보고되지 않음
(e) 기존 send 의미(EAGAIN·SNDTIMEO)가 변하지 않음.

| 허용하는 transient | 왜 허용되는가 |
|---|---|
| `attach_pipe`가 mutex를 놓은 뒤 edge를 발행하는 사이에 새 상태가 수락되면, 그 edge가 잠시 낡은 상태를 반영한다 | 뒤이은 send는 EAGAIN을 받거나 byte HWM 아래에서 처리된다. Queue된 상태가 곧 적용되어 수렴한다. (a)를 깨지 않는다 |
| Pair의 completion pipe가 등록되기 전에 도착한 frame은 버려진다 | 버려질 수 있는 frame은 아직 RUNNING인 pipe에 대한 PAUSE뿐이다(PAUSE 상태인 pipe는 이미 등록을 마친 pair를 뜻한다). 따라서 throttling이 늦어질 뿐 route가 멈추지 않고, 늦어지는 비용은 byte HWM이 제한한다 |
| Message-start marker를 relaxed atomic으로 두어, 다른 thread의 상태 적용과 겹치면 PAUSE 적용이 한 message 밀릴 수 있다 | Send 측은 자기 store를 항상 관찰하므로 (c)는 유지된다. 한 message 지연은 hint 계층의 허용 범위다 |
| Peer가 상태를 바꾸지 않는 한 유실된 PAUSE는 재전송되지 않는다 | §4.2의 resync는 *보내는 쪽* pair가 ready가 될 때 발생하므로 받는 쪽의 뒤늦은 등록을 보상하지 않는다. 이를 보상하려면 새 wire 요청이 필요한데, 유실된 PAUSE는 정지가 아니라 throttling 지연이므로 §2.3이 이미 덮는다 |

이전 회차에서 이 transient들을 막으려고 넣었던 sequence counter, pre-validation hold,
per-source slot, connection-id 명명, teardown 정리는 모두 제거했다. 그 부재를 단언하던 test도
제거했고(사유는 위 표), hard contract를 단언하는 test는 전부 남겼다.

## 12. 자체 검토

### 12.1 문서 적합성

| 문서 조항 | 구현 위치 |
|---|---|
| 계획 §4.1 기존 completion lane 사용, 새 control socket 없음 | `socket_base_flow_state.cpp:128` `write_receive_flow_state_frame ()`이 pair의 completion pipe에만 쓴다. 새 socket·lane·registry 없음 |
| 계획 §4.1 첫 계약은 paired DEALER/ROUTER, 나머지는 기존 byte HWM 유지 | `socket_base_flow_state.cpp:14` `socket_type_supports_receive_flow_state ()`; 다른 유형은 `:29`에서 `ENOTSUP` |
| 계획 §4.2 frame field 5종 | `core/src/runtime/core/flow_state_frame.hpp` (35 B, version·pair id·generation·epoch·state) |
| 계획 §4.2 절대 상태, 반복 적용은 idempotent | `socket_base_flow_state.cpp:183` `consume_receive_flow_state_frame ()`의 같은 상태 조기 반환; `set_local_receive_flow_state ()`의 같은 상태 무동작 |
| 계획 §4.2 지원하지 않는 version 거절 | `flow_state_frame.hpp` `decode_frame ()`의 `decode_unsupported_version` |
| 계획 §4.2 이전 generation·중복 epoch 무시 | `socket_base_flow_state.cpp:183`의 generation 일치 검사와 epoch 전진 검사 |
| 계획 §4.2 새 pair ready 시 최신 local state 전송 | `socket_base_api.cpp:238` `sync_local_receive_flow_state_to_pair ()` |
| 계획 §4.3 차단 원인 합성 | `pipe.cpp:1073` `write_state_admission_unlocked ()`, `pipe.cpp:1132` `check_write_status ()` |
| 계획 §4.3 remote PAUSE가 byte HWM counter를 수정하지 않음 | `pipe.cpp:1088` `remote_flow_blocked_unlocked ()`는 읽기 전용이고 `_out_active`·`_bytes_written`을 건드리지 않는다 |
| 계획 §4.3 각 전이는 자기 원인만 제거 | `pipe.cpp:1248` `apply_remote_flow_state ()`, `release_writes_for_transport_pair ()`, `process_activate_write ()` |
| 계획 §4.3 multipart atomicity | `pipe.cpp:1088`의 `_out_incomplete_bytes`·owner marker 조건, `router_send_path.cpp:115` |
| 계획 §4.3 새 public send status 없음 | 기존 `pipe_message_admission_transport_wait`로 보고 |
| 설계 의도 §4.2 raw control·selective receive·우회 send 없음 | 추가된 공개 표면 없음. Frame은 command frame이라 `session_base_pipe_io.cpp:165`에서 pipe queue에 들어가지 않는다 |
| 설계 의도 §5 EAGAIN·SNDTIMEO 불변 | 기존 status로만 보고하므로 호출부 변경 없음 |

문서보다 더 하는 것으로 남은 장치는 두 가지이고, 각각 hard contract가 근거다. (1) pipe
command에 실은 epoch — attach 재적용이 더 새로운 상태를 덮어 pipe를 영구 PAUSED로 고정하는
것을 막는다(a). (2) RESUME 시 byte-credit waiter 인계와 재확인 — 그 인계가 없으면 credit
복귀 경로가 edge를 내지 않아 route가 영구히 비활성으로 남는다(a).

### 12.2 복잡도

`core/src` 기준 기능 전체 순증은 **+919/−12**이고, 이번 회차에서 **−795/+88** 을 되돌렸다.
남은 구조는 frame codec 1개 파일(157줄), socket 진입점 1개 파일, pipe의 원인 flag 하나와
marker 하나, 그리고 세 곳의 통합점(`xpeer_command`, completion drain, router 송신)이다.

스스로 우발적 복잡도로 부르고 싶은 잔여물은 없다. 남은 것 중 가장 논쟁적인 두 가지는 12.1
끝에 적은 epoch와 credit 인계이며, 둘 다 (a)를 직접 지킨다. Epoch wrap 규칙은 10줄이고
`UINT64_MAX`번의 상태 변경이라는 도달 불가능한 조건을 정의만 해 둔다.

### 12.3 성능 위험

Message마다 실행되는 경로에 추가된 것은 다음이 전부다. Lock·할당·syscall 추가는 없다.

| 위치 | 추가된 것 | 빈도 |
|---|---|---|
| `pipe.cpp:1073` send admission | `remote_flow_blocked_unlocked ()` — bool 3개와 uint64 1개 비교. 이미 잡고 있는 `_out_sync` 안 | Frame마다 |
| `pipe.cpp:1132` `check_write_status` | 같은 비교 | Admission 확인마다 |
| `pipe.cpp:2374` message commit | relaxed atomic store 1회. 이미 잡고 있는 lock 안 | 완성된 message마다 |
| `router_send_path.cpp:115` | relaxed atomic store 1회, **lock 없음** | Classic ROUTER message마다 |
| `socket_request_reply_dispatch.cpp:119` | `flags & command` inline 분기. 일반 reply part는 호출 자체를 건너뛴다 | Completion part마다 |
| `pipe.cpp:1479` `process_activate_write` | 비교 1회 | Credit 복귀마다(message마다 아님) |
| `socket_base_api.cpp:758` `write_activated` | atomic exchange 1회 | Write 활성화마다(message마다 아님) |

Recv 경로와 decoder에는 추가가 없다. Flow frame 자체는 상태가 바뀔 때만 흐른다.
`router_send_path.cpp:115`의 marker는 처음에 `_out_sync`(실제 recursive pthread mutex)를
잡았는데, ROUTER message마다 mutex를 하나 더 잡는 것이라 relaxed atomic으로 바꿨다.
