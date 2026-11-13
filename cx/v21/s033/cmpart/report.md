# GitHub #4118 multipart backpressure 진단

## 결과

원인은 C++ binding이 아니라 Core의 complete-record multipart fast path에 있다. Core가 receiver 재개 뒤 `WRITABLE / SEND_ADMITTED`를 발행하고 binding은 같은 staging record를 다시 제출한다. Core의 재제출이 HWM 16 byte에서 다시 거부되므로 pending admission이 끝나지 않는다. 이 진단에서는 제품 코드를 수정하지 않았다. 변경 분류는 B, 기존 Core 결함이다.

Core 스펙 [`core/doc/spec/core/systems/06-auto-hwm.ko.md`](/root/worktree/zlink-4118-multipart/core/doc/spec/core/systems/06-auto-hwm.ko.md:464)는 queue가 비었을 때 시작한 multipart가 final frame에서 HWM을 넘어도 record를 완성할 수 있다고 정한다. Binding 스펙 [`bindings/doc/spec/message-ownership.ko.md`](/root/worktree/zlink-4118-multipart/bindings/doc/spec/message-ownership.ko.md:181)는 multipart 전체를 한 번 제출하고 backpressure 뒤 보관한 record 전체로 재제출한다고 정한다. 실제 binding 제출과 재시도는 이 규칙을 따른다.

## 재현 행렬

재현은 Auto HWM을 끄고 두 ROUTER socket을 inproc으로 연결했다. Source SNDHWM과 target RCVHWM은 기본 16 byte다. Target은 receive flow를 PAUSED로 설정했다. Core C API 프로그램은 source monitor가 pause를 관찰한 뒤 제출하고, 재개 뒤 completion과 수신을 관찰했다. C++ binding 변형도 동일한 socket 구성과 수신 중단·재개 절차를 사용했다.

| 계층 | frame shape 및 HWM | 결과 | 로그 |
|---|---|---|---|
| Core C API | 6 B header + 1 KiB body, HWM 16 | 제출은 BACKPRESSURED, token 1. 재개 뒤 WRITABLE/ADMITTED를 받고 재제출했으나 BACKPRESSURED와 token 2를 받았다. 수신 0건이다. | [raw/core-api-matrix.log](raw/core-api-matrix.log) |
| Core C API | 1 KiB + 4 B tail, HWM 16 | token 1의 writable 뒤 재제출이 다시 BACKPRESSURED, token 2. 수신 0건이다. | [raw/core-api-matrix.log](raw/core-api-matrix.log) |
| Core C API | 6 B + 512 B + 512 B, HWM 16 | token 1의 writable 뒤 재제출이 다시 BACKPRESSURED, token 2. 수신 0건이다. | [raw/core-api-matrix.log](raw/core-api-matrix.log) |
| Core C API | 단일 1 KiB frame, HWM 16 | token 1의 writable 뒤 재제출이 admitted되고 1 frame을 수신했다. | [raw/core-api-matrix.log](raw/core-api-matrix.log) |
| Core C API | 6 B header + 1 KiB body, HWM 2048 | 재개 뒤 재제출이 admitted되고 2 frame을 수신했다. | [raw/core-api-matrix.log](raw/core-api-matrix.log) |
| C++ binding | 6 B header + 1 KiB body, HWM 16 | pending admission은 ready가 되지 않았고 수신 0건이다. | [raw/binding-matrix.log](raw/binding-matrix.log), 원본은 `csrc/raw/native-default-multi.log`, `csrc/raw/native-explicit-multi.log` |
| C++ binding | 1 KiB + 4 B tail, HWM 16 | pending admission은 ready가 되지 않았고 수신 0건이다. | [raw/binding-matrix.log](raw/binding-matrix.log) |
| C++ binding | 6 B + 512 B + 512 B, HWM 16 | pending admission은 ready가 되지 않았고 수신 0건이다. | [raw/binding-matrix.log](raw/binding-matrix.log) |
| C++ binding | 단일 1 KiB frame, HWM 16 | terminal이 성공하고 1 frame을 수신했다. | [raw/binding-matrix.log](raw/binding-matrix.log), 원본은 `csrc/raw/native-default-single.log`, `csrc/raw/native-explicit-single.log` |
| C++ binding | 6 B header + 1 KiB body, HWM 2048 | terminal이 성공하고 2 frame을 수신했다. | [raw/binding-matrix.log](raw/binding-matrix.log) |

