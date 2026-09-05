# Java async submit typed-result 계약 점검 결과

## 결론

Java public API의 기존 `ZlinkSubmitException#getResult()`와 `SubmitResult`만으로 route 없음, admission 거절, HWM 대기, 명시적 target 제거를 구분할 수 있다. Caller가 raw errno 숫자로 분기할 필요는 없다. 새 public 타입은 추가하지 않았다.

TCP에서 HWM 대기 중인 exact route를 `disconnectRid()`로 제거하면 간헐적으로 `NOT_FOUND`가 아니라 `NOT_CONNECTED`가 되던 binding race를 수정했다. `disconnectRid()`가 Java completion owner와 같은 send/drain 경계에서 실행되고, 호출 당시의 matching pending state에 명시적 제거 사실을 기록한다. 이미 queue에 있던 WRITABLE wake를 나중에 처리하더라도 재제출 결과를 새 route 없음으로 잘못 분류하지 않고 `NOT_FOUND`로 끝낸다. Native completion은 끝까지 drain한다.

## 4 case 결과

| case | inproc | TCP | Java에서 관찰한 타입 | raw errno 의존 |
|---|---|---|---|---|
| exact-route loss: MANDATORY ROUTER가 없는 RID로 async send | PASS | PASS | `ZlinkSubmitException`, `SubmitResult.NOT_CONNECTED` | 없음 |
| admission 거절: ROUTER가 식별된 DEALER RID로 typed request | PASS | PASS | `ZlinkSubmitException`, `SubmitResult.NOT_ADMITTED` | 없음 |
| capacity: ROUTER→DEALER HWM 도달 | PASS | PASS | `CompletionStage<Void>`가 미완료로 남고 binding이 `BACKPRESSURED` wait token을 소유; peer drain 뒤 WRITABLE을 받아 정확히 한 번 재제출하고 정상 완료 | 없음 |
| wait-token terminal: HWM token의 exact RID를 `disconnectRid()`로 명시적 제거 | PASS | PASS | stage가 `ZlinkSubmitException`, `SubmitResult.NOT_FOUND`로 실패 | 없음 |

Socket close 대안도 기존 `socketCloseTerminatesBackpressuredSendWithTypedFailure`에서 `ZlinkSubmitException`, `SubmitResult.TERMINATED`만으로 확인한다.

## Spec 근거

- `bindings/doc/spec/README.ko.md:3974-3977`: socket/connection/transport/protocol 오류는 Core가 결정하고 binding은 caller에 전달한다.
- `bindings/doc/spec/README.ko.md:4210-4232`: public result enum을 언어별 typed 값으로 매핑하며 `internalErrno`는 coarse bucket의 진단 세부다.
- `bindings/doc/spec/java/README.ko.md:835-845`: Java typed exception은 public result를 보존하고 native errno는 주된 public contract가 아니다.
- `bindings/doc/spec/java/README.ko.md:987-994`: Java runtime은 native completion을 `CompletionStage`로 바꾸고 late completion도 정리한다.
- `core/doc/spec/core/03-errors.ko.md:34-38`, `334-352`: caller는 typed result enum으로 분기하고 errno는 log/진단에 사용한다. Submit 결과는 `BACKPRESSURED`, `NOT_CONNECTED`, `NOT_FOUND`, `NOT_ADMITTED`를 서로 다른 제어 흐름으로 정의한다.
- `core/doc/spec/core/socket/README.ko.md:962-970`: HWM/credit 회복은 matching WRITABLE record를 만들고 caller/binding이 같은 record를 다시 제출한다.
- `core/doc/spec/core/socket/README.ko.md:982-989`: 명시적 target 제거는 TERMINAL+ENOENT, close는 lifecycle terminal이며, 아직 반환하지 않은 wait도 각각 `NOT_FOUND`와 `TERMINATED`로 끝난다.
- `core/doc/spec/core/socket/README.ko.md:1018-1022`: ROUTER→DEALER typed request는 `NOT_ADMITTED`; 없는 RID의 DONTWAIT submit은 `NOT_CONNECTED`이고 token이 없다.
- `core/doc/spec/core/socket/README.ko.md:1305-1321`: DONTWAIT capacity는 wait token을 만들며 route 없음, 명시적 제거, close의 결과를 구분한다.

