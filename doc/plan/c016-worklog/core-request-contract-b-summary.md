# Core REQUEST contract B 결과

## 결과

D-B85에 따라 `zlink_request_part(..., DONTWAIT, FINAL, ...)`을 SEND 계약 B와 같은 one-shot admission + payload-free WRITABLE retry 계약으로 변경했다. REQUEST 전용 pre-admission payload pending pool과 redrive 경로를 제거했고, blocking REQUEST, REPLY submit, PUB publish 및 공개 ABI는 변경하지 않았다.

## 변경 파일

Core public header:

- `core/include/zlink/socket/api.h`
- `core/include/zlink_enum.h`

Raw header mirror(byte-identical):

- `bindings/c/include/zlink/socket/api.h`
- `bindings/c/include/zlink_enum.h`
- `bindings/cpp/include/zlink/socket/api.h`
- `bindings/cpp/include/zlink_enum.h`
- `bindings/go/include/zlink/socket/api.h`
- `bindings/go/include/zlink_enum.h`
- `bindings/rust/include/zlink/socket/api.h`
- `bindings/rust/include/zlink_enum.h`

Core implementation:

- `core/src/api/socket/part_helper_api.cpp`
- `core/src/api/socket/socket_message_handler_api.cpp`
- `core/src/api/socket/socket_request_reply_submit_api.cpp`
- `core/src/runtime/sockets/common/socket_base.hpp`
- `core/src/runtime/sockets/common/socket_base_api.cpp`
- `core/src/runtime/sockets/common/socket_base_endpoint.cpp`
- `core/src/runtime/sockets/common/socket_base_lifecycle.cpp`
- `core/src/runtime/sockets/common/socket_runtime.hpp`
- `core/src/runtime/sockets/common/socket_send_complete.cpp`
- `core/src/runtime/sockets/common/socket_send_pending_submit.cpp`
- `core/src/runtime/sockets/dealer/dealer.cpp`
- `core/src/runtime/sockets/router/router.hpp`
- `core/src/runtime/sockets/router/router_admission.cpp`
- `core/src/runtime/sockets/stream/stream.cpp`

Core tests:

- `core/tests/CMakeLists.txt`
- `core/tests/integration/test_request_writable_contract.cpp` (new)
- `core/tests/integration/test_dealer_router_single_lane_contract.cpp`
- `core/tests/integration/test_helper_ownership.cpp`
- `core/tests/integration/test_phase3_request_reply_contract.cpp`
- `core/tests/unittest/unittest_socket_runtime.cpp`

생성된 `core/build/`은 release lib-only 빌드 산출물이며 gitignore 대상이다. 기존 사용자 소유 untracked `core/build-main-readonly`는 변경하지 않았다. 문서, 그 밖의 binding source와 framework source는 수정하지 않았다.

## 계약 표

| 조건 | submit 결과 | 반환 ID | 후속 결과/소유권 |
|---|---|---|---|
| DONTWAIT FINAL 즉시 admission | `ZLINK_SUBMIT_OK`, errno 0 | nonzero REQUEST ID | 기존 REQUEST reply/error/timeout completion. Reply timeout은 admission 시점부터 시작 |
| HWM/byte credit/flow pause 또는 존재하지만 미준비인 target | `ZLINK_SUBMIT_BACKPRESSURED`, `EAGAIN` | nonzero WRITABLE wait token | Core는 token/target/context만 보관. payload는 보관하지 않으며 WRITABLE `ADMITTED` 뒤 caller가 같은 request를 재제출 |
| ROUTER mandatory route 없음 | `ZLINK_SUBMIT_NOT_CONNECTED`, `EHOSTUNREACH` | 0 | completion 없음 |
| 대기 중 target 명시적 제거 | 기존 submit 결과 유지 | 같은 wait token | WRITABLE `TERMINAL`, `ENOENT`, 같은 context/RID |
| socket close/context term | 기존 SEND writable-wait lifecycle과 동일 | 같은 wait token | terminal retirement 후 close/term lifecycle에서 queue 정리 |
| `ZLINK_OPT_PENDING_MAX_MSGS/BYTES` | set/get ABI 유지 | 해당 없음 | 저장·조회만 하며 admission/payload/credit에 영향 없는 no-op |
| blocking `NONE` REQUEST, REPLY, PUB | 변경 없음 | 기존 계약 | 기존 synchronous admission/전달 경로 유지 |

