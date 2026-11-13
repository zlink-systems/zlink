# .NET ladder L5~L7 호출 형태

L5만 선택하려면 실제 `ZLinkMeshOwnedMailbox`·owner lane의 dispatch 진입점만 따로 노출해야 한다. 그러나 production request record는 L7 operation ID/완료 경로와 연결되고, 실제 handler dispatch는 L6 DI scope와 handler 활성화를 포함한다. bench 전용 production 진입점은 만들지 않고 실제 `AddZLinkFramework` 경로 전체를 `L5_7` 한 단계로 묶었다.

| 경로 | binding bench | L5~L7 bench | 차이와 복원 기능 |
|---|---|---|---|
| source send | `Client/Program.cs:511-545,579-614` raw envelope/body 구성 후 binding `RawBenchSocket`로 제출 | `Client/Program.cs:331-348,359-374`의 `FrameworkBenchTransport`; `IZLinkRouteClient.RequestToNode`/`SendToNode` public API 사용. Production: `ZLinkRouteClient.cs:66-81,293-317,403-473` | L2 envelope·L3 service framing·L4 typed payload + L5 source owner-turn 진입(E2/E3)·L7 operation 등록/submit |
| target receive | `ZLinkRawServer/Program.cs:64-76,119-137`; blocking binding `Recv` 1회 후 protobuf decode | 서버 공개 등록 `ZLinkServer/Program.cs:27-37`; production `ZLinkManagedMeshNode.cs:7360-7441`에서 service/application record decode 및 `EnqueueOwned`; `:2810-2874` ready owner drain; `ZLinkMeshOwnedMailbox.cs:72-120` owner lane enqueue/claim/drain; `ZLinkMeshNodeRouteDispatcher.cs:623-650` route handler → `ZLinkRouteHandlerInvoker.cs:123-160` → `ZLinkHandlerDispatcher.cs:96-103` | L2 envelope·L3 service framing·L4 typed payload + L5 owner mailbox/lane·worker dispatch + L6 per-dispatch activation/DI scope + L7 request/operation 경로 |
| target reply | `ZLinkRawServer/Program.cs:103-109`; 새 response header와 protobuf body 두 part 제출 | `ZLinkMeshNodeRouteDispatcher.cs:659-675,776-795` production reply callback; `ZLinkChannelReplyWriter.cs:34-57` typed reply header; `ZLinkManagedMeshNode.cs:7444-7455` native reply capability | L3 service reply framing·L4 typed response serialization + L6 활성 handler 반환값 + L7 operation terminal/completion dispatch |
| source completion / reply receive | `Client/Program.cs:545-556,689-745`; binding completion task 후 reply 마지막 part를 protobuf decode | `Client/Program.cs:359-368` Framework request를 await; production `ZLinkRouteClient.cs:456-473` request submit/decode callback, `ZLinkManagedMeshNode.cs:5682-5749` operation registry/deadline 등록 및 dispatcher reserve, `:12476-12556` reply 해석·operation 제거·completion enqueue | L3 service reply decode·L4 typed reply decode + L7 operation registry, deadline registration 및 completion dispatcher |

## production 경로 대조

- Source: `FrameworkBenchTransport`는 벤치에 이미 있던 public route client 구성을 그대로 사용한다. `ZLinkRouteClient.cs:293-317,403-473`이 command/request envelope를 만들고 submit한다.
- Target receive: `ZLinkManagedMeshNode`가 `ZLinkServiceWireCodec.TryDecodeApplication` 뒤 application record를 `EnqueueOwned`로 넘기고, owned mailbox의 FIFO owner turn/claim을 dispatch path가 처리한다.
- Handler/activation: route node dispatcher는 `ZLinkRouteHandlerInvoker`와 `ZLinkHandlerDispatcher`를 사용한다. dispatcher는 요청마다 `CreateAsyncScope` 후 scoped handler를 생성한다.
- Reply/completion: handler 결과는 typed reply writer를 거쳐 native reply capability로 제출된다. source operation registry가 deadline과 completion dispatcher를 소유한다.

기준 binding과 달라진 네 경로의 차이는 모두 실제 L5~L7 Framework dispatch를 복원하면서 생긴다. Binding receive는 blocking `Recv` 한 번이며 별도 drain이 없다. L5~L7은 production owner mailbox claim/drain을 사용한다. 측정 전 receive/reply 비용 parity를 다시 확인해야 한다.