Core C API 프로그램은 현재 worktree의 Release Core shared library에 직접 링크했다. C++ binding 재현은 현재 worktree의 `zlink_cpp` Release 정적 라이브러리와 같은 Core library에 링크했다. 구성·빌드 출력은 [raw/core-api-build.log](raw/core-api-build.log), [raw/cpp-config.log](raw/cpp-config.log), [raw/cpp-build.log](raw/cpp-build.log), [raw/binding-build.log](raw/binding-build.log)에 있다. 전체 크기는 25 MiB로 제한인 20 GiB보다 작다.

## 신호와 재제출 추적

Core C API 결과에서 초기 `zlink_send_rid`는 `BACKPRESSURED`, `errno=EAGAIN`, token 1을 반환했다. Receiver 재개 후 Core completion은 `kind=WRITABLE`, `send_result=SEND_ADMITTED`, token 1로 도착했다. 같은 전체 record를 재제출하자 Core가 다시 `BACKPRESSURED`, token 2를 반환했다. 이 순서가 작은 header+큰 body, 큰 frame+작은 tail, 세 frame에서 같았다. 단일 frame 재제출은 성공했고, HWM 2048 multipart 재제출도 성공했다. 원시 값과 수신 결과는 [raw/core-api-matrix.log](raw/core-api-matrix.log)에 있다.

Scratch copy에만 trace를 넣은 C++ binding 실행에서도 `completion kind=WRITABLE id=1 result=SEND_ADMITTED` 뒤 `retry admitted=0 new_id=2 errno=EAGAIN`이 찍혔다. 단일 frame은 같은 completion 뒤 `retry admitted=1 new_id=0 errno=0`으로 끝났다. 따라서 binding은 writable을 놓치지 않고 실제로 재제출한다. 로그는 [raw/binding-trace.log](raw/binding-trace.log), instrumentation scratch는 [completion_owner_trace.cpp](/root/worktree/zlink-4118-multipart/cmpart/completion_owner_trace.cpp)다.

Core resume 흐름은 [`pipe_write.cpp`](/root/worktree/zlink-4118-multipart/core/src/runtime/core/pipe_write.cpp:310)에서 remote flow state를 적용하고, resume 시 HWM readiness를 다시 확인해 write activation을 발행한다([`pipe_write.cpp`](/root/worktree/zlink-4118-multipart/core/src/runtime/core/pipe_write.cpp:401)). Socket owner는 activation 뒤 routed target의 readiness를 검사하고 writable completion을 publish한다([`socket_base_api.cpp`](/root/worktree/zlink-4118-multipart/core/src/runtime/sockets/common/socket_base_api.cpp:1774), [`socket_send_complete.cpp`](/root/worktree/zlink-4118-multipart/core/src/runtime/sockets/common/socket_send_complete.cpp:153)). Core는 이 단계까지 spec에 맞게 동작한다.

C++ binding은 `ZLINK_SEND_ADMITTED` WRITABLE을 재시도 대상으로 반환하고([`completion_owner.cpp`](/root/worktree/zlink-4118-multipart/bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:399), [`completion_owner.cpp`](/root/worktree/zlink-4118-multipart/bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:476)), completion drain에서 같은 entry의 retry를 실행한다([`completion_owner.cpp`](/root/worktree/zlink-4118-multipart/bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:691), [`completion_owner.cpp`](/root/worktree/zlink-4118-multipart/bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:751)). Retry는 원래 operation state를 다시 `submit_raw_send_state`에 전달한다([`completion_owner.cpp`](/root/worktree/zlink-4118-multipart/bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:182)). 그 경로가 Core C API `zlink_send_rid`를 호출하는 지점은 [`operation_submit.hpp`](/root/worktree/zlink-4118-multipart/bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:93)다.

## 소유 계층과 원인