REQUEST 전용 `request_pending_submit`, `drive_request_pending`, `try_admit_request_pending`, `finish_request_pending`, `fail_pull_request_pending_for_logical_*` 및 REQUEST queue/FIFO/reservation/charge/redrive 상태를 제거했다. `fail_all_send_pending`은 blocking SEND logical waiter만 처리한다. completion recv, poll/get-events, mailbox와 pipe/weight/attach wake 지점의 REQUEST redrive 호출도 제거했다.

## 테스트

- `JOBS=4 bash scripts/build-core.sh dev`: PASS.
- `ctest --test-dir core/build-dev --output-on-failure -E '^hotpath_gate$' -j2`: PASS, 140/140, 184.57 s.
- 변경 suite 5회 반복: 매 실행 5/5 PASS.
  - `test_request_writable_contract`
  - `test_phase3_request_reply_contract`
  - `test_zmp_request_reply`
  - `test_single_lane_flow_normal_kind_gate`
  - `test_helper_ownership`
- 새 public API suite: DEALER-ROUTER HWM, ROUTER-ROUTER RID, connect-before-bind, mixed SEND/REQUEST token, admission 전 timeout 미기동, admission 뒤 timeout, target removal terminal, close cleanup, missing mandatory route, PENDING_MAX no-op를 검증. sleep/usleep/nanosleep 사용 없음.
- 8개 Core raw header × c/cpp/go/rust: `cmp` 32/32 PASS.
- `git diff --check`: PASS.
- tracked 변경의 허용 범위 검사: PASS.
- 제거 대상 심볼 검색: `request_pending_submit|drive_request_pending|try_admit_request_pending|finish_request_pending|fail_pull_request_pending_for_logical|send_pending_record_t|send_pending_request_` 잔존 없음.

## Gate

- 마지막 빌드: `JOBS=4 bash scripts/build-core.sh release --lib-only`: PASS (`Release`, LTO ON, `core/build/lib/libzlink.so`).
- hotpath gate: SKIP. `core/build-release-gate` tree가 없고 release lib-only tree에는 `core/build/bin/hotpath_bench`가 생성되지 않아 지정 command를 실행할 수 없다.

## 측정

감독관 지시에 따라 성능 측정은 실행하지 않았다. 기존 runner는 REQUEST 계약 B를 아직 반영하지 않았으므로 runner 수정 후 감독관이 측정해야 한다. 위 hotpath gate 실행 가능 여부 확인은 성능 측정으로 간주하지 않는다.

## SPEC

보호된 문서는 수정하지 않았다. 다음 문장은 현재 구현과 불일치하므로 별도 승인 후 갱신을 제안한다.

- `core/doc/spec/core/socket/README.ko.md:277`, `:299-303`, `:957-973`
  - 제안: “`ZLINK_COMPLETION_WRITABLE`은 DONTWAIT SEND와 admission 전 DONTWAIT REQUEST가 반환한 payload-free 대기 토큰을 완료한다. REQUEST가 즉시 admission되면 nonzero REQUEST ID를, backpressure이면 nonzero WRITABLE token을 반환하며, WRITABLE `ADMITTED` 뒤 caller가 payload를 다시 제출한다.”
- `core/doc/spec/core/socket/README.ko.md:379-394`, `:1254-1256`
  - 제안: “`ZLINK_OPT_PENDING_MAX_MSGS/BYTES`는 ABI 호환을 위해 set/get 값을 저장하지만 Core 동작에서는 완전히 무시하며, REQUEST pending pool이나 payload reservation을 만들지 않는다.”
- `core/doc/spec/core/socket/README.ko.md:1028-1045`, `:1314-1316`, `:1322`
  - 제안: “DONTWAIT REQUEST FINAL은 admission을 한 번만 시도한다. 성공 시 nonzero REQUEST ID와 admission 시점부터의 reply timeout을 만들고, physical backpressure 또는 known-but-unready target이면 `BACKPRESSURED`+`EAGAIN`과 nonzero WRITABLE token을 반환한다. Mandatory ROUTER route miss는 `NOT_CONNECTED`+`EHOSTUNREACH`, ID 0, completion 없음이다.”
