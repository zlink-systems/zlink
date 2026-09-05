# Node `RawStreamSessionService.deliver()` catch 경계 진단

## 1. 결론

`RawStreamSessionService.deliver()`의 현재 구현은 STREAM 전송의 모든 예외를 세션 종료로
해석한다. `submit_sync()`에서 발생한 backpressure뿐 아니라 payload decode 오류, builder 사용
오류, 프로그래밍 오류도 같은 `catch`에 들어가며, 이때 `sessionTargets`를 삭제한다
(`framework/languages/node/packages/framework/src/runtime/backend/node/node-raw-mesh-backend.ts:1993-2011`).
그 결과 일시적인 전송 실패 한 번이 영구적인 local session target 소실로 바뀐다.

이 결함의 분류는 **B — 기존 Framework 구현 결함**이다. 이미 Core, Node binding, Framework
공통 spec에 비동기 submit과 backpressure 소유권이 정의되어 있으므로 새 계약은 필요하지 않다.
pull-completion 전환 자체는 계약 적응이었지만, 그 과정에서 이 경로가 `submit_sync()`를 선택하고
기존 catch-all을 유지한 것은 현재 계약에 맞지 않는 구현 결함이다.

최소 수정 방향은 다음과 같다. 이 문서는 STAGE 1 진단이므로 구현하지 않는다.

1. 실제 bound-session push를 Node binding의 비동기 terminal인 `submit()`으로 보내고 await한다.
2. binding이 소유하는 exact `WRITABLE(token, context, RID)` 재제출을 그대로 사용한다.
3. catch는 typed terminal result만 판별한다. closed/not-connected terminal은 기존 STREAM
   close/unbind lifecycle로 session close를 요청하고, backpressure/timeout은 target을 지우지 않은
   typed failure로 전달한다. 알려지지 않은 오류는 그대로 다시 던진다.
4. decode와 message-builder 오류는 transport catch 밖에 둔다.
5. Framework에 두 번째 poller, 별도 retry queue, timeout 증가, 문자열 기반 오류 분류를 추가하지
   않는다.

분류 판정은 다음과 같다.

| 분류 | 판정 | 이유 |
|---|---|---|
| A — 계약 적응 | 아니오 | async terminal과 typed failure 계약이 이미 구현 전에 존재한다. |
| B — 기존 결함 | **예** | Node Framework가 기존 계약과 세 언어 구현과 달리 sync terminal과 catch-all을 사용한다. |
| C — 우회 | 아니오 | 제안은 timeout/retry/별도 상태를 추가하지 않고 소유 binding과 lifecycle 경로를 복구한다. |
| D — spec gap | 아니오 | Core retry, Framework async terminal, session liveness 소유권이 이미 명시되어 있다. |

## 2. 재현과 직접 원인

후속 M6B contract test는 다음 순서를 실행한다
(`framework/languages/node/test/m6b/m6b-runtime.contract.ts:5394-5575`).

1. 최초 push는 성공한다.
2. fake STREAM을 backpressured 상태로 만들고 push한다.
3. backpressure를 해제한 뒤 같은 bound session에 다시 push한다.

관찰 결과는 3회 모두 동일했다.

- 1차: `20.852 ms`
- 2차: `21.779 ms`
- 3차: `22.322 ms`
- 각 실행에서 첫 push와 backpressure 시점의 기대는 통과했지만, 해제 후 push가
  `SubmitResult.Ok` 대신 `SubmitResult.InvalidState`가 되어 실패했다.
- M6B 전체 결과는 `111 passed / 1 failed`였다.

현재 type-exact fake의 두 terminal은 같은 `submitPending()`을 사용한다
(`framework/languages/node/test/m6b/m6b-runtime.contract.ts:6630-6658`). Backpressure이면
`Error("stream route is backpressured")`를 던지고, disconnected이면
`Error("stream route is disconnected")`를 던진다(`:6637-6638`). 이 fake 오류에는
`SubmitError.result`, `code`, `nativeErrno`가 없다.

실패 흐름은 다음과 같다.

```text
bound-session push
  -> RawStreamSessionService.deliver()
  -> submit_sync()
  -> backpressure 예외
  -> catch-all
  -> sessionTargets.delete(sessionRid)
  -> 다음 push의 target lookup 실패
  -> false -> protocolError -> InvalidState
```

