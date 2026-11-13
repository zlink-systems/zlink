# m1675c — Node #1675 메시지당 비용 감축 계획

진행 단계 1(계획)의 결과다. 코드는 바꾸지 않았다. 감독자가 승인한 항목부터 단계 2를 시작한다.

- worktree: `/root/worktree/zlink-1675-node`, 브랜치 `framework-node/1675-per-message-cost`, HEAD `90aafafaff`(main과 같음). 아래 file:line은 모두 이 HEAD 기준이다.
- 경로 접두사: `F=framework/languages/node/packages/framework/src/runtime`, `SW=framework/languages/node/packages/stream-wire/src/internal`.
- 빌드 준비: `scripts/gate/rebuild-dev.sh m1675c`를 백그라운드로 시작했다(로그 `/root/jobs/fx/cx/m1675c/rebuild.log`, 세부 `zlink-work/gates/m1675c/`). 계획은 정적 분석과 m1639n3의 계수를 근거로 했고, 이 계획을 쓰는 동안 측정은 하지 않았다.
- `origin/framework/1648-receive-path`(HEAD `85d0eceeb8`)에는 Node 변경이 없다(.NET·Java만). .NET 커밋 `1620452aa7`("수신 예산을 count로 보유하고 실제 record에만 lease를 만든다")은 Node 항목 P6의 교차언어 기준으로 삼는다. 그 브랜치는 읽기만 했다.

## 1. binding 경로와 framework 경로의 메시지당 작업

비교 대상은 `framework/bench/grpc/node`의 두 행이다. binding 행은 `zlink-raw-server/main.js`와 `client/main.js`의 raw transport이고, framework 행은 `zlink-framework-server/main.js`(NestJS host)와 `client/framework-transport.js`(`requestToNode`/`sendToNode`)다. 두 행 모두 protobuf encode·decode를 메시지마다 한다. 아래 표는 그 공통 작업 밖에서 각 경로가 하는 일만 적는다. 횟수는 정적 추정이며, 괄호 안 값은 m1639n3 최종 계수(두 host 합계, 1024B)다.

### 1.1 send — target(수신) 쪽

| 단계 | binding 경로 | framework 경로(현재 HEAD) |
|---|---|---|
| I0 깨우기 | `setImmediate(pump)` 회전, 최대 20000회 idle spin 뒤 1ms timer | readable callback → `pump()` async → `pumpBatch()` async, 진행이 있으면 `yieldToIO` Promise |
| I1 permit | 없음 | 회전마다 permit 64개 상태 전환 + 미사용 63개 되돌림(P6) |
| I2 claim | 재사용하는 `Received` 하나에 `recv` | `new Received`, rid 문자열·rid Buffer 복사, `BigInt` 순번, 모든 part `toBytes()` 전체 복사, closure 2~3(P2) |
| I3 header | 해석하지 않음(envelope는 고정 Buffer) | service-wire header 객체, m6a 문자열 2개, JSON header 문자열 5개 이상·객체 2개, flow 문자열(P2·P4·P5), 배열 literal 2개(P11) |
| I4 commit | 없음 | `ApplicationIngressRecordOwner`·공유 상태·lease, mailbox record(spread 임시 객체 포함), `ReceiveRecord` 22필드 + closure 3 + `operationId` + part wrapper 2(P2) |
| W1–W3 | 함수 직접 호출 | Promise 사슬 약 8개(handler 자체 Promise 제외)와 ALS `enterWith` 2회, `runWithFlow` 1회, admission claim 객체·closure(P1). (PROMISE 12.7, Buffer 16, sampled heap 9.2KB) |

### 1.2 request — source(송신·완료) 쪽

| 단계 | binding 경로 | framework 경로 |
|---|---|---|
| E1 | 고정 envelope Buffer + payload `Message.from` | `DefaultZLinkRequestCall`·closure 2, ZLinkRequest closure 4·`then`·`defineProperty`, outbound flow ALS `run`·UUIDv7 생성, header 2-pass 기록·`Buffer.alloc`, `randomUUID().replaceAll`, `Date.toISOString`, m6a frame 복사(P3·P4·P5) |
| E3 | binding 내부 pending 1개 | pending 4겹: completion table slot Promise, `OperationRegistry` Promise·entry·`Map<bigint>`, backend `observeCompletion` `then`, binding reply Promise(P3) |
| E4 | `request().message().message().timeout().submit()` | 같은 binding 호출 + async wrapper 2개(`request`, `bindingRequestReply`) |
| E5 | `reply.then` 1회 | `request.then` → registry resolve → backend `then` → table resolve → `submitRequestOperation` await → `requestScoped` await → ZLinkRequest `then`(P3). (request-serial PROMISE 50.2 두 host 합계, Timeout 3.25) |

