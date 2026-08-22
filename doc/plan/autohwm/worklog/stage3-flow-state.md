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