이전 진단 trace인 `/dev/shm/zlink-tmp-node/diag-m6b-invalidstate/`도 먼저 확인했다. 그 trace는
fake가 아직 `submit_sync()`를 제공하지 않던 시점의
`TypeError: submit.submit_sync is not a function`을 기록한다. 현재 backpressure 예외 자체의
trace는 아니지만, catch-all이 프로그래밍 오류까지 session close로 바꾼다는 독립적인 증거다.
현재 현상은 위의 수정된 fake 소스와 3회의 동일한 contract 결과로 확인했다.

실제 Node binding에서는 동기 terminal의 backpressure가 문자열 `Error`가 아니라
`SubmitError(result = SubmitResult.Backpressured, nativeErrno = EAGAIN)`이다.
`sendSync()`가 Core NONE 결과의 모든 non-OK를 typed `SubmitError`로 던지기 때문이다
(`bindings/node/src/zlink/runtime/messaging/completion_owner.ts:408-431`,
`bindings/node/src/zlink/contracts/errors/errors.ts:24-52`). 따라서 fake의 오류 모양과 실제 binding의
오류 모양은 다르지만, 현재 argumentless catch가 둘을 똑같이 삭제한다는 원인은 같다.

## 3. 소유 계층과 spec 근거

### 3.1 Core와 binding의 소유권

Core STREAM은 전송 admission과 route별 backpressure를 소유한다.

- Core NONE은 `SNDTIMEO`까지 기다린 뒤 timeout이면 `BACKPRESSURED/EAGAIN`을 반환한다.
- Core DONTWAIT는 한 번 시도하고 backpressure이면 nonzero retry token을 반환한다.
- binding은 그 token의 정확한 `WRITABLE(token, context, RID)` completion 뒤에 동일 payload를
  재제출한다.
- 일시적 physical disconnect는 token을 종료하지 않는다. 명시적 target 제거 또는
  socket/context close만 terminal이다.

근거는 `core/doc/spec/core/socket/08-stream.ko.md:99-142`,
`core/doc/spec/core/socket/README.ko.md:930-992,1127-1136`,
`core/doc/spec/core/05-polling.ko.md:43-101,298-308`이다.

Node binding도 이 계약을 그대로 구현한다.

- `submit_sync()`는 Core NONE이고, `submit()`은 Core DONTWAIT completion이다
  (`bindings/doc/spec/node/README.ko.md:527-536,775-829`).
- STREAM send builder는 async terminal을 `CompletionOwner.submitSend`, sync terminal을
  `sendSync`에 연결한다
  (`bindings/node/src/zlink/runtime/sockets/stream_socket.ts:37-42`,
  `bindings/node/src/zlink/runtime/sockets/socket_operation_builders.ts:51-65`).
- `submitSend()`는 DONTWAIT를 사용하고 최초 backpressure 뒤 payload를 snapshot하며 exact
  WRITABLE을 기다린다
  (`bindings/node/src/zlink/runtime/messaging/completion_owner.ts:207-286,547-640`).
- close는 pending submit을 typed `SubmitResult.Terminated`로 끝낸다
  (`bindings/node/src/zlink/runtime/messaging/completion_owner.ts:469-483`).

그러므로 Framework가 `submit_sync()` 예외를 보고 직접 session 상태를 결정하거나 자체 retry를
만드는 것은 binding 소유 결정을 중복 구현하는 것이다.

### 3.2 Framework의 소유권

Framework 공통 submit 계약은 bound-session send를 비동기 terminator로 정의하고, HWM 재제출과
completion을 Core/binding 소유로 둔다
(`framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md:100-179`).
특히 Framework 내부 HWM 경로는 각 언어의 async terminal을 사용해야 하고, public sync 계약이나
즉시 backpressure 관찰이 아닌 내부 전송에서 sync terminal을 쓰지 않는다(`:434-459`). 실패 분류는
문자열이 아니라 typed category를 사용한다(`:421-432`).

bound-session command 36의 session owner는 fence를 검증한 뒤 실제 STREAM 전송을 수행하지만,
client delivery ack나 Framework retry는 만들지 않는다
(`framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:186-218`).
실제 socket I/O와 connect/disconnect event는 Core가 소유하며, Framework는 monitor event를
관찰해 session lifecycle을 수렴시킨다
(`framework/doc/framework/common/spec/server/02-channel-transport/README.ko.md:26-34`,
`framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md:226-248`).

Node에는 이미 이 lifecycle 경로가 있다.

- STREAM 생성 시 같은 socket의 session service와 기존 monitor를 함께 구성한다
  (`framework/languages/node/packages/framework/src/runtime/streams/index.ts:240-259`).
