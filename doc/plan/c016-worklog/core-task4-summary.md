# Core task 4 결과

## 결론

- Core 버그를 재현하고 수정했다. DEALER application socket을 별도 poller에 등록만 했거나 등록하지 않은 상태에서도 client monitor만 소비하면 `DISCONNECTED`와 자동 reconnect `CONNECTION_READY`가 진행된다.
- tcp/inproc, ROUTER close/서버 `zlink_disconnect(endpoint)`, poller 등록/미등록의 8개 조합을 각 20회 측정했다. 모든 `DISCONNECTED` p95는 11 ms 이하, reconnect READY p95는 93 ms 이하로 회귀 기준(`DISCONNECTED <= 200 ms`, READY `RECONNECT_IVL(50)+200 ms` 이내)을 통과했다.
- client의 즉시 `disconnect -> connect`에서는 이전 `connection_id`의 `DISCONNECTED`와 다른 새 `connection_id`의 READY를 모두 확인했다. HANDOVER 정책은 tcp/inproc 모두 새 connection에서 reply가 완료됐다.

## 시나리오 (a) 결과 (각 20회, ms)

| transport | 서버 단절 | client public poller | DISCONNECTED min/p50/p95/max | reconnect READY min/p50/p95/max |
|---|---|---|---:|---:|
| tcp | close | 등록만, wait 없음 | 9/10/11/11 | 60/90/92/100 |
| tcp | close | 미등록 | 9/10/11/11 | 81/91/92/92 |
| inproc | close | 등록만, wait 없음 | 9/10/10/10 | 9/10/10/10 |
| inproc | close | 미등록 | 9/10/10/10 | 9/10/10/10 |
| tcp | server `zlink_disconnect` | 등록만, wait 없음 | 9/10/10/10 | 51/91/92/100 |
| tcp | server `zlink_disconnect` | 미등록 | 9/10/10/11 | 71/91/92/92 |
| inproc | server `zlink_disconnect` | 등록만, wait 없음 | 9/9/10/10 | 10/10/10/10 |
| inproc | server `zlink_disconnect` | 미등록 | 9/10/10/10 | 10/10/10/10 |

## 시나리오 (b)/(c) 결과

아래는 최종 대표 실행이다. `first_result=1`은 BACKPRESSURED 후 WRITABLE을 받아 재제출한 경우다.

| transport | duplicate policy | old -> new connection_id | 최초 REQUEST | reply | 귀속 |
|---|---|---:|---|---|---|
| tcp | default REJECT | 1041 -> 1047 | BACKPRESSURED | timeout | 직접 귀속 불가 |
| inproc | default REJECT | 1056 -> 1059 | OK | timeout | 직접 귀속 불가 |
| tcp | HANDOVER | 1068 -> 1074 | BACKPRESSURED 후 재제출 | OK | old terminal 뒤 유일한 live replacement 1074로 추론 |
| inproc | HANDOVER | 1083 -> 1086 | OK | OK | old terminal 뒤 유일한 live replacement 1086으로 추론 |

- 반복 관찰에서 default REJECT의 inproc reply는 timing에 따라 OK 또는 timeout이 모두 나왔다. HANDOVER는 tcp/inproc 모두 reply OK로 고정됐다.
- 공개 `zlink_reply_token_t`는 opaque이고 completion에는 physical `connection_id`가 없다. 따라서 테스트는 이전 connection의 terminal edge를 먼저 확인한 뒤 reply가 완료되면 유일하게 live인 replacement connection으로 귀속을 추론하며, timeout은 귀속하지 않는다.

## spec 대조

- `core/doc/spec/core/05-polling.ko.md:72-81`: readiness는 level-triggered이고 내부 I/O/async/임시 owner가 transition을 처리해도 동일하게 wake해야 하며 lost wake는 계약 위반이다.
- `core/doc/spec/core/socket/README.ko.md:825-827`: peer가 사용 불가능해지면 library가 자동 reconnect한다.
- `core/doc/spec/core/socket/README.ko.md:851-861`: `zlink_disconnect`는 이전 연결을 제거한다.
- `core/doc/spec/core/socket/README.ko.md:145-165`: REJECT는 중복 시 기존 pipe 유지/새 pipe 거절, HANDOVER는 새 reconnect pipe가 기존 pipe를 인수한다. 물러난 방향에 admit된 request는 이전되지 않고 timeout되며 caller가 다시 보낸다.
- `core/doc/spec/core/socket/README.ko.md:940-980`, `1040-1062`: DONTWAIT는 admission 1회, 준비 전이면 payload-free WRITABLE token, admit 뒤 disconnect에도 payload replay 없음과 정확히 한 번의 timeout/reply 종결을 규정한다.
- `core/doc/spec/core/06-monitoring.ko.md:69-77`, `517-533`: `connection_id`는 물리 시도의 진단/correlation 값일 뿐 send target이 아니며 READY edge는 logical peer별 정확히 한 번이다. 동일 monitor 순서는 Core commit 순서다.

