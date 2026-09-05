# Node M6B bound-session `InvalidState(8)` 진단

## 결론

실패한 test는
`framework/languages/node/test/m6b/m6b-runtime.contract.ts:5389`의
`raw backend dispatches Spot requests and Actor sends through M6B owners`다.
원인은 Core·Node binding의 reply 순서나 token 재사용이 아니다. Test가 만든 fake
`StreamSocket`의 send builder에는 이전 terminal인 `submit()`만 있고, 현재 Node binding
계약에 맞는 `submit_sync()`가 없다. Node Framework가
`node-raw-mesh-backend.ts:2004`에서 `submit_sync()`를 호출하면 `TypeError`가 발생하고,
기존 catch가 전달 실패로 바꾼다. Local command 36 ingress가 이 실패를
`protocolError`로 반환하며, `service-stateful-runtime.ts:4156-4158`이 최종
`SubmitResult.InvalidState(8)`을 만든다.

분류는 **A — 현재 binding terminal 계약에 맞추지 못한 test double**이다. 실제
Core/binding submit은 호출되지 않았으므로 B session용 public-API repro는 만들 근거가 없다.

## 재현 결과와 determinism

모든 실행은 `framework/languages/node`에서 `TMPDIR=/dev/shm/zlink-tmp-node`를 사용하고
`ZLINK_LIBRARY_PATH`를 unset했으며, 각 npm/test invocation 전체를
`flock -w7200 /tmp/zlink-node-gate.lock`으로 감쌌다. Core와 local package는 다시 빌드하지
않았다.

| 실행 | 결과 |
|---|---|
| 위 test만 단독 실행 1 | fail, `8 !== 0`, 21.788 ms |
| 위 test만 단독 실행 2 | fail, `8 !== 0`, 21.371 ms |
| 위 test만 단독 실행 3 | fail, `8 !== 0`, 19.785 ms |
| `npm run verify:m6b-runtime` 전체 파일 | 109 pass / 1 fail, 같은 test만 fail |

따라서 이 실패는 단독 **3/3 fail**이며 전체 파일에서도 **1/1 fail**인 결정적 실패다.

이전 108 pass / 2 fail에서 함께 실패한 test의 정확한 이름은
`remote User Spot target executes once and rewrites correlation on terminal replay`
(`m6b-runtime.contract.ts:860`)이다. 당시 aggregate에서 `0 !== 101`로 한 번 실패했지만
단독 재실행은 379 ms에 통과했다. 이번 전체 M6B 실행에서도 378.787 ms에 통과했다.
현재 증거에서 이 test는 비결정적이며, bound-session 실패와 원인이 같다는 근거가 없다.

## 실패 timeline과 정확한 submit 경계

기존 worklog에는 첫 `sendActorBoundSession()`의 예상값 `Ok(0)`, 실제값
`InvalidState(8)`만 기록돼 있었다. 기존 결과를 먼저 확인한 뒤 catch의 반환·순서를
바꾸지 않는 환경변수 제한 trace sink를 한 번 사용했다. 다음 stack을 얻은 뒤 sink는
제거했다.

```text
TypeError: submit.submit_sync is not a function
  at RawStreamSessionService.deliver (...node-raw-mesh-backend.js:1092)
  at ServiceStatefulRuntime.deliverBoundSession (...service-stateful-runtime.js:2253)
  at ServiceStatefulRuntime.submitOneWay (...service-stateful-runtime.js:2684)
  at ZLinkNodeRawMeshBackend.sendActorBoundSession (...node-raw-mesh-backend.js:689)
```

실제 순서는 다음과 같다.

1. Test는 fake STREAM을 `createFakeStream()`으로 만들고
   `createStreamSessionService()`에 넘긴다
   (`m6b-runtime.contract.ts:5517-5520`). `createFakeStream()`의 builder는
   `message()`, `flags()`, `submit()`만 제공한다(`:6625-6651`). 반환형을 `unknown`으로
   두었기 때문에 TypeScript가 `StreamSocket` 계약과 대조하지 못한다.
2. Local actor와 `session-a`의 bind가 완료된다. Test는 완료 record가 `Ok`인지 확인하고
   registry가 준 current `bindingGeneration`을 읽는다(`:5521-5532`). Binding token이나
   generation을 재사용한 실패가 아니다.
3. 첫 push는 `sendActorBoundSession()`에서 current binding generation과 actor fence를
   넘긴다(`:5533-5542`). Backend는 그대로 `ServiceStatefulRuntime.sendBoundSession()`에
   전달한다(`node-raw-mesh-backend.ts:1332-1344`).