- `StreamSessionRuntime`이 monitor를 구독하고, `Disconnected` event에서 session을 찾아 disconnect를
  enqueue한다
  (`framework/languages/node/packages/framework/src/runtime/streams/stream-session-runtime.ts:916-927,1125-1177`).
- completion/transport-close 처리는 같은 runtime에서 수행된다(`:746-779`).

따라서 임의 전송 예외에서 `sessionTargets`를 삭제하는 것은 monitor/lifecycle 소유 판단을
중복한다. closed/not-connected typed terminal이 발생하면 해당 delivery는 closed/unavailable로
분류하고 기존 close/unbind entry point로 넘겨야 한다. target 폐기와 session callback은
documented typed terminal 또는 monitor event라는 lower-layer 사실을 근거로 한 번만 수렴해야 한다.

## 4. 현재 Node 호출 경계

`RawStreamSessionService.bindActor()`는 `sessionTargets`에 target을 넣고 synchronous boolean
delivery closure를 `ServiceStatefulRuntime.bindSession()`에 등록한다
(`node-raw-mesh-backend.ts:1930-1955`). `deliver()`의 try 블록에는 아래 네 종류의 작업이 함께 있다.

1. STREAM send operation 생성
2. application payload와 multipart decode
3. message builder 조립
4. `submit_sync()` 실행

이 전체를 덮는 `catch { ...delete...; return false; }` 때문에 transport terminal과 data/programming
오류를 구분할 수 없다(`:1993-2011`).

`ServiceSessionDelivery.deliver`의 현재 계약도 `boolean`뿐이다
(`framework/languages/node/packages/framework/src/runtime/foundation/service-stateful-runtime.ts:200-208`).
retained delivery와 direct delivery는 각각 이 boolean을 동기 호출한다(`:3572-3577,3599-3606`).
direct 경계의 바깥 catch도 모든 오류를 `protocolError`로 바꾸므로, 내부 catch만 좁혀도 typed
failure가 다시 소실된다.

local one-way send는 ingress 결과가 `application` 또는 `infrastructure`가 아니면 전부
`SubmitResult.InvalidState`로 접는다(`:4113-4160`). 이것이 target 삭제 이후 사용자가 본
`InvalidState`의 직접적인 public mapping이다.

원격 command 36은 one-way이며 source의 submit은 source-local raw transport admission에서 이미
완료한다. session owner에서 나중에 발생한 physical STREAM delivery 실패를 원격 source의 두 번째
completion으로 돌려보내면 안 된다. 그 실패는 session owner의 typed diagnostics/lifecycle에
남아야 한다. 반면 같은 process의 local fast path는 ingress를 await하므로 현재 구현처럼 세부 실패를
`InvalidState` 하나로 압축하지 말고 동일한 typed 의미를 보존해야 한다.

## 5. 교차언어 대조

세 구현 모두 bound-session의 실제 STREAM push에서 binding async terminal을 사용한다. Node만
`submit_sync()`와 catch-all session 삭제를 결합한 예외다.

| 언어 | bound-session push | 실패 처리 | async terminal |
|---|---|---|---|
| C++ | `stream_host_service.cpp:1446-1499`의 `send_core_frame()`이 `_core_socket->send(rid).message(...).async()` 후 await | send result/error를 호출 경계로 보존한다. 실제 disconnect는 별도 monitor가 받고 `close_core_session()`으로 수렴한다(`:1550-1617`). | 예 |
| .NET | `ZLinkBackendStreamSocketWrapper.cs:141-149`의 `SendAsync()`가 `_socket.Send(...).Message(payload).Async(cancellationToken)`을 직접 await | async 경로는 catch로 압축하지 않는다. 별도 public sync `Send()`만 typed `Backpressured`를 잡아 `false`로 만든다(`:125-139,152-165`). | 예 |
| Java | `ZLinkSessionActorsRuntime.java:1346-1357`이 `sendBoundSessionPushAsync(...).thenApply(true)`를 반환하며 retained path도 async를 사용한다(`:1372-1405`). `ZLinkJavaStreamSocket.submitOwnedStreamFrameAsync()`는 owned frame을 복사해 binding completion stage를 반환한다(`:228-255,307-329`). | synchronous setup 예외만 failed future로 만들고 session target을 지우지 않는다. binding completion owner가 exact WRITABLE과 typed terminal을 처리한다(`bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:1063-1090,1408-1444`). | 예 |

정확한 파일 경로는 다음과 같다.

