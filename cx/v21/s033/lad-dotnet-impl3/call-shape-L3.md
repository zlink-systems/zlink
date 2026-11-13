# .NET ladder L3 호출 형태

| 경로 | binding bench | L3 bench | 차이와 소유 기능 |
|---|---|---|---|
| source send | `Client/Program.cs:511-535`에서 raw JSON header + protobuf body를 `RawBenchSocket.SubmitRequest`로 2-part 제출; send는 `:573-596` | 같은 transport의 `:511-539,514-533`에서 production envelope header + protobuf body를 service multipart frame으로 묶고 service header와 frame을 2-part 제출; send는 `:573-596,575-593` | L2 envelope header encode; L3 service header와 multipart frame. transport/request submit 자체는 동일 |
| target receive | `ZLinkRawServer/Program.cs:64-76,125-137`, blocking `Recv` 한 번, 마지막 part의 protobuf decode | `ZLinkServer/Program.cs:199-217,262-310`, blocking `Recv` 한 번; permit 획득·handler 진입 전 반환 후 service header 및 multipart frame decode, inner envelope header decode | L1 application permit; L2 envelope parse; L3 service wire frame parse. drain 없음 |
| target reply | `ZLinkRawServer/Program.cs:103-109`, 새 raw Response header + protobuf body를 2-part 제출 | `ZLinkServer/Program.cs:294-309`, 새 raw Response header + protobuf body를 multipart frame에 담고 service reply header와 frame을 2-part 제출 | L3 reply wire header와 multipart frame |
| source completion / reply receive | `Client/Program.cs:642-693`, Core completion 이후 마지막 reply part의 protobuf decode | `Client/Program.cs:642-693,655-686`, service reply correlation/status 확인, multipart frame decode 후 마지막 part의 protobuf decode | L3 service reply와 multipart frame decode. L2 envelope response parsing은 이전 동작과 같이 없음 |

## production 경로 대조

- Source envelope encode: `ZLinkRouteClient.cs:149,232,349,439,534`; service header와 multipart wire 작성: `ZLinkManagedMeshNode.cs:10922-10965`.
- Target receive: `ZLinkManagedMeshNode.cs:7360-7412`에서 application service header와 multipart frame을 decode한다.
- Target reply: `ZLinkManagedMeshNode.cs:7446-7472,12370-12397`에서 service reply header와 multipart frame을 만들어 Core reply token으로 제출한다.
- Source completion: `ZLinkManagedMeshNode.cs:11456-11480,11963-11988`에서 reply correlation/status 및 multipart payload를 decode한다.

L3 경로는 같은 production codec(`ZLinkServiceWireCodec`, `ZLinkApplicationPayloadEnvelopeCodec`)을 사용한다. L3 외의 receive primitive, request token, reply submit part 수는 binding과 같다. L1 permit은 L1 복원 비용으로 남고, L3 decode는 이를 handler 진입 직전에 반환한다.
