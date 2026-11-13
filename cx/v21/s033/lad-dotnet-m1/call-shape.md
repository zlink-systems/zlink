# .NET ladder L0/L1/L2 call shape

L0 요청·command header는 raw binding과 byte 단위로 같다. L1은 target receive 앞 permit 획득과 handler 직전 반환을 추가한다. L2는 L1 위에 production envelope header encode/decode를 추가한다. 같은 L1 빌드에서 `ZLINK_DOTNET_LADDER_LEVEL=L0|L1|L2`로 server path를 고르고, L2에서만 source와 target이 envelope header를 처리한다.

| 단계 | Binding call (`file:line`) | L0 call (`file:line`) | L0 같은가 | L1 call (`file:line`) | L1 같은가 / 복원 기능 | L2 call (`file:line`) | L2 같은가 / 복원 기능 |
|---|---|---|---|---|---|---|---|
| Source send — request/command header + protobuf body submit | `Client/Program.cs:491-507,530-544`; raw header `Shared/RawEnvelopeHeaders.cs:5-16` | `Client/Program.cs:436-453,494-510,533-547` | 예: raw Request/Command bytes와 2-part submit 동일 | 같은 raw header call | 예: L0와 동일 | `Client/Program.cs:436-453,494-510,533-547`; production header creator `Runtime/Messaging/ZLinkClientCallCodec.cs:6-44`, encoder `Runtime/Messaging/ZLinkEnvelopeCodec.cs:183-243` | 아니오—L2 envelope header encode |
| Target receive — wait/parse | `ZLinkRawServer/Program.cs:64-76,119-137`; timeout `:26-28` | `ZLinkServer/Program.cs:48-51,140-159` | 예: blocking `Recv(None)` 한 건 | `ZLinkServer/Program.cs:48-51,196-217` | 아니오—receive 전 permit acquire와 handler 전 반환 | L1 receive + `ZLinkServer/Program.cs:200-203`; production decoder `Runtime/Messaging/ZLinkEnvelopeCodec.cs:374-385` | 아니오—L1 permit + header `DecodeHeader` 1회 |
| Target reply — body/header creation + submit | `ZLinkRawServer/Program.cs:74-85,103-109` | `ZLinkServer/Program.cs:148-159,298-305` | 예: 새 protobuf body, raw Response header, 2 parts | `ZLinkServer/Program.cs:205-217,298-305` | 예: 같은 reply; permit 반환은 handler 직전 | `ZLinkServer/Program.cs:150-164,262-281` | 아니오—request correlation을 보존한 production Response envelope 생성 및 2-part submit |
| Source completion / reply receive | `Client/Program.cs:491-510,589-612` | 같은 공통 raw client 경로 | 예: completion/reply decode 동일 | 같은 raw client 경로 | 예: L0와 동일 | `Client/Program.cs:592-611` | 아니오—reply part 0의 production envelope header parse 1회 후 protobuf body decode |

## Per-message waits / allocations / atomics / copies

- **공통 source:** 헤더 `Message` 1개와 protobuf body `Message` 1개를 만들고 protobuf body를 한 번 쓴다. Request는 submit completion/reply를 기다리고 마지막 reply part를 decode한 뒤 응답 parts와 요청 Message를 dispose한다. Command는 기존 submit/admission 경로를 따른다.
- **공통 target/reply:** blocking `Recv(None)` 한 번으로 record 하나를 받고 drain하지 않는다. protobuf body를 한 번 parse하고 같은 metrics를 갱신한다. Request는 새 reply payload, protobuf response body와 raw Response header를 만들어 2-part submit한다. 요청 body 재사용이나 copy는 없다.
- **L0:** binding과 같은 per-message waits, allocations, atomics, copies다. L0 handler를 직접 호출한다.
- **L1:** receive 전에 permit을 acquire하고 handler 진입 직전에 반환한다. 메시지마다 queue/lease 상태 처리와 lease 객체, lease `Interlocked.Exchange` 1회가 추가된다.
- **L2:** source request와 target reply에서 production Framework envelope header를 encode하고, target request와 source completion에서 `DecodeHeader`를 각 1회 호출한다. Reply header는 request correlation ID를 보존한다. protobuf body part와 encode/decode/copy 수는 L1과 같다.

L2 source는 production `ZLinkClientCallCodec.CreateEnvelope`를 써서 요청 header를 만들고 `ZLinkEnvelopeCodec.EncodeHeader`로 encode한다. Target request parse는 `ZLinkEnvelopeCodec.DecodeHeader(Message)`를 호출한다. `AssemblyInfo.cs`의 L1/L2 전용 friend 접근은 벤치 assembly가 이 internal production 경로를 호출하도록 한다.

## L2 reply-side rows

| 단계 | Binding call (`file:line`) | L2 bench call (`file:line`) | Production envelope path (`file:line`) | 복원 |
|---|---|---|---|---|
| Target reply — response header + submit | `ZLinkRawServer/Program.cs:74-85,103-109` | `ZLinkServer/Program.cs:150-164,262-281` | `Runtime/Channels/ZLinkChannelRequestDispatchPipeline.cs:132-143` → `Runtime/Channels/ZLinkChannelReplyWriter.cs:5-16,34-57` → `Runtime/Messaging/ZLinkEnvelopeCodec.cs:151-167` (raw body: `175-181`) | request correlation을 보존한 response envelope encode 1회, body와 함께 2 parts submit |
| Source completion — reply envelope parse | `Client/Program.cs:491-510,589-612` | `Client/Program.cs:592-611` | `Runtime/Channels/ZLinkRouteClient.cs:232-254` → `Runtime/Messaging/ZLinkClientCallCodec.cs:77-97` → `Runtime/Messaging/ZLinkEnvelopeCodec.cs:374-375` | response header parse 1회 후 같은 protobuf body decode |