## 원인

- `bindings/java/src/main/java/systems/zlink/runtime/sockets/SocketCore.java:106`: 기존 `disconnectRid()` native call은 `CompletionOwner`의 send/drain 직렬화 경계 밖에서 실행됐다.
- `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:920-948`: Core가 이미 queue에 넣은 WRITABLE wake와 `disconnectRid()`가 교차하면 Java가 wake를 받은 뒤 제거된 RID로 다시 제출할 수 있었다. 이 retry는 typed `NOT_CONNECTED`가 되어 명시적 제거의 `NOT_FOUND` 의미를 잃었다.
- `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:1431-1462`: WRITABLE terminal 처리에는 호출자가 명시적으로 target을 제거했다는 Java-side ordering 정보가 없었다.

## 변경

- `bindings/java/src/main/java/systems/zlink/runtime/sockets/SocketCore.java`: `disconnectRid()`를 기존 completion owner send/drain lock으로 직렬화하고 성공 뒤 matching pending state를 표시한다.
- `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java`: pending state에 target 제거 표식을 보존한다. Stale WRITABLE 뒤 retry 또는 TERMINAL completion 모두 send는 `SubmitResult.NOT_FOUND`, request는 `RequestResult.NOT_FOUND`로 끝내며 native completion은 계속 drain한다.
- `bindings/java/src/test/java/systems/zlink/integration/contract/AsyncSubmitTypedResultContractTest.java`: 4 case를 inproc/TCP에서 public Java API만으로 검증하는 동적 contract test 8개를 추가했다.
- `bindings/java/src/test/java/systems/zlink/integration/contract/DontWaitBackpressureContractTest.java`: wait-token removal/close 검증에서 raw errno assertion을 제거하고 typed result만 계약으로 남겼다.
- 기존 작업 5 변경인 `NativeLayouts.java`, `NativeLayoutsTest.java`, `MonitorConnectionIdentityContractTest.java`는 수정하지 않았다.
- spec과 `framework/**`는 수정하지 않았다.

## 테스트와 게이트

- 신규 테스트 이름:
  - `exactRouteLossIsNotConnected` — inproc/TCP
  - `peerTypeAdmissionRefusalIsNotAdmitted` — inproc/TCP
  - `capacityWaitCompletesAfterWritable` — inproc/TCP
  - `removedWaitTargetIsNotFound` — inproc/TCP
- 최종 코드에서 신규 class 5회 반복: 회당 8 cases, 총 40/40 PASS.
- `bindings/java/tests/run_tests.sh`: PASS.
  - `:test`: 97 tests, failures 0, errors 0, skipped 3.
  - `:integrationTest`: 25 tests, failures 0, errors 0, skipped 0.
  - `:zlink-ext-netty:test`: 3 tests, failures 0, errors 0, skipped 0.
  - `:kotlin-contract-test:test`: 4 tests, failures 0, errors 0, skipped 0.
  - JUnit 합계: 129 tests, failures 0, errors 0, skipped 3.
  - sample smoke runner: 7/7 PASS (`runRequestReplyAsync`, `runPairRecv`, `runPubSubRecv`, `runDealerRouterRecv`, `runStreamRecv`, `runStreamPacketCallback`, `runMonitorRecv`).
- `git diff --check`: PASS.

## BLOCKERS

없음. 필요한 public typed result와 exception은 이미 spec과 Java API에 있었으므로 spec gap이나 사용자 결정 사항이 없다.

A가 쓸 한 줄: Java async submit은 errno 없이 `NOT_CONNECTED`·`NOT_ADMITTED`·HWM WRITABLE 대기·명시적 제거 `NOT_FOUND`를 구분하며, TCP disconnectRid/completion race 수정과 inproc/TCP 40회 반복 및 전체 Java gate를 통과했다.