- `core/doc/spec/core/socket/README.ko.md:1111-1120`, `:1348`
  - 제안: “WRITABLE 표의 적용 대상을 SEND/REQUEST wait token으로 확장하고, transient disconnect 때 Core의 REQUEST pre-admission retry가 아니라 같은 target의 WRITABLE 뒤 caller 재제출로 기술한다. 명시적 target 제거는 같은 token/context/RID의 `TERMINAL`+`ENOENT`다.”

## Binding/framework 영향 목록

요청한 `rg -n 'request_part|RequestOperation|REQUEST|PENDING_MAX' bindings/*/src framework/languages`를 실행했다. 일반 REQUEST 상수·테스트·sample hit를 제외한 계약 변경 영향 지점은 다음과 같다. 이 범위는 수정 금지이므로 조사만 했다.

Binding REQUEST retry 필요:

- C++: `bindings/cpp/src/Runtime/Messaging/request_reply.cpp:23-79`, `:153-177` — async DONTWAIT가 BACKPRESSURED token을 request failure로 던진다. SEND writable-owner와 같은 payload snapshot/wait/retry 상태가 필요하다.
- .NET: `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:149-174`, `:461-486` — async REQUEST가 BACKPRESSURED를 예외로 처리한다. 같은 파일의 SEND retry state를 REQUEST에도 적용해야 한다.
- Java: `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:140-160`, `:978-993` — `requireRequestSuccess`가 nonzero writable token도 거부한다. `isWritableWait` 기반 request payload 보관/재제출이 필요하다.
- Node: `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:298-324`, `:470-518` — request submit failure 처리와 completion capture가 WRITABLE request token을 지원하지 않는다. SEND retry와 분리된 REQUEST retry/correlation 상태가 필요하다.
- Python: `bindings/python/src/zlink/_runtime/messaging/routed_async.py:861-910`, `:1001-1022` — async REQUEST가 BACKPRESSURED를 즉시 unregister/error 처리한다. token 대기 후 payload 재물질화·재제출이 필요하다.
- Rust: `bindings/rust/src/runtime/messaging/operations/routed_async.rs:93-130`, `:183-215` — future가 `check_submit_rc`의 BACKPRESSURED를 terminal error로 처리하고 operation을 버린다. WRITABLE 동안 operation을 유지해 재제출해야 한다.
- Go: `bindings/go/src`에는 해당 검색식의 request wrapper 구현 hit가 없었다. Raw header mirror만 이번 변경에 포함했다.

Binding PENDING_MAX 설명 no-op 정렬 필요:

- `bindings/rust/src/runtime/native/ffi.rs:287-290`
- `bindings/java/src/main/java/systems/zlink/internal/sockets/SocketOption.java:32-35`
- `bindings/java/src/main/java/systems/zlink/internal/sockets/SocketOptions.java:108-115`
- `bindings/node/src/zlink/runtime/options/option_mapping.ts:26-27`와 Python enum 값은 ABI 값은 그대로이며, 문서/주석이 있는 곳은 “stored and ignored”로 정렬해야 한다.

Framework 전파 지점(직접 Core 호출이 아니라 위 binding RequestOperation을 소비):

- C++: `framework/languages/cpp/framework/src/runtime/channels/route_channel_runtime.cpp:222`, `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:207`
- .NET: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:268`, `:1121`, `:1477`; `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkRawRouterServicePort.cs:152`
- Java: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java:169`; `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:3100`, `:4143`
- Node: `framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-binding-port.ts:391`; `framework/languages/node/packages/framework/src/runtime/channels/channel-outbound-operations.ts:222`, `:502`; `framework/languages/node/packages/framework/src/runtime/channels/channel-multipart.ts:39`

Framework는 binding이 WRITABLE을 내부 admission control로 처리하면 대체로 기존 RequestOperation await 경로를 유지할 수 있다. 각 binding 정렬 전에는 연결 직후/HWM 상황에서 비동기 REQUEST가 이전처럼 Core 내부에서 대기하지 않고 즉시 backpressure failure로 노출될 수 있다.

## BLOCKERS

- Hotpath gate 실행 blocker: release-gate test tree와 `hotpath_bench`가 없음. release lib-only는 정상 통과했다.
- 상위 언어 비동기 REQUEST의 자동 재시도 parity는 이번 수정 금지 범위 밖이다. 위 binding 지점을 정렬하기 전 framework의 backpressure 동작은 Core 계약 B를 완전히 소비하지 못한다.
- Core 구현/테스트의 추가 blocker는 없다.