## 원인 (`file:line`)과 수정

1. `core/src/runtime/sockets/common/socket_base_api.cpp:1713-1718`: inproc endpoint metadata의 오래된 connection id를 DISCONNECTED에 복사해 READY와 terminal id가 달랐다. 종료 시 live transport connection id로 정규화했다.
2. `core/src/runtime/sockets/common/socket_base_api.cpp:1842-1848,1902-1907` 및 `socket_endpoint_runtime.cpp:116-126`: inproc peer close 시 pipe map을 삭제하면서 connect intent도 소실됐다. 명시 disconnect/teardown이 아닌 connector application pipe termination은 기존 endpoint intent를 다시 pending connect로 materialize한다.
3. `core/src/runtime/sockets/common/socket_base_endpoint.cpp:422-433,943-972`: 1-lane DEALER-ROUTER inproc `send_bind`/terminal command가 idle peer mailbox에 남았다. peer command executor를 해당 attach/explicit disconnect에만 일시적으로 설치하고 quiesce한다. 영구 owner는 사용하지 않아 completion/public owner를 침범하지 않는다.
4. `core/src/runtime/sockets/common/socket_base_endpoint.cpp:1034-1069`: bound inproc endpoint의 server `zlink_disconnect`가 registry만 제거하고 attached pipe를 종료하지 않았으며, 양방향 ack도 후속 application API에 의존했다. matching pipe를 no-delay 종료하고 lifetime pin 아래 최대 200 ms 동안 현재 public command owner가 ack를 drain한다.

## 변경 파일

- `core/src/runtime/sockets/common/socket_base.hpp`
- `core/src/runtime/sockets/common/socket_base_api.cpp`
- `core/src/runtime/sockets/common/socket_base_endpoint.cpp`
- `core/src/runtime/sockets/common/socket_endpoint_runtime.cpp`
- `core/src/runtime/sockets/common/socket_runtime.hpp`
- `core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp`
- `core/tests/CMakeLists.txt`

## 회귀 테스트

- 신규 `test_socket_disconnect_progress_without_app_poll` (12 Unity cases):
  - `test_{tcp,inproc}_{registered_without_waiter,without_registration}_progresses`
  - `test_{tcp,inproc}_{registered,unregistered}_server_disconnect_progresses`
  - `test_{tcp,inproc}_immediate_disconnect_connect_default_policy_observation`
  - `test_{tcp,inproc}_immediate_disconnect_connect_handover_uses_replacement`
- CMake: labels `integration;serial`, TIMEOUT 60.
- 기존 owner 회귀: `test_zmp_request_reply_receive_transaction`, `test_flow_state_paired`, `test_completion_pipe_budget_is_fair_and_stale_requeue_is_fenced` green.

## 게이트

| gate | 결과 |
|---|---|
| 격리 build `core/build-task4`, RelWithDebInfo/LTO OFF/tests ON, `-j3` | 성공 |
| 신규 테스트 반복 | 5/5 green, 51.06 s |
| final integration `ctest -L integration -j2` | 92/92 green, 166.49 s |
| final full `ctest -j2` 기능 테스트 | 142/142 green |
| `hotpath_gate` | 별도 감독자 gate 실패: 4 cells ratio 1.2551-1.3168 (요청상 감독자 별도 판정) |
| `git diff --check` | green |

## BLOCKERS

1. **spec gap — 사용자 결정 필요:** terminal edge를 소비하기 전 같은 socket에서 즉시 `disconnect(endpoint) -> connect(endpoint)` 했을 때, 새 connect가 이전 physical terminal을 반드시 기다리는지 또는 old/new overlap을 허용하는지 명시가 없다. 선택지: (A) disconnect 반환 전에 logical/physical terminal을 직렬화해 다음 connect를 항상 새 pipe로 admit, (B) overlap을 허용하고 default REJECT/HANDOVER 정책에 따라 request 결과가 달라질 수 있음을 계약화. 현재 Core는 transport timing에 따른 overlap을 허용하며 HANDOVER만 결정적으로 새 reply를 완료한다.
2. **공개 관찰 한계:** request/reply가 어느 physical `connection_id`를 사용했는지 직접 반환하는 공개 C API가 없다(`connection_id`는 send target으로 사용 금지, reply token은 opaque). 직접 귀속이 필수라면 monitor/request completion 계약 확장 여부를 별도 결정해야 한다. 현재 테스트는 old terminal 이후 유일한 live replacement로만 추론한다.
3. `hotpath_gate`는 기능 gate와 별도인 감독자 판정 항목이며 이 실행에서는 네 셀 모두 threshold를 초과했다. 기능 테스트 실패는 없다.

Core 버그 → 수정(core/src/runtime/sockets/common/socket_base_api.cpp, core/src/runtime/sockets/common/socket_base_endpoint.cpp, core/src/runtime/sockets/common/socket_endpoint_runtime.cpp)