Multipart DONTWAIT 제출은 [`socket_message_send_api.cpp`](/root/worktree/zlink-4118-multipart/core/src/api/socket/socket_message_send_api.cpp:259)에서 part 수가 2 이상이면 `try_send_parts_scoped_once`로 간다. 이 Core 경로는 [`socket_send_submit.cpp`](/root/worktree/zlink-4118-multipart/core/src/runtime/sockets/common/socket_send_submit.cpp:268)에서 complete record를 한 번에 admission한다. Pipe의 [`try_write_complete_record_and_flush`](/root/worktree/zlink-4118-multipart/core/src/runtime/core/pipe_write.cpp:847)는 candidate 전체 charge를 계산하지만 [`can_commit_bytes_with_peer_snapshot_unlocked`](/root/worktree/zlink-4118-multipart/core/src/runtime/core/pipe_write.cpp:884)에 empty-pipe oversize exception을 `false`로 넘긴다. 그래서 empty queue라도 multipart 총 charge가 HWM을 넘으면 fast path는 거절한다. 재시도도 같은 fast path를 지나므로 같은 결과가 반복된다.

반면 단일 frame은 [`socket_message_send_api.cpp`](/root/worktree/zlink-4118-multipart/core/src/api/socket/socket_message_send_api.cpp:263)에서 별도 complete-message 경로로 보내진다. 그 경로는 [`router_send_path.cpp`](/root/worktree/zlink-4118-multipart/core/src/runtime/sockets/router/router_send_path.cpp:489)의 `write_single_message_and_flush_no_recursive_hwm_check`를 거쳐 [`pipe_write.cpp`](/root/worktree/zlink-4118-multipart/core/src/runtime/core/pipe_write.cpp:977)에서 empty-pipe 예외를 허용한다. 따라서 1 KiB 단일 frame은 HWM 16에서도 receiver 재개 뒤 admission된다. HWM 2048 multipart는 전체 charge가 한도 이내라서 exception 없이 admission된다.

소유 spec은 Core Auto HWM §「Multipart와 큰 message」다([`06-auto-hwm.ko.md`](/root/worktree/zlink-4118-multipart/core/doc/spec/core/systems/06-auto-hwm.ko.md:464)). 결정 위치는 Core의 multipart complete-record admission 한 곳이어야 한다. Binding의 native 호출과 retry handoff는 중복 판정이나 누락 원인이 아니다. 양쪽을 대조하면 Core 제출부는 `socket_message_send_api.cpp:263-280` 및 `pipe_write.cpp:884-890`, binding 소비부는 `completion_owner.cpp:399-411` 및 `completion_owner.cpp:182-198`이다. Binding은 Core가 알려준 admission 상태만 소비하며 별도 HWM 판단을 하지 않는다.

## 제안하는 최소 수정 위치와 검증

최소 수정 위치는 `core/src/runtime/core/pipe_write.cpp:884-890`이다. 모든 frame이 있는 complete record를 이미 검증했고 pipe에 기존 incomplete record가 없는 fast path에서, candidate가 empty pipe의 단일 oversize record 예외를 사용할 수 있게 한다. `ZLINK_OPT_MAXMSGSIZE` 검사는 유지한다. Binding에 retry나 상태를 추가할 이유는 없다. 제품 코드는 수정하지 않았다.

수정 뒤에는 Core C API contract test에서 ROUTER-to-ROUTER inproc, HWM 16, receive-flow pause/resume을 설정하고 두 frame·큰 첫 frame·세 frame을 각각 제출해야 한다. 각 경우 token 1의 `SEND_ADMITTED` 뒤 전체 record 재제출이 성공하고 모든 frame을 수신하는지 확인해야 한다. 같은 test에서 단일 frame control, HWM 2048 control, 그리고 `MAXMSGSIZE`가 record보다 작을 때의 거부를 확인해야 한다. Core pipe 단위 test는 complete-record fast path에서 empty queue multipart oversize admission을 직접 검증해야 한다. C++ binding contract test는 async pending admission이 receiver 재개 뒤 완료되고 전체 multipart가 전달되는지 확인해야 한다.

검증 실행은 Core C API 재현 shape matrix와 HWM 2048 control, C++ binding shape matrix, scratch trace 실행이다. 제품 unit test는 실행하거나 추가하지 않았다. 현재 worktree에서 바뀐 항목은 진단 산출물인 `cmpart/`뿐이다.