- C++:
  `framework/languages/cpp/framework/src/runtime/streams/stream_host_service.cpp:1446-1499,1550-1617`
- .NET:
  `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/Wrappers/ZLinkBackendStreamSocketWrapper.cs:125-165`
- Java actor runtime:
  `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/actors/ZLinkSessionActorsRuntime.java:1193-1222,1346-1405`
- Java stream wrapper:
  `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaStreamSocket.java:228-255,307-329`

Java에는 legacy synchronous `sendBoundSessionPush()`도 존재하지만
(`ZLinkJavaStreamSocket.java:208-225`), command 36의 실제 actor delivery 경로는 위의 async 메서드를
사용한다. .NET의 sync 메서드 역시 public sync 의미를 위한 별도 경로이며 Framework internal
bound-session push가 아니다.

C++ monitor와 Java monitor도 write exception이 아닌 transport event를 session close의 근거로
사용한다. Java는 `ZLinkJavaStreamSocket.java:176-193`에서 `DISCONNECTED`를 관찰하고,
`framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/streams/ZLinkStreamRuntime.java:1113-1135`에서
session 제거와 callback을 수행한다. .NET도
`framework/languages/dotnet/src/Zlink.Framework/Runtime/Streams/ZLinkStreamNodeRuntime.cs:614-657,713-727`에서
monitor event를 처리한다.

## 6. 최소 수정 경계

승인 뒤 STAGE 2에서 필요한 최소 변경은 아래 경계에 한정한다.

### 6.1 delivery terminal

- `RawStreamSessionService.deliver()`를 awaitable delivery로 바꾸고 마지막 terminal을
  `await submit.submit()`으로 바꾼다.
- decode와 builder 조립은 transport-result catch 밖에 둔다. malformed payload, `TypeError`, 알 수
  없는 `Error`는 target을 유지한 채 그대로 throw한다.
- catch는 `SubmitError`의 typed `result`만 분기한다. `error.message`나 `nativeErrno` 문자열을 정책
  기준으로 사용하지 않는다.
- `NotConnected`, `NotFound`, `Terminated`처럼 계약상 route/socket terminal인 결과만 해당 delivery를
  closed/unavailable로 분류한다. 이 결과에서만 기존 close/unbind lifecycle을 요청하고, 그 경로가
  해당 target과 session callback을 한 번 정리하게 한다. Stage 2에서는 현재의 직접 map 삭제를
  유지하기보다 기존 lifecycle entry point를 재사용할 수 있는지 먼저 확인해야 한다.
- async binding이 정상 동작하면 transient backpressure는 pending Promise 내부에서 exact WRITABLE 후
  재제출되어 delivery catch에 도달하지 않는다. 그래도 typed Backpressured 또는 timeout/deadline이
  terminal로 올라오면 session target은 유지하고 typed capacity/deadline failure로 전달한다.
- `InvalidState`는 독립적으로 close가 확인되지 않았다면 프로그래밍 또는 socket-state 오류로 다시
  던진다. unknown result도 다시 던진다.

### 6.2 await와 typed result 전파

다음 직접/간접 호출자가 함께 영향을 받는다.

1. `ServiceSessionDelivery.deliver`의 boolean 계약
   (`service-stateful-runtime.ts:200-208`).
2. `bindSession()`의 delivery parameter와 저장 객체(`:1619-1645`).
3. `RawStreamSessionService.bindActor()`가 등록하는 closure
   (`node-raw-mesh-backend.ts:1930-1955`).
4. retained outbound operation의 `deliver()` callback과 payload settle 시점
   (`service-stateful-runtime.ts:3572-3581`). Promise terminal 전에 payload를 해제하면 안 된다.
5. direct delivery와 현재 catch-all `protocolError` 변환(`:3599-3606`).
6. command 36 ingress dispatch(`:2150-2159`).
7. local `submitOneWay()`의 ingress-result-to-`SubmitResult` mapping(`:4113-4160`).
8. backend `sendActorBoundSession()`(`node-raw-mesh-backend.ts:1332-1345`).
9. backend/node contract의 `sendActorBoundSession()` 선언
   (`framework/languages/node/packages/framework/src/runtime/backend/contracts/index.ts:362-368,412-420`).
10. Node adapter의 result mapping
   (`framework/languages/node/packages/framework/src/runtime/backend/mesh-actor-session-node-adapter.ts:24-47`).