4. `sendBoundSession()`은 current binding과 actor fence를 통과한 뒤 command 36을 만든다
   (`service-stateful-runtime.ts:1823-1844`). Actor owner와 session owner의 RID가 모두
   `m6b-node`이므로 Core ROUTER에 보내지 않고 local `submitOneWay()`로 들어간다
   (`:4111-4161`).
5. `deliverBoundSession()`은 current delivery와 네 generation/fence를 확인하고
   STREAM delivery를 호출한다(`:3547-3607`). 이 단계까지 거절된 상태는 없다.
6. `RawStreamSessionService.deliver()`는 socket type을 `StreamSocket`, operation을 routed
   **send**로 선택한다. 정확한 terminal 호출은
   `stream.send(rid).message(...).submit_sync()`의 마지막 호출
   (`node-raw-mesh-backend.ts:1997-2004`)이다. Fake에는 이 함수가 없어 Core 호출 전에
   `TypeError`가 발생한다.
7. `deliver()`의 catch가 session target을 지우고 `false`를 반환한다(`:2006-2011`).
   `deliverBoundSession()`은 이를 `protocolError`로 바꾸고(`service-stateful-runtime.ts:3599-3607`),
   local `submitOneWay()`가 `InvalidState(8)`로 바꾼다(`:4156-4158`).

이 호출에는 `ReplyToken`이 없다. REQUEST·reply operation도 아니고 command 36 one-way
send이므로 request ID와 reply token이 만들어지지 않는다. Core까지 진입하지 않았으므로
WRITABLE wait token도 없다. 앞서 성공한 Spot·Instance request의 reply token은 각각의
완료에서 이미 소비됐으며 이 send와 연결되지 않는다.

## Core·binding 계약과 소유 계층

Core 공통 결과표는 `ZLINK_SUBMIT_INVALID_STATE(8)`을 caller contract violation인 “핸들이
잘못된 상태”로 정의한다(`core/doc/spec/core/socket/README.ko.md:207-241`). ROUTER raw reply의
구체적인 `INVALID_STATE+EBUSY` 조건은 REQUEST `FINAL` 전 reply와 같은 token의 동시 second
sequence다(`core/doc/spec/core/socket/07-router.ko.md:276-315`). 이번 경로는 ROUTER reply가
아니므로 이 조항에 해당하지 않는다.

실제 대상인 STREAM routed send 계약은 `zlink_send_part_rid()`를 사용하며, 연결된 RID의
정상 admission, backpressure와 연결 없음 결과를 정의한다
(`core/doc/spec/core/socket/08-stream.ko.md:99-142`). Node binding은 STREAM
`send(routingId)`가 `SendOperation`을 반환한다고 정의하고
(`bindings/node/src/zlink/contracts/sockets/stream_socket.ts:13-20`), builder terminal은
managed async `submit(): Promise<void>`와 blocking `submit_sync(): void`다
(`bindings/node/src/zlink/contracts/messaging/operations.ts:17-31`). 실제 binding은
`StreamSocket.send()`에서 두 terminal을 모두 배선한다
(`bindings/node/src/zlink/runtime/sockets/stream_socket.ts:37-42`,
`socket_operation_builders.ts:51-65`).

따라서 소유권 판정은 다음과 같다.

- Binding은 STREAM queue admission과 managed `submit()`의 WRITABLE token 대기·재제출을
  소유한다. Framework가 이를 위한 token이나 두 번째 poller를 만들지 않는다.
- Framework는 command 36의 current binding/fence 검사와 local ingress 결과 변환을 소유한다.
  공통 계약도 session owner가 네 fence를 확인한 뒤 실제 STREAM connection에 제출하도록
  정한다(`04-session/02-session-actor-binding.ko.md:186-218`).
- 이번 실패에서는 Framework가 reply를 두 번 보내거나 `FINAL` 전에 reply하지 않았고,
  consumed `ReplyToken` 또는 wait token을 재사용하지 않았다. Binding이 이미 소유한 순서를
  Framework state machine으로 복제한 증거도 없다.
- 계약을 어긴 객체는 `unknown`으로 type 검사를 피한 test double이다. `360181172f`가 runtime을
  pull-completion binding surface의 `send(...).submit_sync()`로 바꿨지만 이 fake의 이전
  `submit()` terminal은 함께 바뀌지 않았다.

Framework의 public one-way 계약은 bound-session send를 source-local queue admission까지
기다리는 async terminal로 정의한다(`01-execution/01-submit-and-completion.ko.md:100-150`).
그러나 이 test에서 관찰한 실패는 그 admission 의미를 시험하기 전에 fake method lookup에서
발생했다. `submit_sync()` 사용 자체의 public latency 적합성을 바꾸려면 별도 runtime 진단과
승인이 필요하며, 이 A 수정에 섞을 근거는 없다.

