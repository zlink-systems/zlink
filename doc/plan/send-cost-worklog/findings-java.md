# Java send 건당 비용 — 조사 결과

이 기록은 `send-saturation @ 1 KiB`의 제공된 source `1.48 → 11.56 µs/message`, target
`1.88 → 26.56 µs/message` 수치를 설명하기 위한 코드 조사다. profiler와 bench는 실행하지
않았으므로 아래 순서는 각 role의 실측 총비용과 매 message마다 확인되는 작업량으로 정한
수정 우선순위이며, 항목별 µs 분해 측정값은 아니다.

## 1. send 한 건이 지나는 경로

벤치 문서의 구현 표에서 `zlink-framework-java`는 `ZLinkRouteClient.sendToChannel(...).submit()`
표면을, `zlink-java`는 command 전용 raw ROUTER의 `send(peer)`를 사용한다
([java.ko.md](../../../framework/bench/grpc/doc/java.ko.md) §1, §3). 이 Java 행의 framework
source는 channel이 아니라 node-direct API를 호출한다.

### Framework source → socket send

1. `FrameworkStack.send()`가 `BenchPayload`를 만들고
   `route.sendToNode(...).submit()`을 호출한다:
   `framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/FrameworkStack.java:78`.
2. `ZLinkChannelRuntime.sendToNode()`가 typed payload를 encode하고 `RouteSendCall`을 만든다:
   `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelRuntime.java:1108`.
   `ZLinkPayloadEncoding.encode()`는 protobuf serializer 결과를 `Message`로 복사한다:
   `.../runtime/messaging/ZLinkPayloadEncoding.java:27`.
   이 벤치의 protobuf serializer는 `MessageLite.toByteArray()`를 호출한다:
   `framework/languages/java/zlink-framework-codec-protobuf/src/main/java/systems/zlink/framework/codecs/protobuf/ZLinkProtobufMessageSerializer.java:23`.
3. `RouteSendCall.submit()`은 single-submit gate와 flow scope 뒤에 registry의 node submit을
   호출한다: `.../runtime/channels/ZLinkChannelRouteCalls.java:145`.
4. `ZLinkChannelSocketRegistry.submitToNode()`은 state lane에서 route/node를 고른다:
   `.../runtime/channels/ZLinkChannelSocketRegistry.java:357`; lane 진입은
   `runAsync(...).join()`이다: `.../runtime/channels/ZLinkChannelSocketRegistry.java:124`.
5. 이 벤치에서는 node branch가 shared channel envelope를 만들고
   `node.sendToNode()`로 넘긴다: `.../runtime/channels/ZLinkChannelRouteCalls.java:177`.
   envelope header는 JSON writer로 만든다:
   `.../runtime/messaging/ZLinkChannelEnvelope.java:221`.
6. Spot node가 MeshNode `sendNode()`로 위임한다:
   `.../runtime/binding/ZLinkJavaRawSpotNode.java:327`. MeshNode는 service `nodeSend` header와
   framework multipart frame을 만든다:
   `.../runtime/binding/ZLinkJavaRawMeshNode.java:1632`,
   `.../runtime/binding/ZLinkJavaRawMeshNode.java:4213`.
7. Service port는 이 새 frames를 `RouterSocket.send(target)` builder에 넣고 binding submit
   completion을 반환한다: `.../runtime/binding/ZLinkJavaRawServicePort.java:97`.
   binding은 DONT_WAIT submit과 completion-id를 사용한다:
   `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:105`.
8. `RouteSendCall`은 binding stage를 public one-way stage로 적응하고 모든 original part를
   completion에서 닫는다: `.../runtime/channels/ZLinkChannelRouteCalls.java:197`.

### Raw source → socket send

1. `RawStack.send()`은 `router.send(peer)`를 열고, 고정 JSON envelope와 protobuf body의
   두 `Message`를 넣어 submit한다:
   `framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/RawStack.java:76`.
   body도 같은 protobuf runtime으로 만든다:
   `framework/bench/grpc/java/shared/src/main/java/systems/zlink/bench/withgrpc/shared/RawWire.java:41`.
