# .NET ladder L4 호출 형태

| 경로 | binding bench | L4 bench | 차이와 소유 기능 |
|---|---|---|---|
| source send | `Client/Program.cs:510-544,578-610`; raw JSON header + protobuf body를 2-part 제출 | `Client/Program.cs:519-554,588-610,652-670`; production `ZLinkClientCallCodec.EncodeEnvelopeParts`로 typed `BenchPayload`를 인코딩하고 service multipart frame으로 묶어 2-part 제출 | L2 envelope, L3 service framing, L4 typed payload serializer |
| target receive | `ZLinkRawServer/Program.cs:64-76,125-137`; blocking `Recv` 한 번, 마지막 part protobuf decode | `ZLinkServer/Program.cs:218-240,289-358`; 같은 blocking `Recv` 및 L1 permit, L3 service frame decode, L4 `ZLinkEnvelopeCodec.DecodeBody`로 typed `BenchPayload` deserialize | L1 permit, L2 envelope parse, L3 service framing, L4 typed decode |
| target reply | `ZLinkRawServer/Program.cs:103-109`; 새 raw Response header + protobuf body 2-part submit | `ZLinkServer/Program.cs:341-358,370-383`; typed response를 production `PrepareResponse`/`EncodeParts`로 serialize하고 service multipart frame + reply header 2-part submit | L3 service reply framing, L4 typed response serializer |
| source completion / reply receive | `Client/Program.cs:688-745`; reply completion 뒤 마지막 part protobuf decode | `Client/Program.cs:688-740`; service reply correlation/status 확인, multipart frame decode 후 production `ZLinkEnvelopeCodec.DecodeBody`로 typed reply decode | L3 service reply decode, L4 typed reply deserializer |

## production 경로 대조

- Source request/command: `ZLinkRouteClient.cs:149,232,349,439,534` → `ZLinkClientCallCodec.EncodeEnvelopeParts` → `ZLinkEnvelopeCodec.EncodeParts` (`ZLinkClientCallCodec.cs:46-60`, `ZLinkEnvelopeCodec.cs:184-201`). Service framing은 `ZLinkManagedMeshNode.cs:10922-10965`.
- Target receive: service header/frame decode `ZLinkManagedMeshNode.cs:7360-7412`; typed request/command body decode `ZLinkRouteHandlerInvoker.cs:58-63,134-139`.
- Target reply: typed response encode `ZLinkChannelReplyWriter.cs:5-16` → `ZLinkEnvelopeCodec.EncodeParts`; service reply framing `ZLinkManagedMeshNode.cs:12370-12397`.
- Source completion: service reply decode와 payload frame `ZLinkManagedMeshNode.cs:11456-11480,11963-11988`; typed envelope body decode `ZLinkEnvelopeCodec.cs:751-810`.

L4 bench는 bench의 실제 codec 등록(`ZLinkProtobufCodec.Default`)을 사용하며, 직렬화·역직렬화는 Framework typed serializer 경로다. L4 외 receive primitive, drain, metrics와 multipart 수는 binding 기준을 유지한다.