## 교차언어 대조

| 언어 | 동등 경로와 test | 결과 |
|---|---|---|
| C++ | command 36을 받은 session owner는 current tenure를 검사하고 delivery capability를 실행한다(`public_host_runtime.cpp:5584-5676`). STREAM sink 제출은 `actor_gateway_runtime.cpp:3103-3188`이 담당한다. M6B의 `verify_bound_session_push_uses_session_registry_when_gateway_projection_rejects()`는 `test_cpp_framework_m6b_runtime.cpp:1498-1606`에서 이 경로와 실제 delivery 1회를 확인한다. | 최종 C++ gate에서 `test_cpp_framework_m6b_runtime` 통과. 이 함수는 binary main에서 호출된다(`:6926`). |
| .NET | current local binding은 `ZLinkActorBoundSessionCoordinator.Send()`가 session admission capability에 전달한다(`ZLinkActorBoundSessionCoordinator.cs:845-897`). `Local_Actor_Bound_Session_Send_Uses_The_Bound_Stream`은 정확한 stream write를 확인한다(`SessionActorCoordinatorTests.cs:156-179`); fake는 해당 언어의 `IZLinkStream.Write`를 정확히 구현한다(`:2646-2667`). | `gate-v5-rest.trx`, `gate-v6-rest.trx`, `gate-v7-rest.trx`에서 모두 pass. 최신 `gate-v7-rest.trx` 전체도 1,920/1,920 pass. |
| Java | raw M6B test `remoteBoundStreamUsesRawMeshAndExactGenerationFences`가 두 command 36 push와 exact generation fence를 확인한다(`ZLinkJavaRawSpotNodeM6BTest.java:2563-2715`). Production receive owner는 handler stage를 기다리지 않고 관찰한다(`ZLinkJavaRawMeshNode.java:6343-6387`), session owner는 current route를 판정한 뒤 async STREAM 제출을 시작한다(`ZLinkSessionActorsRuntime.java:1193-1222`, `ZLinkJavaStreamSocket.java:228-249`). | 세 관련 focused test가 `BUILD SUCCESSFUL`; Java core 전체의 남은 2개 실패는 별도 M6A descriptor-fence test다. |

세 언어에는 Node test처럼 binding `StreamSocket`인 척하면서 현재 terminal을 빠뜨린 객체가
없다. C++·.NET test double은 Framework가 정의한 sink/stream interface를 구현하고, Java M6B
test는 실제 wrapper에 명시적인 bound-session sink를 주입한다. Node만 test double이 binding
builder 구조를 수동 복제하면서 API 변경을 놓친 구조적 차이가 있다.

## 분류와 Stage 2 제안

분류는 **A — binding 계약 적응 누락**이다. Production Framework·Core·binding 결함인 B로
분류하지 않는다.

최소 수정 위치는
`framework/languages/node/test/m6b/m6b-runtime.contract.ts:6625-6651`의
`createFakeStream()`뿐이다. Fake send builder가 현재 `SendSubmitOperation`의
`submit_sync(): void`를 제공하고, backpressure·disconnect는 실제 binding처럼 terminal에서
예외로 나타내야 한다. 가능하면 반환형을 `unknown` 대신 필요한 `StreamSocket` 부분 타입과
대조해 다음 terminal 변경을 compile 단계에서 검출한다.

대안으로 production `RawStreamSessionService.deliver()`를 async `submit()` 경로로 바꾸는 방법이
있지만, delivery port의 `boolean` 계약과 ingress settlement까지 함께 바꾸는 runtime 변경이다.
현재 실패는 실제 binding 호출 전에 발생하므로 이 대안은 최소 원인 수정이 아니다.

회귀 증명은 기존 test의 네 assertion을 그대로 통과시키는 것이다.

1. current binding의 첫 push는 `Ok`이고 payload가 한 번 전달된다.
2. backpressure push는 `InvalidState`이며 payload를 추가하지 않는다.
3. 상태를 복원한 push는 `Ok`이고 두 번째 payload가 정확히 한 번 추가된다.
4. disconnect 뒤 push는 `InvalidState`다.

추가로 fake를 current binding type과 정적으로 대조하면 `submit_sync` 같은 필수 terminal이
빠진 fixture가 typecheck를 통과하지 않는 것이 재발 방지 조건이 된다. Assertion과 deadline을
바꿀 필요가 없다.

Core/binding B session용 public-API repro는 **해당 없음**이다. 실제 `createStreamSocket()`과
`stream.send(rid).message(frame).submit_sync()`를 사용하면 이번 `TypeError`를 만들 수 없고,
그 호출에서 `INVALID_STATE+EBUSY`가 관찰된 증거도 없다.