2. `NativeRouterSocket.send()`은 곧바로 `MessageOperations.SendBuilder`에 binding send를
   연결한다: `bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterSocket.java:42`;
   builder는 part list를 그대로 submit한다:
   `bindings/java/src/main/java/systems/zlink/runtime/messaging/MessageOperations.java:79`.
3. `CompletionOwner.submitSend()`이 두 native message header를 DONT_WAIT으로 submit하고,
   즉시 수락이면 input part를 닫고 완료 admission을 반환한다:
   `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:105`.

즉 raw도 protobuf DTO와 두 wire part를 만든다. framework만 typed payload의 추가 배열 복사,
공유 envelope JSON, service header, multipart 재물질화, registry turn, public one-way 적응을
더 지난다.

### Target 수신 → handler

target 비용은 socket 수신 뒤 다음 경로에서 발생한다.

1. MeshNode receive pump가 `ApplicationJobQueue` permit을 먼저 얻은 뒤 record를 receive하고
   dispatch한다:
   `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:4397`.
2. MeshNode가 `nodeSend` service header와 framework multipart를 검증·decode하고
   `ZLinkMeshDispatchRecord`를 만든다:
   `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:4753`,
   `.../ZLinkJavaRawMeshNode.java:4801`,
   `.../ZLinkJavaRawMeshNode.java:4881`.
3. `ZLinkMeshApplicationDispatcher.accept()`이 envelope header를 decode하고 node-send namespace를
   선택한다: `.../runtime/channels/ZLinkMeshApplicationDispatcher.java:145`.
4. `dispatchSend()`이 namespace serial queue에 작업을 넣고 handler invocation을 연결한다:
   `.../runtime/channels/ZLinkMeshApplicationDispatcher.java:353`.
5. handler invoker는 `Message`를 `byte[]` 기반 encoded payload로 바꾼 뒤 protobuf parser로
   decode하고 context/filter/handler를 호출한다:
   `.../runtime/channels/ZLinkChannelHandlerInvoker.java:481`,
   `.../runtime/channels/ZLinkChannelHandlerInvoker.java:611`.
   실제 bench handler는 body header를 기록하고 즉시 completed future를 반환한다:
   `framework/bench/grpc/java/zlink-framework-server/src/main/java/systems/zlink/bench/withgrpc/frameworkserver/BenchCommandHandler.java:24`.

raw target은 command ROUTER receive loop에서 마지막 part의 `ByteBuffer`를 protobuf로 직접
parse하고 metric만 기록한다:
`framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/ZLinkRawBenchServer.java:138`,
`.../RawWire.java:56`.

## 2. 지목한 비용, 비싼 순서로

### 1. Target의 permit·serial queue·handler dispatch

- **무엇** — 한 send마다 target이 host-shared application permit, namespace serial queue,
  handler invocation과 여러 `CompletionStage`를 통과한다.
- **어디** — permit 획득은
  `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/dispatch/ZLinkApplicationJobQueue.java:129`,
  queue 등록은 `.../runtime/channels/ZLinkMeshApplicationDispatcher.java:372`, queue의
  synchronized enqueue와 virtual-thread drain은
  `.../execution/ZLinkSerialExecutionQueue.java:127`,
  `.../execution/ZLinkSerialExecutionQueue.java:516`, handler stage wrapping은
  `.../runtime/channels/ZLinkChannelHandlerInvoker.java:128`이다.
- **왜 비싼가** — 빈 queue여도 `acquire()`는 lock을 얻어 permit과 pressure 상태를 갱신하고,
  `enqueue()`는 queue monitor, `Entry`, `CompletableFuture`, flow state를 만든다. 첫 entry는
  shared virtual-thread drain executor에 schedule된다. 그 뒤 `executeHandler()`도 result future와
  `Runnable`을 만들며 job ownership을 transfer한다. `BenchCommandHandler`는 즉시 끝나므로
  이 scheduling·stage 비용이 업무 처리보다 지배적이다. 이 경로는 target framework
  `26.56 µs/message` 대 raw `1.88 µs/message` 차이가 모이는 곳이다.
