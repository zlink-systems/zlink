# .NET send 건당 비용 — 조사 결과

## 1. send 한 건이 지나는 경로

벤치 문서의 구현 표는 `zlink-framework-dotnet`을
`SendToChannel("bench", payload).Async()`로 설명하지만
[`framework/bench/grpc/doc/dotnet.ko.md:13`](../../../framework/bench/grpc/doc/dotnet.ko.md:13),
이 벤치의 실제 source는 target RID를 지정하는 node direct send다.
`FrameworkBenchTransport.SendAsync`가
`SendToNode("bench", Target, payload).Async()`를 호출한다
([`framework/bench/grpc/dotnet/Client/Program.cs:551`](../../../framework/bench/grpc/dotnet/Client/Program.cs:551)).
따라서 아래는 `send-saturation`의 이 실제 경로다.

### framework source A → socket send

1. `IZLinkRouteClient.SendToNode`가 `ZLinkRouteSendCall<TMessage>`와 call-local
   metadata/gate를 만든다
   ([`framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteClient.cs:60`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteClient.cs:60),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteClient.cs:278`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteClient.cs:278)).
2. `Async`는 한 번 제출만 허용한 뒤 envelope header와 protobuf body `Message` 두 개를
   만든다. `ZLinkProtobufCodec`는 `CalculateSize()`만큼 native `Message`를 할당하고
   그 span에 직접 protobuf를 쓴다
   ([`framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteClient.cs:300`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteClient.cs:300),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteClient.cs:338`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteClient.cs:338),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkClientCallCodec.cs:42`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkClientCallCodec.cs:42),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs:263`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs:263),
   [`framework/languages/dotnet/src/Zlink.Framework.Codecs.Protobuf/ZLinkProtobufCodec.cs:82`](../../../framework/languages/dotnet/src/Zlink.Framework.Codecs.Protobuf/ZLinkProtobufCodec.cs:82)).
3. `ZLinkSpotNodeRuntime.SendToNodeAsync`가 remote target에 대해 managed mesh node의
   direct async send를 호출한다
   ([`framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotNodeRuntime.cs:483`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotNodeRuntime.cs:483)).
4. `ZLinkManagedMeshNode.SendToNodeDirectAsync`는 peer를 찾고,
   service-wire `nodeSend` head와 framework multipart payload envelope을 만든다
   ([`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:8899`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:8899),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:9017`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:9017),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:9026`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:9026)).
5. 같은 node의 `_socketGate` 아래에서 binding의
   `socket.Send(target).Messages(messages).Async(...).EnsureAcceptedAsync()`를 호출하고,
   source-local admission만 기다린다
   ([`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:9060`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:9060),
   [`bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Send.cs:33`](../../../bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Send.cs:33),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkOneWaySubmitOutcome.cs:25`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkOneWaySubmitOutcome.cs:25)).

### raw source A → socket send

1. `send-saturation`일 때 raw transport는 concurrency 수만큼 `RawBenchSocket`을
   만들며, 기본값 8이면 서로 다른 ROUTER 8개다
   ([`framework/bench/grpc/dotnet/Client/Program.cs:570`](../../../framework/bench/grpc/dotnet/Client/Program.cs:570),
   [`framework/bench/grpc/doc/dotnet.ko.md:77`](../../../framework/bench/grpc/doc/dotnet.ko.md:77)).
2. 매 호출은 static raw envelope header를 `Message.From`으로 만들고, payload 크기만큼
   `Message.Allocate`한 뒤 protobuf를 span에 쓴다
   ([`framework/bench/grpc/dotnet/Client/Program.cs:632`](../../../framework/bench/grpc/dotnet/Client/Program.cs:632),
   [`framework/bench/grpc/dotnet/Client/Program.cs:658`](../../../framework/bench/grpc/dotnet/Client/Program.cs:658)).
3. 선택된 socket의 private `SemaphoreSlim`을 얻어
   `router.Send(peer).Message(header).Message(body).Async(...)`를 제출한다. backpressure일 때만
   binding admission task를 기다린다
   ([`framework/bench/grpc/dotnet/Client/Program.cs:674`](../../../framework/bench/grpc/dotnet/Client/Program.cs:674),
   [`framework/bench/grpc/dotnet/Client/Program.cs:729`](../../../framework/bench/grpc/dotnet/Client/Program.cs:729),
   [`bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Send.cs:33`](../../../bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Send.cs:33)).

### framework target B → typed handler

1. Mesh receive loop은 poller를 깨운 뒤 application job permit을 얻고, `_socketGate` 아래에서
   nonblocking `Recv`한다
   ([`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:4935`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:4935),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:4976`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:4976)).