11. public bound-session send chain:
    `streams/bound-session-service.ts:424-438`, `streams/session-calls.ts:92-108`,
    `streams/native-fallback-bound-session.ts:267-293`.

여기서 awaitable 계약은 성공 여부와 typed failure를 함께 보존해야 한다. `Promise<boolean>`을
선택하더라도 backpressure/timeout을 `false`로 합치지 말고 typed rejection으로 유지해야 한다.
정확한 public mapping은 기존 `SubmitResultValue`와 adapter의 다음 mapping을 유지한다.

- `Backpressured`/`NotAdmitted` -> `Backpressured`
- `NotFound`/`InvalidState` -> `TargetNotFound`
- `NotConnected` -> `RouteNotConnected`
- `Terminated` -> `Shutdown`

단, 원격 one-way source에 이미 완료된 admission 결과를 사후 delivery 결과로 덮어쓰지는 않는다.

### 6.3 고려한 대안

1. **선택:** binding async terminal + 좁은 typed catch + 기존 monitor lifecycle. 기존 계약과 세 언어
   구현에 일치한다.
2. **기각:** `submit_sync()`를 유지하고 Framework에서 backpressure를 검사해 retry한다. exact token과
   payload snapshot을 binding과 중복 소유하며 Framework submit spec을 위반한다.
3. **기각:** catch-all 뒤 target을 다시 넣거나 retry/timeout 횟수를 늘린다. 원인을 가리고 decode와
   programming 오류도 계속 오분류한다.
4. **기각:** 별도 STREAM poller 또는 pair/generation 상태를 추가한다. 이미 있는 monitor lifecycle과
   같은 사실을 두 곳에서 소유한다.

## 7. 회귀 테스트

기존 M6B test를 assertion 완화 없이 binding async terminal의 실제 의미로 갱신하고, 아래 사례를
각각 독립적으로 검증해야 한다.

1. **Backpressure 대기와 회복:** 첫 push 성공 후 두 번째 async submit은 pending 상태가 된다.
   exact WRITABLE을 발생시키면 동일 payload가 정확히 한 번 전달되고 Promise가 성공하며, 같은
   session의 다음 push도 성공한다. `sessionTargets`는 삭제되지 않는다.
2. **Typed capacity/deadline:** fake가 typed `SubmitError(Backpressured)` 또는 timeout/deadline을
   terminal로 반환하면 `InvalidState`나 session close가 아니라 대응하는 typed failure가 보존된다.
   이후 push가 가능해야 한다.
3. **Typed close terminal:** `NotConnected`, `NotFound`, `Terminated` 각각을 문자열 없이 분류한다.
   기존 monitor disconnect와 결합했을 때 target/session close와 callback이 정확히 한 번 발생하고,
   후속 send는 계약상 `RouteNotConnected`, `TargetNotFound`, 또는 `Shutdown`이 된다.
4. **Unknown 오류:** malformed application payload, message builder `TypeError`, 일반 `Error`가 그대로
   throw되고 session target은 유지된다. 이전 `submit_sync is not a function` 유형도 session close로
   바뀌지 않아야 한다.
5. **Retained delivery:** relocation retention callback은 async terminal까지 payload를 소유하고 성공
   후 한 번만 `settle()`한다. terminal failure에서도 double settle이나 premature release가 없어야
   한다.
6. **실제 Node binding 통합:** 작은 STREAM HWM으로 Promise가 pending됨을 확인하고 peer drain 뒤
   exact WRITABLE 재제출로 packet이 한 번만 도착하는지 검증한다. pending 중 target 제거/socket close는
   typed terminal과 기존 monitor session close로 끝나야 한다.
7. **Local/remote 의미:** local fast path는 typed result를 보존하고, remote one-way source는 raw
   transport admission 뒤 사후 client delivery ack를 받지 않는지 검증한다.

현재 fake의 “backpressure 즉시 `InvalidState`” 기대는 공통 submit 계약과 맞지 않는다.
회귀 테스트는 실패를 없애기 위해 assertion을 완화하는 것이 아니라, async binding이 transient
backpressure를 흡수하는 사례와 명시적인 typed terminal 사례를 분리해야 한다.

## 8. STAGE 1 종료 상태

- runtime/code 변경 없음
- Core, binding, protected spec 문서 변경 없음
- commit 없음
- 이 단계에서는 새 test를 실행하지 않음. 근거는 기존 3회 focused 결과, M6B 전체 결과, 보존된
  trace, 현재 소스와 교차언어 구현 대조임
- 구현 승인 전 여기서 중단함