- **raw는 어떻게 하는가** — command loop는 `recv` 뒤 `BenchPayload.parseFrom(ByteBuffer)`와
  `metrics.record`만 수행한다:
  `framework/bench/grpc/java/zlink-raw-server/src/main/java/systems/zlink/bench/withgrpc/rawserver/ZLinkRawBenchServer.java:138`.
- **고치는 방법** — host permit과 namespace FIFO라는 소유권은 유지하되, permit 획득부터
  ready handler까지를 하나의 allocation-lean ingress job으로 표현한다. 즉 즉시 permit이 있는
  경우 `CompletionStage`/`whenComplete` 사슬을 만들지 말고, queue의 기존 batch drain에서 handler를
  직접 시작한다. handler가 실제로 비동기인 경우에만 completion continuation을 붙인다.
- **위험** — permit 반환이 handler 첫 instruction 직전이어야 하고, queue FIFO·relocation seal·
  cancellation/close가 유지되어야 한다. ready fast path가 별도 queue나 별도 상태를 만들면 이
  계약을 깨기 쉽다.

### 2. Service multipart 포장과 target의 재물질화

- **무엇** — framework envelope part와 payload를 하나의 service multipart native `Message`로
  다시 쓰고, target에서 다시 개별 `Message`로 복사한다.
- **어디** — source의 pack은
  `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:333`
  와 `.../runtime/binding/ZLinkJavaRawMeshNode.java:4213`; target의 multipart view와
  `Message.from(ByteBuffer)` materialization은
  `.../runtime/internal/service/ZLinkFrameworkMultipartView.java:22`와
  `bindings/java/src/main/java/systems/zlink/contracts/messaging/Message.java:413`이다.
- **왜 비싼가** — source는 part count와 길이를 쓰고 각 `Message` bytes를 새 native frame에
  복사한다. target은 같은 frame을 parse한 뒤, dispatcher가 header와 payload를 읽을 때 각각
  새 native `Message`로 복사한다. 따라서 1 KiB body가 raw처럼 native message header를
  참조 공유하는 대신, framework에서는 service frame으로 한 번 쓰이고 target typed dispatch를
  위해 다시 materialize된다.
- **raw는 어떻게 하는가** — raw source는 envelope/body `Message` 두 개를 직접 submit한다
  (`framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/RawStack.java:80`).
  binding의 `CompletionOwner`는 native headers를 `messageCopyTo`로 stage하며 payload storage는
  refcount로 공유한다고 명시한다:
  `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:345`.
  raw target은 body `ByteBuffer`를 바로 parser에 준다 (`.../RawWire.java:62`).
- **고치는 방법** — service wire의 outer record는 유지하되, 받은 multipart frame을 owner가
  살아 있는 동안 header decoder와 protobuf decoder에 zero-copy `ByteBuffer` view로 제공한다.
  handler가 비동기로 frame lifetime을 넘길 때만 그 part를 materialize한다. source도 binding이
  scatter/gather ownership을 받을 수 있다면 service header와 multipart view를 하나의 큰 native
  frame으로 복사하지 않도록 service-port/binding 경계를 바꾼다.
- **위험** — received owner를 handler completion까지 유지해야 하므로 close 순서·backpressure
  charge·비동기 handler lifetime이 달라진다. borrowed `ByteBuffer`를 public handler API로
  노출하면 lifetime 계약도 바뀐다.

### 3. Source state-lane 왕복

- **무엇** — route가 이미 준비된 node-direct send도 channel registry의 state lane에 post한 뒤
  호출 thread가 `join()`으로 기다린다.
- **어디** — `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/channels/ZLinkChannelSocketRegistry.java:357`,
  `.../ZLinkChannelSocketRegistry.java:416`,
  `.../runtime/internal/execution/ZLinkStateLane.java:59`.
- **왜 비싼가** — 매 send가 `WorkItem`과 result future를 mailbox에 넣고 virtual-thread executor를
  schedule하며, caller는 join한다. state lane은 한 turn만 실행하므로 source의 8개 submit thread가
  같은 selection turn을 serialize한다. raw source는 이 framework owner turn 없이 socket의
  completion owner로 바로 간다.