### 1.3 request — target reply 쪽

| 단계 | binding 경로 | framework 경로 |
|---|---|---|
| W4 | `received.reply().message(고정 envelope).message(payload).submit()` | reply JSON header(flow가 있어 준비된 header를 쓰지 못함) 2-pass 기록, m6a frame, reply wire header, 배열 spread 2, 응답마다 `BindingRoutingId.from`(P4·P9) |

정리하면 framework가 스펙상 반드시 더 하는 일은 envelope header 해석 1회(I3), permit 계수, application handler의 Promise, 기본 diagnostics 수준(`errors`)의 flow 전파다. 나머지는 아래 항목들이 줄일 대상이다. 0.90 비율이 Node에 그대로 적용되는지는 이 차이를 횟수로 줄인 뒤 감독자의 단독 시간 측정으로 판정한다(#1639).

## 2. 항목 요약(비용이 큰 순)

| # | 항목 | 메시지당 비용(정적 추정, 괄호는 n3 계수) | 허용 작업 표 위반 | 소유 스펙 | 분류 | 스펙 개정 |
|---|---|---|---|---|---|---|
| P1 | target dispatch Promise 사슬 | Promise 약 8, closure 6 이상, ALS 3회 (send target PROMISE 9.3) | 예 — 메시지별 Promise·wrapper | 08 §4.3 W1–W3, 02 §2·§4 | B | 불필요 |
| P2 | ingress record 재구성·payload 복사(#1648 Node 몫) | 객체 약 20, closure 6, 문자열 3, payload 전체 복사 1, rid 복사 1 | 예 — 소유권 이전 대신 복사, queue 사이 재구성 | 08 §4.1 I2–I4, 05 §2·§4 | B | 불필요 |
| P3 | request source pending 4겹 등록 | Promise 약 10, closure 10 이상, BigInt 2 (source PROMISE 약 25) | 예 — E3 등록 1회 초과, 메시지별 Promise | 08 §3 E3–E5, 01 §10·§11 | B | 불필요 |
| P4 | 기본 `errors` 수준의 flow가 준비된 header를 막음 | header 2-pass·`Buffer.alloc` 2(요청·응답), UUIDv7 생성 1(randomBytes + 문자열 6), ALS `run` 2~3 | 예 — 준비된 header 미사용 | 08 §3 E1, 06-obs/04 §3·§4, 06-obs/03 §5 | B | 후보(§5) |
| P5 | envelope header 문자열 재구성(#1643 Node 몫) | 문자열 7~9, 객체 3, `isUtf8` 전체 검사 1 | 일부 — header 해석 2벌(m6a + JSON) | 08 §4.1 I3, 05-message-model | B | 불필요 |
| P6 | 수신 회전 permit 저장소 선형 처리 | 회전당 상태 전환 64 + 되돌림 63 + 저장소 길이만큼 scan. serial은 메시지당 같은 값 | 예 — 메시지별 permit 예약·반환 | 04-ajq §3·§4, 08 I1 | B | 불필요 |
| P7 | pump 양보 예산이 idle 이후에 바로 소진됨 | claim마다 `setTimeout(0)` 1(최소 1ms), 추가 worker 기동 (serial Timeout 3.25) | 예 — 불필요한 macrotask | 08 §4.3 W5·§6, 02 「9」 | B | 불필요 |
| P8 | 회전마다 관리 작업·timer ref 변경 | 회전당 async 2, liveness tick 1, peer 확인 loop 1, serial은 timer ref/unref 1 | 예 — 관리 작업이 deadline이 아니라 회전마다 실행 | 08 §4.2 | B | 불필요 |
| P9 | reply 경로 재포장 | `BindingRoutingId` 1, 배열 2, 수신 때 만든 closure 3 | 예 — binding reply 1회 초과 | 08 W4 | B | 불필요 |
| P10 | send 대상 분류의 선형 검사 | manual peer 수만큼 `routingIdsEqual`, closure 호출 3 | 예 — 미리 준비한 target 미사용 | 08 E2, 02-channel-messaging 후보 목록 | B | 불필요 |
| P11 | 수신 분류의 배열 literal | 배열 2(11칸·4칸) | 예 — 할당 | 08 I3 | B | 불필요 |
| P12 | completion table 확장 재색인 | 정상 상태 0, 확장할 때마다 전체 재색인·closure 64. 조회마다 BigInt→Number 변환 4 | 아니오(확장 시에만) | 08 E3 | B | 불필요 |
| P13 | stream-wire base64 byte 순회 | JSON bytes 필드의 byte마다 순회, decode 뒤 다시 encode해 비교 | RouteMesh protobuf bench 경로 밖 | 05 §2 | B | 불필요 |

스펙 약어: 08=`spec/server/01-execution/08-messaging-hot-path.ko.md`, 02=`02-handler-turn-and-execution-gate.ko.md`, 05=`05-payload-ownership-and-codec.ko.md`, 01=`01-submit-and-completion.ko.md`, 04-ajq=`04-application-job-queue-and-backpressure.ko.md`, 06-obs/03·04=`spec/server/06-observability/03-message-flow-tracing.ko.md`·`04-flow-correlation.ko.md`.

## 3. 항목별 상세

### P1. target dispatch Promise 사슬

현재 위치:
- `F/backend/mesh-dispatch-pump.ts:184-212` — `dispatchRecord`가 결과를 `Promise.resolve(result).then`으로 감싸고, `finishRecord`가 다시 `Promise.resolve().catch`로 감싼다.
- `F/backend/mesh-dispatch-pump.ts:307-387` — ready claim마다 `async` closure(`drainClaim`)와 그 Promise를 만들고, record마다 `await dispatched`한다.
- `F/application-jobs/application-job-queue-scope.ts:37-57` — record마다 ALS `enterWith` 2회와 `Promise.resolve(result).then`.
- `F/spots/spot-node-runtime-manager.ts:873-899` — `async dispatchMeshRecord`가 다시 `await dispatcher(...)`.
- `F/host/index.ts:2867-2871`, `F/admission.ts:66-109` — record마다 claim 객체·`close` closure·`then` 2 closure와 Promise.
- `F/channels/channel-dispatchers.ts:688-760` — record마다 `writeReply`·`writeError` closure, fields·context 객체, 처리기가 있어도 만드는 `missingHandlerMessage` template 문자열(:748).
- `F/channels/channel-dispatch-pipeline.ts:89-141,143-241` — `async dispatchOneWay`/`dispatchRequest`의 Promise와 `await`, `createInboundFlow` 객체, `runWithFlow` closure·ALS `run`.

메시지당 비용: handler 자체 Promise를 빼고 Promise 약 8, closure 6 이상, ALS 3회. Node 22의 ALS는 async_hooks 기반이므로 Promise마다 hook 비용이 붙어, Promise 수가 곧 비용 배수다.

위반: 허용 작업 표의 "같은 owner turn에서 handler 호출"을 넘어 메시지별 Promise·wrapper를 만든다. 08 §4.3("record마다 별도 dispatch task나 supervisor wrapper를 만들지 않는다")에도 어긋난다.

교차언어:
- C++ `framework/languages/cpp/framework/src/runtime/channels/route_packet_dispatcher.cpp`는 동기 호출로 handler에 들어간다.
- .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:475` `DispatchRecord`·`ZLinkMeshNodeRouteDispatcher.cs:269`는 ValueTask 하나다.
- Java `.../runtime/channels/ZLinkChannelRuntime.java:1387,1477`.

수정안:
- pump → host → channel dispatcher → pipeline을 handler 호출까지 동기 반환(`void | Promise`)으로 바꾼다.
- handler가 Promise를 돌려줄 때만 continuation 하나를 붙인다. 그 continuation에서 reply 기록, trace, permit 반환, admission 해제, `finishRecord`를 한 번에 한다.
- admission claim은 기존 gate 상태의 counter 증감으로 바꾸고, 해제는 같은 terminal에서 한다(gate 하나, 판정 위치 수 유지).
- ALS `enterWith` 2회는 record 경계 한 번으로 줄인다. `missingHandlerMessage`는 처리기가 없을 때만 만든다.

### P2. ingress record 재구성과 payload 복사(#1648의 Node 몫)

현재 위치:
- `F/backend/node/node-raw-binding-port.ts:474-519` `receiveRecord`:
  - 메시지마다 `new Received()`.
  - `routingId.toString()`과 `toBytes()`(rid 복사).
  - `BigInt` 순번(:231-236), reply closure 2, `close` closure, 조건부 spread 임시 객체.
  - `received.parts.map((part) => part.toBytes())`(:510). binding이 이미 `copyData`로 만든 managed Buffer를 한 번 더 전체 복사한다(binding `bindings/node/src/zlink/contracts/messaging/message.ts:138-140,309-329`).
- `F/foundation/raw-service-mesh-runtime.ts:762-766` — `ApplicationIngressRecordOwner`와 공유 상태 객체.
- `F/foundation/raw-service-mesh-runtime.ts:911-925` — `serviceIngress` 인자 literal과 spread.
- `F/foundation/raw-service-mesh-runtime.ts:1097-1110` — mailbox record와 spread.
- `F/backend/node/node-raw-mesh-backend.ts:2139-2178`(claim), `:2276-2330` — `ReceiveRecord` 22필드, `receiveIngressLifecycle` 객체·closure(:2440-2451), `operationId` 객체, `reply`·`replyActorJoin` closure, m6a 문자열 2(`decodeApplicationEnvelope`), `decodeMultipart` 배열과 part wrapper 2.

메시지당 비용: 객체 약 20, closure 6, 문자열 3, payload 전체 복사 1, rid 복사 1. binding 경로는 `Received` 하나를 재사용하고 binding 복사 1회만 한다.

위반:
- payload 소유권을 이전하지 않고 복사한다. 05 §2의 "Framework가 더하는 복사"이고, 08 I4의 "record는 이동하며 복사하지 않는다"에 어긋난다.
- 같은 record를 세 표현(`ZLinkRawReceivedRecord` → mailbox record → `ReceiveRecord`)으로 다시 만든다. 08 §4.1의 "수신 뒤 다시 꺼내 다른 queue로 옮기는 단계를 두지 않는다"는 취지에 어긋난다.

교차언어:
- C++ `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.hpp:142-162`는 `_received`를 재사용한다.
- .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5490-5500`는 `Received.Create` + `RawIngressOwnership` 하나다.
- Java `.../runtime/binding/ZLinkJavaRawServicePort.java:373`는 `new Received`다.

수정안:
- part는 `data()`로 binding의 managed Buffer를 그대로 소유 이전한다.
- rid는 binding `RoutingId` 객체를 그대로 들고, 문자열은 기존 port의 `routingIds` 대응을 쓴다. 새 cache는 만들지 않는다.
- raw runtime이 최종 `ReceiveRecord` 형태(prototype method를 가진 class)를 한 번 만들어 mailbox에 넣고, backend claim은 그 객체를 그대로 넘긴다.
- m6a의 packetName·contentType은 실패 문맥에서만 쓰므로 decode하지 않거나 늦게 decode한다.
- `Received` 재사용은 part 수명이 dispatch 끝까지 이어지므로 part를 분리한 뒤에만 가능하다. 구현 전에 binding `Received`의 part 분리 API를 확인한다.

### P3. request source의 pending 4겹 등록

현재 위치:
- `F/channels/channel-clients.ts:209-224` — 요청마다 closure 2. send(:190-206)처럼 준비하지 않는다.
- `F/messaging/request-result.ts:6-57` — closure 4, `then`, Promise 인스턴스에 `defineProperty`.
- `F/channels/channel-transports.ts:412-534` — `runWithOutboundFlow`, `async requestScoped`.
- `F/channels/channel-transports.ts:685-710` — `async submitRequestOperation`, closure 2, `performance.now` 2.
- `F/backend/mesh-completion-table.ts:147-180` — slot Promise.
- `F/backend/node/node-raw-mesh-backend.ts:692-703,1598-1651` — id 객체, `then`, `completionRecord` 객체(:2187-2216).
- `F/foundation/raw-service-mesh-runtime.ts:1228-1234,1316-1365` — BigInt correlation, `OperationRegistry` 등록, `request.then`, 결과 객체 spread.
- `F/foundation/operation-registry.ts:50-81` — Promise, entry, `Map<bigint>`.
- `F/backend/node/node-raw-binding-port.ts:318-338`, `F/backend/node/node-backend-adapter-support.ts:211-241` — async wrapper 2.

메시지당 비용: framework Promise 약 10, closure 10 이상, BigInt 2. binding 경로는 binding reply Promise와 `then` 1이다.

위반: E3의 "pending entry 등록 한 번"과 허용 작업 표의 "binding reply 1회"를 넘는다. 메시지별 Promise이기도 하다.

교차언어:
- .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:116`(`RegisterRequestBeforeSubmit`), `:176`(`TryComplete`)는 table 하나를 쓴다.
- C++ `framework/languages/cpp/framework/src/runtime/foundation/operation_registry.hpp:38`.
- Java `.../runtime/internal/service/ZLinkServiceOperationRegistry.java:72,119`, `.../runtime/internal/ZLinkCompletionBridge.java:64`.

수정안:
- 원격 node/channel request는 completion table slot을 유일한 pending entry로 둔다.
- binding reply Promise의 continuation 하나가 reply wire header를 확인하고 slot을 동기로 완료한다.
- deadline은 이미 binding `.timeout()`이 소유하므로 `OperationRegistry` 등록과 backend `observeCompletion` 간접 단계를 원격 request에서 뺀다. local request는 기존 registry를 유지한다. 이 판정은 raw runtime 한 곳에 둔다.
- `requestScoped`·`submitRequestOperation`의 async를 없앤다.
- requestToNode의 closure는 send와 같은 준비 구조로 옮긴다.
- service-wire의 node request correlation(header 안의 값)은 wire 규격이므로 유지한다.

### P4. 기본 `errors` 수준의 flow가 준비된 header를 막음

현재 위치:
- `F/host/index.ts:3164-3170` — 기본 수준이 `errors`이므로 flow 생성이 켜져 있다.
- `F/channels/channel-transports.ts:331-337,438-447` — 송신마다 ALS `run`.
- `F/diagnostics/flow-context.ts:81-86,100-109` — `randomBytes(16)`, hex, slice 5, template.
- `F/channels/channel-envelope.ts:153` — flow를 header에 넣는다.
- `F/channels/channel-envelope.ts:544-581` — 준비된 header는 `flowId === undefined`일 때만 쓴다. 그래서 `:585-620`의 2-pass 기록과 `Buffer.alloc`이 요청·응답마다 실행된다.
- target 쪽은 `F/channels/channel-dispatch-pipeline.ts:110-128,165-190`(`createInboundFlow`, `runWithFlow`)에서 비용이 생긴다.

메시지당 비용: header 2-pass 2회와 `Buffer.alloc` 2, UUIDv7 1회(randomBytes Buffer 1, 문자열 6), ALS `run` 2~3.

위반: 허용 작업 표의 "미리 준비한 header로 encode 1회"를 지키지 못한다. flow 전파 자체는 06-obs/04 §4가 요구하므로 위반이 아니다.

교차언어:
- .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkChannelCalls.cs:19`, `.../Diagnostics/ZLinkFlowContext.cs:68`(호출마다 생성), `.../Messaging/ZLinkEnvelopeCodec.cs:174`(`EncodeHeader`).
- Java `.../runtime/spots/ZLinkSpotRoutedOutbound.java:63`, `.../runtime/messaging/ZLinkChannelEnvelope.java:307`.
- C++ `framework/languages/cpp/framework/src/runtime/diagnostics/flow_context.cpp:94`, `.../messaging/envelope_codec.cpp:350`.
- 네 언어 모두 `errors`에서 송신마다 flow를 만든다. 언어 사이 동작 차이는 없다.

수정안:
- flowId는 고정 36 byte이고 origin은 열거값이다. 그래서 기존 준비된 header를 "flow 자리 포함" 형태로 넓혀, 준비된 byte에 flowId 36 byte만 써 넣는다.
- UUIDv7은 process 하나의 무작위 기준값과 단조 counter로 만든다(RFC 9562의 counter 방식). 메시지마다 `randomBytes`를 호출하지 않는다.
- outbound flow는 ALS `run` 대신 encode 인자로 넘긴다. 단, async continuation이 ambient flow를 읽는 지점을 단계 2에서 확인한다.

### P5. envelope header 문자열 재구성(#1643의 Node 몫)

현재 위치:
- `F/channels/channel-envelope.ts:861-868` — canonical 경로 앞에서도 header 전체를 `isUtf8`로 검사한다.
- `F/channels/channel-envelope.ts:877-930` — 필드마다 문자열 5개 이상과 record 객체를 만들고, `validateChannelHeader`가 객체를 하나 더 만든다.
- `F/channels/channel-envelope.ts:92-94` — 요청마다 `randomUUID().replaceAll`.
- `F/channels/channel-envelope.ts:162` — 요청마다 `toISOString`.
- `F/foundation/service-wire-m6a-codec.ts:262-275` — JSON header와 같은 값을 m6a에서 또 문자열로 만든다.

메시지당 비용: 수신 문자열 7~9, 객체 3, UTF-8 전체 검사 1. 송신은 요청마다 문자열 2~3.

위반: I3의 "header만 해석"은 지키지만, 같은 값을 header 두 벌에서 각각 문자열로 만든다.

교차언어:
- .NET `ZLinkEnvelopeCodec.cs:335-373`은 `ReadOnlySpan`에서 decode한다.
- Java `ZLinkChannelEnvelope.java:706-767`.
- C++ `envelope_codec.cpp:804-834`.

수정안:
- canonical 경로의 channel·message·contentType byte를 등록된 이름의 준비된 byte와 비교해, 이미 있는 문자열을 재사용한다(등록 시점 사실이므로 새 cache가 아니다).
- canonical reader가 ASCII를 검증하면 `isUtf8`을 생략한다.
- header 객체를 하나로 줄인다.
- correlation_id는 06-obs/04 §3이 "같은 lifecycle에서 대기 중 request 사이 중복 없는 1..64 byte ASCII"만 요구하므로 process 접두사 + counter로 만든다.
- deadline 문자열은 스펙 형식이 ISO 문자열이므로 유지하고, 계수에서 따로 기록한다.

### P6. 수신 회전 permit 저장소의 선형 처리

현재 위치:
- `F/host/application-job-queue.ts:198-232` — `reserve()`가 회전마다 저장소를 처음부터 훑어 최대 64개를 `reserved`로 바꾼다. 모자라면 64개씩 저장소를 늘린다.
- `F/host/application-job-queue.ts:482-496` — `releaseUnusedReceiveRotation`이 미사용분을 하나씩 되돌린다.
- 호출은 `F/foundation/raw-service-mesh-runtime.ts:688-718`이다.

메시지당 비용: 회전당 상태 전환 64, 되돌림 63, 저장소 길이 S만큼 scan(S ≥ 64, 처리 중 handler 수만큼 증가). request-serial처럼 회전당 record가 1이면 이 비용이 그대로 메시지당 비용이다.

위반: 08 I1은 회전 예산을 count로 정한다. 지금 구현은 메시지별 permit 예약·반환 작업을 64배로 한다.

교차언어:
- .NET `1648-receive-path`의 `1620452aa7`(`ZLinkManagedMeshNode.cs`)은 count로 보유하고 실제 record에만 lease를 만든다. main은 `ZLinkApplicationJobQueue.cs:339` `TryAcquireBatch`다.
- Java `.../internal/dispatch/ZLinkApplicationJobQueue.java:331`.
- C++ `framework/languages/cpp/framework/src/runtime/dispatch/application_job_queue.hpp:385`.

수정안:
- 회전 예산은 count로만 보유한다.
- `take()`가 기존 저장소의 idle permit을 빈 목록 index(stack)로 O(1)에 꺼낸다. 이 저장소는 이미 있는 재사용 저장소이며 새 pool이 아니다.
- 미사용 반환은 count 하나로 queue owner에 돌려준다. FIFO·pressure 판정 위치는 n3이 정한 queue owner 한 곳 그대로다.

### P7. pump 양보 예산이 idle 이후 곧바로 소진됨

현재 위치:
- `F/backend/mesh-dispatch-pump.ts:405-416` — `yieldStartedAtMs`는 양보한 뒤에만 갱신된다. 그래서 idle 뒤 첫 claim을 처리하면 경과 시간이 2ms를 넘은 것으로 보여 `setTimeout(0)`(최소 1ms)으로 양보한다. 호출 위치는 `:376-377,396-397`, 정의는 `:458-460`이다.
- 그동안 새 readiness가 오면 `:134-136`이 application worker를 더 띄운다.

메시지당 비용: serial에서 claim마다 Timeout 1(n3 Timeout 3.25/메시지), 추가 worker Promise.

위반: 08 §6의 "묶음 처리의 시간 예산이 끝나면" 양보한다는 규칙과 맞지 않는다. 지금은 예산을 쓰지 않았는데도 양보한다.

교차언어:
- .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkExecutionLanePolicy.cs:6`(owner 시간 예산 10ms, turn 시작 기준).
- Java `.../execution/ZLinkSerialExecutionQueue.java:51,283`.
- C++ `framework/languages/cpp/framework/src/runtime/dispatch/receive_batch_budget.hpp:13`.

수정안: worker가 idle에서 깨어 drain을 시작할 때 예산 시작 시각과 건수를 함께 초기화한다. 판정 위치는 `yieldIfNeeded` 한 곳 그대로다. 양보 수단(timer)은 바꾸지 않는다.

### P8. 회전마다 관리 작업과 timer ref 변경

현재 위치:
- `F/foundation/raw-service-mesh-runtime.ts:679-720` — `pumpBatch`가 async이고, 회전마다 `observe` closure를 만들며, `:714-717`에서 회전마다 `expectedPeers` 확인과 `tickLiveness()`를 실행한다.
- `F/backend/node/node-raw-mesh-backend.ts:1561-1580` — `pump` async와 `yieldToIO` Promise.
- `F/backend/node/node-raw-mesh-backend.ts:1548-1559` — pump가 끝날 때마다 `scheduleMaintenance`의 `hasRef/ref/unref`를 실행한다. serial에서는 `OperationRegistry`의 pending이 0과 1을 오가므로 요청마다 ref가 바뀐다(`F/foundation/operation-registry.ts:73,135`).

메시지당 비용(serial): async Promise 2, liveness tick 1, timer ref 전환 1~2.

위반: 08 §4.2는 관리 작업을 다음 deadline 기준으로 실행하라고 정한다.

교차언어:
- Java `1648-receive-path` `7787607857`("Combine Java liveness deadline scan").
- .NET `ZLinkManagedMeshNode.cs:5346` receive wake timer.
- C++ host dispatch thread의 관리 deadline.

수정안:
- liveness tick과 peer 확인은 liveness owner의 다음 deadline이 지났을 때만 실행한다.
- `pumpBatch`는 대기할 일이 없으면 동기로 끝낸다.
- P3로 원격 request가 registry를 쓰지 않으면 serial의 ref 전환도 사라진다.

### P9. reply 경로의 재포장

현재 위치:
- `F/backend/node/node-raw-mesh-backend.ts:2322-2327`.
- `F/foundation/raw-service-mesh-runtime.ts:632-654` — 배열과 spread.
- `F/backend/node/node-raw-binding-port.ts:238-257` — `preparedRoutingId`가 `Uint8Array`는 대응표 없이 매번 `BindingRoutingId.from`을 호출한다.
- `:490-509` — 수신 때 reply closure 2개를 만든다.

메시지당 비용: `RoutingId` 1, 배열 2, closure 3.

위반: 허용 작업 표의 "binding reply 1회"를 넘는다.

교차언어:
- C++ `raw_route_port.hpp:144` `reply(request, parts)`는 받은 record에서 바로 reply한다.
- .NET `ZLinkManagedMeshNode.cs`와 Java `ZLinkJavaRawServicePort.java`도 받은 record의 route를 그대로 쓴다.

수정안: 수신 record가 binding `RoutingId`와 reply token을 그대로 들고, reply는 그 값으로 binding reply 1회만 한다. P2와 같은 커밋으로 묶는 것이 자연스럽다.

### P10. send 대상 분류의 선형 검사

현재 위치:
- `F/channels/channel-transports.ts:385-394` — 매번 `routingIdsEqual(source, target)`과 `manualNodeTarget`.
- `F/host/mesh-router-resolver.ts:16-24` — Map 조회, `?? []`, manual peer 선형 `some`.
- manual이 아니면 `F/channels/channel-transports.ts:1026-1037`이 `peers()` 전체 snapshot을 만든다(`F/backend/node/node-raw-mesh-backend.ts:754`~).

메시지당 비용: manual peer 수만큼 비교, closure 호출 3. manual이 아닌 경로에서는 peer snapshot 배열들을 만든다.

위반: 허용 작업 표의 "미리 준비한 target"과 E2의 "변경 시점에 준비한 후보 목록"에 어긋난다.

교차언어:
- .NET `.../Runtime/Spots/ZLinkSpotPeerConnectionSet.cs:113`(lane + `Contains`).
- Java `ZLinkJavaRawMeshNode.java:1265-1344`.

수정안:
- manual peer 분류는 registration 시점에 정해지므로 registration에서 RID 집합을 한 번 만들어 O(1)로 조회한다.
- `isKnownBackendPeer`는 topology owner의 기존 `admittedPeer` 조회를 쓴다.

### P11. 수신 분류의 배열 literal

`F/foundation/raw-service-mesh-runtime.ts:1063-1070`(4칸)과 `:1604-1618`(11칸)이 메시지마다 배열을 만든다. 수정안은 `switch` 또는 모듈 상수 하나로 판정하는 것이다(판정 위치 수는 그대로). P2와 같은 커밋에 넣는다.

### P12. completion table 확장 재색인

`F/backend/mesh-completion-table.ts:60-80`은 slot 64개를 늘릴 때마다 bucket 전체를 다시 만들고 모든 slot을 다시 색인한다. 최대 in-flight N까지 늘어나는 동안 O(N²/128)이다. 정상 상태의 메시지당 비용은 0이며, request-window(100)에서는 확장이 1회뿐이다. `:82-95`의 bucket 계산은 조회마다 BigInt→Number 변환 2와 `Math.trunc` 2를 하고, n3이 기록한 "hash bucket 4회/건"이 이것이다.

수정안: bucket 배열은 부하율이 넘을 때만 두 배로 늘리고, 새 slot만 색인한다. P3로 원격 request id를 table이 직접 발급하면 BigInt 없는 정수 key로 줄일 수 있다. 확장 시에만 드는 비용이므로 우선순위가 낮다.

### P13. stream-wire base64 byte 순회

- `SW/framework-json-v1.ts:345-351` — `String.fromCharCode(...chunk)` + `btoa`.
- `SW/framework-json-v1.ts:353-358` — `atob` 뒤 byte마다 `charCodeAt`.
- `:131-132` — decode한 뒤 다시 encode해 canonical 여부를 비교하므로 decode마다 작업이 두 배다.

RouteMesh protobuf bench 경로 밖이며, JSON codec의 bytes 필드(ClientServer stream 등)에 해당한다.

교차언어: .NET `Convert.FromBase64String`, Java `Base64`는 runtime 내장 변환이다.

수정안: Node에서는 `Buffer`의 base64 변환을, browser에서는 지금 경로를 쓴다. canonical 판정은 길이·padding·마지막 문자 bit 검사로 바꾼다. 이 패키지는 browser 겸용이므로 환경 판정을 한 곳에 둔다.

## 4. 확인한 이전 항목의 현재 상태

- m1639n3의 permit 일괄 반환(4/65/4)은 HEAD에 남아 있다(`F/host/application-job-queue.ts:482-496`). 남은 비용은 P6의 회전당 64개 상태 전환이다.
- m1675b의 payload 유지 정책 일원화와 completion·binary reply 복사 제거는 HEAD에 남아 있다(`F/backend/mesh-completion-table.ts:233-261`). 수신 ingress의 `toBytes` 복사(P2)는 m1675b 범위 밖이어서 남았다.
- m1675b가 "이번 수정 대상이 아님"으로 남긴 send 정책 경로는 P4·P10에 해당한다.
- m1639n3이 BLOCKERS로 남긴 Unity LICENSE 메타 불일치는 성능 항목이 아니므로 이 job에서 다루지 않는다.

## 5. 스펙 개정 후보

이번 계획의 P1~P13은 모두 현행 스펙 안에서 고칠 수 있다. 아래 한 건은 수정 뒤에도 남는 스펙상의 메시지당 비용이라 감독자 판단을 요청한다. 이 후보 때문에 다른 항목을 멈추지 않는다.

- 위치: `framework/doc/framework/common/spec/server/06-observability/04-flow-correlation.ko.md:80-98`(§4)과 `F/host/index.ts:3164-3170`(기본 수준 `errors`).
- 근거: tracing이 `Off`가 아니면 application이 시작한 첫 outbound마다 flow_id를 새로 만들고 envelope에 넣어야 한다. 기본 수준 `errors`에서도 모든 메시지가 UUIDv7 36 byte를 만들고 전파하며, 수신 쪽은 flow context를 설치한다. P4로 비용을 줄여도 메시지당 생성 1회, header 36 byte, 수신 context 설치 1회는 남는다. 네 언어가 모두 같다.
- 제안 문안: "Diagnostics level이 `Errors`이면 flow 식별자는 오류 기록을 만드는 처리 지점에서만 만든다. 정상 완료된 message에는 `flow_id`를 추가하지 않는다." 또는 "bench와 hot path 비교는 `Off`를 기준으로 한다." 어느 쪽을 택할지와 오류 상관관계를 얼마나 잃는지는 감독자가 판단할 문제다.

## 6. 단계 2의 계수 방법

시간은 재지 않는다. m1639n3 방식을 이 머신에 다시 만든다. 그 runner(`coefficients.cjs`)는 Windows 경로에 있어 이 머신에는 없다. 계수 코드는 `/root/jobs/fx/cx/m1675c/` 아래에만 두고 커밋하지 않는다.

- 대상: public NestJS host 두 개(같은 process), bench protobuf serializer, 1024B. 준비 뒤 패턴마다 정확히 128건(send, request-serial, request-backpressure 8 동시)을 처리한다.
- 계수와 방법:
  - PROMISE init·callback: `async_hooks`.
  - Immediate·Timeout·Microtask 생성·실행: `async_hooks`의 resource type.
  - closure 수: heap sampling의 `(closure)` 노드.
  - Buffer factory 호출 수와 반환 길이.
  - JS 할당 byte: HeapProfiler sampling interval 128B, GC로 수거된 객체 포함.
  - native 호출 수: binding addon 함수 wrapper.
- 단계별 집계: 첫 runtime frame 기준으로 E1–E5, I0–I4, W1–W4에 배정한다. 같은 방법으로 binding bench 두 행도 센다(§1의 binding 열을 실측으로 채운다).
- 항목마다 수정 전 → 수정 → 수정 후를 같은 runner로 세어 표로 남긴다. 회귀 시험은 n3이 추가한 `node-request-rotation-cost.test.js`·`node-path-budget.test.js` 형식을 따른다.

## 7. 승인 요청

- 구현 제안 순서: P2(+P9·P11) → P1 → P3(+P12의 id 부분) → P6 → P7 → P4 → P5 → P8 → P10 → P13 → P12.
- 수신·dispatch 쪽(P2·P1)이 send와 request 두 패턴에 모두 걸리고 Promise·할당이 가장 크므로 먼저 둔다.
- P4의 flow 자리 header와 P5의 이름 재사용은 같은 `channel-envelope.ts`를 고치므로 연속 커밋으로 둔다.
- 각 항목은 커밋 하나다. 판정 위치 수는 P1(admission 해제 2→1), P3(pending 완료 판정 4→1), P6(예산 보유 2→1)에서 줄고 나머지는 같다.