2. `ProcessReceived`는 service head를 `ToArray()`로 복사하여 command를 판별하고,
   service wire와 single-frame framework multipart envelope을 decode view로 만든 뒤
   node mailbox에 enqueue한다
   ([`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5524`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5524),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5787`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5787),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5860`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:5860),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:10400`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:10400)).
3. persistent mesh dispatch worker는 ready signal → native claim →
   `ZLinkBackendRouteReceived` → node route dispatcher 순으로 넘긴다
   ([`framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:193`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:193),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:316`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:316),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:666`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshDispatchPump.cs:666)).
4. dispatcher가 envelope JSON header를 decode하고 inbound admission을 얻은 뒤,
   protobuf body를 native span에서 typed `BenchPayload`로 decode한다. 이후 DI scope를 만들고
   registered `CommandHandler.HandleAsync`를 호출한다
   ([`framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:352`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:352),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:520`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkMeshNodeRouteDispatcher.cs:520),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteHandlerInvoker.cs:35`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkRouteHandlerInvoker.cs:35),
   [`framework/languages/dotnet/src/Zlink.Framework/Runtime/Handlers/ZLinkHandlerDispatcher.cs:52`](../../../framework/languages/dotnet/src/Zlink.Framework/Runtime/Handlers/ZLinkHandlerDispatcher.cs:52),
   [`framework/bench/grpc/dotnet/ZLinkServer/Program.cs:65`](../../../framework/bench/grpc/dotnet/ZLinkServer/Program.cs:65)).

### raw target B → handler-equivalent

`RunCommandRouter`가 하나의 ROUTER에서 blocking `Recv`한 뒤 마지막 part를 protobuf로 decode하고
metrics counter만 갱신한다. service-wire decode, framework mailbox, application admission,
envelope JSON decode, DI scope와 handler dispatcher는 없다
([`framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs:112`](../../../framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs:112),
[`framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs:124`](../../../framework/bench/grpc/dotnet/ZLinkRawServer/Program.cs:124)).

## 2. 지목한 비용, 비싼 순서로

순서는 실행 경로에서 메시지마다 발생하는 동기화·큐잉·payload 작업의 수를 기준으로 한 코드
판정이다. 이 job에서는 시간을 재지 않았으므로 각 항목의 비율 기여도는 확정하지 않았다.

### 1. source의 공유 state lane과 공유 socket submit 경계

- **무엇** — framework의 8개 logical stream이 한 RouteMesh node의 peer lookup state lane과
  하나의 `_socketGate`를 공유한다.
- **어디** — `ZLinkManagedMeshNode.cs:9017-9024`, `9069-9077`, `11372-11376`;
  lane의 queued path는 `ZLinkStateLane.cs:101-117`, `162-201`.
- **왜 비싼가** — `RequireDirectPeer`는 매 message에 `RunState`로 `_peersByRid`를 읽는다.
  lane이 비어 있으면 inline 실행되지만, 앞선 send가 lane을 잡고 있으면 각 caller가
  `TaskCompletionSource`를 만들고 mailbox에 enqueue한 뒤 drain을 기다린다. 그 다음에도 모든
  sender가 같은 `_socketGate`에서 socket operation builder와 binding submit을 직렬화한다.
  이는 benchmark의 8 concurrent source Task가 하나의 framework node를 공유하는 구조와 결합한다
  (`Client/Program.cs:510-535`, `551-552`).
- **raw는 어떻게 하는가** — raw는 `Enumerable.Range(... SendConcurrency)`로 socket 8개를 만들고
  (`Client/Program.cs:570-577`), 각 socket이 자기 `SemaphoreSlim`만 사용한다
  (`Client/Program.cs:674-705`, `729-743`). 서로 다른 raw socket의 submit은 같은 framework
  state lane이나 socket gate를 공유하지 않는다.
- **고치는 방법** — admitted peer의 `(logical RID, physical RID, connection epoch)` immutable
  snapshot을 state lane에서 publish하고 outbound node direct send는 그 snapshot을 lock-free로
  읽게 한다. socket submit 직전에는 epoch가 현재 socket epoch인지 확인하여 stale snapshot을
  `NotConnected`로 끝내야 한다. 이렇게 하면 peer dictionary를 읽기 위한 매-message owner turn을
  없애되, peer lifecycle의 쓰기 소유권은 lane에 남는다. socket 한 개의 native submit contract가
  병렬 submit을 지원하는지도 binding/Core 계약으로 먼저 확인해야 한다. 지원하지 않으면
  `_socketGate` 제거가 아니라 RouteMesh outbound submit owner를 별도로 설계해야 한다.
- **위험** — disconnect/admission 갱신과 send 사이에 stale physical RID를 사용하거나,
  source-local admission 뒤 성공을 잘못 보고할 수 있다. snapshot publish와 socket generation
  fence가 없으면 기존 lifecycle 소유권을 깨뜨린다.

### 2. framework multipart envelope이 1 KiB body를 한 번 더 연속 frame으로 복사

- **무엇** — typed envelope의 header/body 두 native `Message`를 다시 service-wire의 하나의
  `framework-multipart-v1` payload frame으로 pack한다.
- **어디** — `ZLinkManagedMeshNode.cs:9026-9050`; 실제 part copy는
  `ZLinkApplicationPayloadEnvelopeCodec.cs:369-387`.
- **왜 비싼가** — send마다 service head `Message` 하나와 payload envelope `Message` 하나를
  새로 만들고, 후자는 part count/length를 기록한 뒤 header와 1 KiB protobuf body를 새 span으로
  `CopyTo`한다. protobuf 자체는 이미 native body span에 써졌으므로, 이 copy는 typed encode의
  결과를 socket 전에 다시 옮기는 추가 O(payload-size) 작업이다.
- **raw는 어떻게 하는가** — raw는 `Message.Allocate(payload.CalculateSize())`에 protobuf를 직접
  쓴 뒤 header/body 두 part를 그대로 `Send().Message(...).Message(...)`에 넘긴다
  (`Client/Program.cs:638-641`, `658-664`). 해당 두 part를 하나로 재포장하지 않는다.
- **고치는 방법** — 현재 service-wire v1에서는 제품 코드만 고쳐서 없앨 수 없다. multi-part
  application payload를 직접 wire record로 허용하는 새 wire profile을 설계하고 4언어 codec과
  compatibility를 함께 바꿔야 한다. 그 전에는 `EncodeFrameworkMultipartMessage`의 contiguous
  frame을 유지해야 한다.
- **위험** — frame 수·length validation·payload owner 수명을 바꾸면 모든 언어의 service-wire
  compatibility, malformed frame 거부와 handler 완료 뒤 release 계약을 깨뜨릴 수 있다.

### 3. target의 receive → mailbox → ready claim → dispatch worker handoff

- **무엇** — target은 raw socket 수신 뒤 즉시 handler를 부르지 않고 application permit,
  owned mailbox, ready signal/claim, route-received object와 persistent worker를 거친다.
- **어디** — `ZLinkManagedMeshNode.cs:4976-5036`, `5524-5887`, `10400-10437`;
  `ZLinkMeshDispatchPump.cs:173-191`, `263-453`, `666-716`.
- **왜 비싼가** — 메시지마다 permit lease 취득/반환, `Received` owner와 `QueuedRecord`, mailbox
  enqueue, interlocked ready-mask update와 semaphore wake/claim, `List<ZLinkBackendRouteReceived>` 및
  route-received object 생성이 있다. raw target은 한 blocking `Recv`, protobuf decode, counter
  update만 한다 (`ZLinkRawServer/Program.cs:112-127`). framework는 handler가 빠른 bench에서도
  backpressure·shutdown·payload lifetime을 위한 이 경계를 전부 지난다.
- **raw는 어떻게 하는가** — `RunCommandRouter`는 `Received`를 loop 밖에 한 번 만들고 재사용하며,
  queue/worker handoff 없이 수신 thread에서 `metrics.Record`까지 실행한다
  (`ZLinkRawServer/Program.cs:112-127`).
- **고치는 방법** — handler를 직접 호출해 mailbox나 application admission을 건너뛰어서는 안 된다.
  먼저 existing owner claim 하나가 여러 ready record를 처리할 수 있는지를 Core receive/claim
  계약과 함께 확인하고, 가능할 때에만 같은 owner의 bounded batch에서 `nodeRoutes` list와 wakeup을
  재사용하는 방향으로 줄여야 한다. 현재 `MeshReadyBatch.MaximumRecords = 1`
  (`ZLinkMeshDispatchPump.cs:199`)의 이유와 FIFO/fairness 검증을 확인하지 않은 상태에서 batch
  limit을 올리는 것은 수정안이 아니다.
- **위험** — handler 순서, host-wide application job queue admission, drain seal 중의 거부 및
  native payload release가 달라질 수 있다. 특히 "raw처럼 직접 dispatch"는 public runtime
  semantics를 바꾼다.

### 4. target handler마다 async DI scope와 scoped handler owner를 새로 생성

- **무엇** — route send handler도 매 message에 `CreateAsyncScope()`와
  `ZLinkScopedHandlerInstanceOwner`를 만들고 handler를 service provider에서 resolve한다.
- **어디** — `ZLinkHandlerDispatcher.cs:52-69`; route handler 경유는
  `ZLinkRouteHandlerInvoker.cs:35-71`.
- **왜 비싼가** — bench handler는 `BenchServerMetrics` 하나를 record하고 끝나지만, 그 전에
  async scope object와 handler owner를 만들고 dispose한다. filter가 없을 때 closure pipeline은
  피하지만 (`ZLinkHandlerDispatcher.cs:62-69`), scope/resolve는 남는다. raw target에는 DI
  resolution이나 disposal가 없다.
- **raw는 어떻게 하는가** — top-level `RunCommandRouter`가 이미 가진 `BenchServerMetrics`에
  직접 `Record`한다 (`ZLinkRawServer/Program.cs:112-127`).
- **고치는 방법** — scoped handler lifetime을 유지해야 하므로 일반 경로에서 scope를 cache하면 안
  된다. no-filter이며 registration이 명시적으로 singleton/stateless인 handler에만 registration
  시점에 pre-resolved invoker를 만드는 fast path는 가능하지만, handler lifetime을 드러내는
  configuration/contract 결정이 먼저 필요하다.
- **위험** — scoped dependency의 격리·dispose 시점, handler instance state 격리와 exception
  cleanup이 바뀐다. singleton 판정을 추측하면 cross-message state leakage가 생긴다.

### 5. send가 불필요한 application correlation을 만들고 hot header cache를 피함

- **무엇** — .NET command send도 `CreateEnvelope`의 기본값으로 새 correlation ID를 만든다.
- **어디** — `ZLinkRouteClient.cs:340-347`; default `includeCorrelationId = true`와
  `ZlinkStreamCorrelation.Next()`는 `ZLinkClientCallCodec.cs:7-40`; correlated header가 cache
  fast path에서 제외되는 조건은 `ZLinkEnvelopeCodec.cs:154-201`.
- **왜 비싼가** — message마다 correlation string 생성, dynamic header encoding과 unique JSON
  header가 발생한다. correlation이 null인 command header라면 cached header bytes를 사용할 수
  있지만, 현재는 `header.CorrelationId is null` 조건을 만족하지 않는다. target도 그 unique
  correlation field를 JSON header에서 읽어 `ZLinkRouteMessageContext`에 넣는다
  (`ZLinkMeshNodeRouteDispatcher.cs:369-392`, `ZLinkRouteHandlerInvoker.cs:52-66`).
- **raw는 어떻게 하는가** — raw command header는 static `RawEnvelopeHeaders.Request` byte array를
  매 message `Message.From`으로만 만든다 (`Client/Program.cs:638-641`).
- **고치는 방법** — command/publish send call site가 `CreateEnvelope(...,
  includeCorrelationId: false)`를 사용하게 하고, send handler context의 correlation을 null로
  만든다. public API signature는 바뀌지 않는다. C++ ClientServer one-way path도 command header를
  직접 만들어 correlation을 비워 두며 shared dialect의 요구라고 주석으로 명시한다
  (`framework/languages/cpp/framework/src/runtime/client_server/raw_client_server_owner.cpp:1504-1511`).
- **위험** — 현 .NET RouteMesh send handler가 비계약 correlation 값을 logging/filter key로
  사용하고 있었다면 null을 처리해야 한다. 그러나 common message model은 send의
  `CorrelationId`를 null, request를 non-null로 정한다
  (`framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:71-76`).
  C++ RouteMesh call site에도 같은 생성 패턴이 남아 있으므로, .NET만 고친 뒤 다른 언어의
  envelope parity를 별도 점검해야 한다.

## 3. 고치지 말아야 할 것

- **remote handler 완료를 send의 public completion에 연결하지 말 것.** 현재 source는
  `socket.Send(...).Async(...).EnsureAcceptedAsync()`까지만 기다리고
  (`ZLinkManagedMeshNode.cs:9060-9084`), request처럼 operation table이나 reply task를 만들지
  않는다. binding의 send admission 구현도 writable token이 없으면 완료 task 없이
  `SubmitResult.Ok`를 돌려준다 (`bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs:45-72`).
  raw도 같은 binding `Async` API를 호출한다. Send는 source-local admission에서 끝나며 remote
  handler 완료를 기다리지 않는다는 계약이므로
  (`framework/doc/framework/common/spec/server/00-foundation/04-interaction-model.ko.md:225-233`,
  `framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:33-44`),
  "completion을 없앤다"는 명목으로 admission await나 failure mapping까지 제거하면 안 된다.

- **service-wire `nodeSend`와 framework multipart payload envelope를 raw의 두 part 형태로
  바꾸지 말 것.** `nodeSend`는 application payload가 필수인 RouteMesh command다
  (`framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md:219-235`).
  service messaging의 여러 framework part는 하나의 application payload envelope으로
  전달하며 length/count를 검증한다
  (`framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md:185-211`).
  이 제약이 §2-2의 copy를 만든다.

- **target mailbox/permit/typed decode를 raw server처럼 생략하지 말 것.** receive 전에
  application job permit을 얻는 규칙은 wire spec에 있고
  (`framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md:208-211`),
  수신 payload는 handler 종료까지 framework가 소유하며
  typed handler에는 decoded object를 한 번 전달해야 한다
  (`framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:60-108`).
  이는 one-way라도 handler admission, shutdown, lifecycle과 payload release를 보장한다.

- **protobuf decode를 raw bytes handler API로 바꾸지 말 것.** 업무 handler에는 decoded typed
  value를 전달해야 하며 raw byte API는 transport/codec extension 용도만 허용된다
  (`framework/doc/framework/common/spec/server/01-execution/05-payload-ownership-and-codec.ko.md:102-126`).
  `ZLinkProtobufCodec`의 span decode
  (`ZLinkProtobufCodec.cs:125-135`)는 이미 native message buffer에서 바로 읽는다.

## 4. 확인 못 한 것

- .NET 벤치 문서의 §1 표는 framework send를 `SendToChannel`로 적지만
  (`framework/bench/grpc/doc/dotnet.ko.md:13`), 이 harness의 실제 call site는
  `SendToNode`이고 (`framework/bench/grpc/dotnet/Client/Program.cs:551-552`), common bench README도
  framework 행을 RID-direct node send라고 설명한다 (`framework/bench/grpc/README.ko.md:48,86`).
  문서 표가 의도적으로 API family만 요약한 것인지 갱신 누락인지는 이번 조사에서 판정하지 않았다.
- 각 §2 항목이 108.50 µs source / 75.72 µs target 중 정확히 얼마를 차지하는지는 profiler나
  perf ticket 측정 없이는 확정할 수 없다. 특히 state lane contention의 실제 queue hit 비율,
  `_socketGate` hold 시간, allocation/GC 비율은 이번 read-only 조사에서 확인하지 않았다.
- `MeshReadyBatch.MaximumRecords = 1`을 키워도 Core claim ordering, handler fairness와 application
  job queue 계약이 유지되는지는 Core/binding contract와 focused test로 확인하지 않았다.
- RouteMesh one-way command의 correlation null 규칙은 common message model에서 확인했지만,
  C++ RouteMesh channel runtime도 `create_envelope(command, ...)`로 correlation을 채우는 call site가
  있다 (`framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:1371-1384`).
  이 차이가 의도된 별도 RouteMesh contract인지, 같은 결함이 여러 언어에 있는지는 이번 .NET
  조사만으로 결론 내리지 않았다.
- service-wire payload를 zero-copy multi-frame으로 표현할 수 있는 Core/native binding capability와
  backward-compatible wire-version migration 범위는 확인하지 않았다.