- **raw는 어떻게 하는가** — `NativeRouterSocket.send()`은 binding send builder를 즉시 만든다:
  `bindings/java/src/main/java/systems/zlink/runtime/sockets/NativeRouterSocket.java:42`; binding 내부의
  lock은 native DONT_WAIT submit과 pending retry 등록만 보호한다:
  `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:105`.
- **고치는 방법** — route lifecycle이 만든 immutable ready transport handle을 단일 source of
  truth로 publish하고, send는 handle의 retain/close fence만 확인한 뒤 직접 submit한다. 연결 교체와
  close만 state lane이 직렬화한다. 이는 selection state를 caller 쪽에 복제하지 않는 방식이어야 한다.
- **위험** — send와 disconnect/relocation 사이에 닫힌 socket이나 이전 node handle을 쓰지 않게
  fence의 linearization point를 보장해야 한다. direct path가 existing route-not-connected 오류
  분류를 우회해서는 안 된다.

### 4. Typed protobuf encode/decode의 추가 배열 복사

- **무엇** — framework의 protobuf serializer가 payload bytes를 immutable wrapper로 두 번 복사하고,
  target도 `Message`를 배열 wrapper로 두 번 복사한 뒤 parse한다.
- **어디** — source serializer는
  `framework/languages/java/zlink-framework-codec-protobuf/src/main/java/systems/zlink/framework/codecs/protobuf/ZLinkProtobufMessageSerializer.java:23`,
  wrapper의 방어 복사는
  `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/ZLinkEncodedPayload.java:13`와
  `.../ZLinkEncodedPayload.java:18`, native message copy는
  `bindings/java/src/main/java/systems/zlink/contracts/messaging/Message.java:259`이다. target은
  `ZLinkMessagePayloads.encoded()`가 `message.toByteArray()`를 거쳐 같은 wrapper를 만든다:
  `.../runtime/messaging/ZLinkMessagePayloads.java:12`; protobuf parser는
  `.../ZLinkProtobufMessageSerializer.java:48`이다.
- **왜 비싼가** — source의 `toByteArray()` 뒤 `ZLinkEncodedPayload.from()`와 `bytes()`가 각각
  `Arrays.copyOf`를 하고, `Message.from(byte[])`가 native payload로 다시 복사한다. target은
  native `Message.toByteArray()`와 같은 두 wrapper copy를 반복한다. raw도 protobuf encode/decode는
  하지만 `RawWire` target은 ByteBuffer를 직접 parse하므로 target wrapper copies가 없다.
- **raw는 어떻게 하는가** — raw source의 protobuf serialize는 `payload.toByteArray()` 한 번과
  `Message.from(...)` 한 번이다:
  `framework/bench/grpc/java/shared/src/main/java/systems/zlink/bench/withgrpc/shared/RawWire.java:49`;
  target은 `BenchPayload.parseFrom(encoded.duplicate())`다:
  `.../RawWire.java:57`.
- **고치는 방법** — serializer internal API를 byte-array ownership wrapper 대신 `Message` 또는
  borrowed `ByteBuffer` result로 바꾸고, protobuf parser에는 message view를 직접 전달한다. public
  typed API는 유지하고, ownership이 필요한 boundary에서만 copy한다.
- **위험** — `ZLinkEncodedPayload`의 현재 defensive-copy 의미가 바뀐다. codec extension과
  asynchronous handler가 mutable/expired buffer를 보지 않도록 새 internal ownership type이 필요하다.

### 5. Source public one-way completion·중복 close 경로

- **무엇** — 이미 즉시 수락된 send에도 Framework가 public cancellation/error mapping future와
  multiple completion callback을 만든다.
- **어디** — `adaptOneWay()`은 새 cancel-forwarding future와 `whenComplete`를 만든다:
  `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/calls/ZLinkOneWayCalls.java:63`.
  `RouteSendCall`은 그 뒤에 또 `whenComplete(parts::close)`를 붙인다:
  `.../runtime/channels/ZLinkChannelRouteCalls.java:197`. Service port도 owned message close callback을
  붙이고: `.../runtime/binding/ZLinkJavaRawServicePort.java:115`, caller `sendNode()`도 같은 service
  frames를 finally에서 닫는다: `.../runtime/binding/ZLinkJavaRawMeshNode.java:1655`.
- **왜 비싼가** — binding 성공은 static completed admission을 반환하지만
  (`.../runtime/binding/ZLinkJavaSocketSupport.java:66`), framework는 매 send마다 independent future,
  completion lambda, error unwrap path와 close iteration을 만든다. `Message.closeAll`을 소유 경계가
  겹쳐 여러 번 호출한다. raw bench의 OK send completion은 단순 completed future다:
  `framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/RawStack.java:92`.
- **raw는 어떻게 하는가** — binding `CompletionOwner.submitSend()`은 OK이면 part를 닫고 static
  admission stage를 반환한다: `bindings/java/src/main/java/systems/zlink/runtime/sockets/CompletionOwner.java:113`.
- **고치는 방법** — one-way adapter가 binding의 immediate-admitted case를 구분하게 하고, public
  stage isolation을 만족하는 independent already-completed stage를 즉시 반환한다. service port와
  caller 중 한 곳만 service frame을 닫도록 ownership을 한 곳으로 모은다. backpressured case만
  pending-stage cancellation forwarding과 deferred close를 유지한다.
- **위험** — public cancellation/stage isolation, DONT_WAIT retry, native submit failure의
  `Unavailable`/`DeadlineExceeded` mapping, 실패 시 message ownership을 모두 보존해야 한다. 이
  항목은 source 총비용의 일부일 뿐이며, profiler로 immediate-admitted 비율과 close 호출수를
  먼저 확인해야 한다.

## 3. 고치지 말아야 할 것

- **source-local admission 경계와 오류 분류** — `Send`는 source outbound queue가 수락할 때만
  완료하고 target handler 실행을 기다리지 않는다. 따라서 completion 최적화는 future를 없애거나
  target 완료 ACK를 추가하는 방식이 아니라, 같은 admission 결과를 더 싸게 표현해야 한다.
  근거: `framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md` §4,
  `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md` §4~§5.
- **typed envelope와 service multipart wire shape** — packet name, content type, metadata와 typed
  payload를 application이 raw frame으로 직접 조립하지 않게 하는 계약, 그리고 cross-node
  framework multipart의 part 순서·length format은 유지해야 한다. zero-copy는 같은 wire bytes와
  검증을 보존하는 내부 변경이어야 한다. 근거:
  `framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md` §2
  “Typed payload envelope”, “Framework multipart application profile”.
- **target의 host-shared permit 및 handler serial ownership** — target fast path가 있어도
  ordinary ingress는 permit을 먼저 얻고 handler queue로 이전해야 한다. permit 없이 receive한
  뒤 별도 counter를 두거나 별도 hidden backlog를 만드는 방식은 허용되지 않는다. 근거:
  `framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md` §3.
- **binding의 DONT_WAIT retry 소유권** — Core queue가 막힐 때 retry와 `admitted` completion은
  binding이 소유한다. Framework가 별도 retry waiter나 재제출을 더하면 안 된다. 근거:
  `framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md` §5.

## 4. 확인 못 한 것

- profiler/allocator trace 없이 각 항목의 exclusive CPU µs·allocation byte와 immediate-admitted
  비율은 확인하지 못했다. 이 기록은 bench를 실행하지 않았다.
- `ZLinkStateLane`을 제거하지 않고 immutable ready handle을 안전하게 publish할 정확한
  linearization point는 lifecycle/close path 전체 검토가 필요하다.
- service multipart의 borrowed-view 최적화가 다른 Java codec extension과 async handler에 적용될
  수 있는지는 각 serializer의 ownership contract를 더 읽어야 한다.
- framework source client의 마지막 `.thenApply(ignored -> (Void) null)`은
  `FrameworkStack.java:81`에 있고 raw source의 OK path에는 대응 변환이 없다. 이는 bench harness의
  추가 future이므로 product 비용과 분리해 한 번 확인할 필요가 있다.
- 다른 언어 구현과의 구조 대조 및 flamegraph는 이번 Java 읽기 전용 조사 범위에서 수행하지 않았다.
