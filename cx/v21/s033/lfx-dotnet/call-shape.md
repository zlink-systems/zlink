# .NET ladder call-shape — `exp/4350-dotnet-fixes` (`4bd6102170`)

| 단계 | source send | target receive | target reply | source completion / reply receive | 비교 |
|---|---|---|---|---|---|
| Binding | `framework/bench/grpc/dotnet/Client/Program.cs:697-721,752-789` raw header + protobuf body; `SubmitSendAsync`/`SubmitRequest` | `ZLinkRawServer/Program.cs:64-76` blocking `Recv`, body protobuf decode, existing metrics | `ZLinkRawServer/Program.cs:103-109` 새 protobuf reply body + raw response header, 2-part submit | `Client/Program.cs:652-663,792-890` native completion + raw protobuf decode | 기준 |
| L1 (L2 비교 기준) | binding과 같은 raw client path | `ZLinkServer/Program.cs:254-280` queue permit 후 같은 blocking `Recv` | `ZLinkServer/Program.cs:296-314` 새 protobuf body/header, 2-part submit | binding과 같은 raw completion | queue permit만 추가 |
| L2 | `Client/Program.cs:618-663,752-789` request envelope header + protobuf body | `ZLinkServer/Program.cs:270-280` 수신 뒤 envelope header parse 1회 | `ZLinkServer/Program.cs:343-365` response envelope + 새 protobuf body, 2-part submit | `Client/Program.cs:792-810` response header parse + raw protobuf decode | L1 대비 envelope header encode/parse 추가 |
| L3 | `Client/Program.cs:631-650,700-718` service-wire multipart framing | `ZLinkServer/Program.cs:283-294,375-424` frame unpack + envelope parse + protobuf body | `ZLinkServer/Program.cs:427-457` 새 protobuf reply + envelope/frame pack, 2-part submit | `Client/Program.cs:811-854` reply frame unpack + protobuf decode | L2 대비 service-wire frame pack/unpack 추가 |
| L4 | `Client/Program.cs:752-768` typed JSON envelope/body + frame | `ZLinkServer/Program.cs:375-437` frame unpack + default typed JSON decode | `ZLinkServer/Program.cs:459-477` default typed JSON reply + frame | `Client/Program.cs:811-848` frame unpack + response header/default JSON decode | L3 대비 typed payload serialize/deserialize 추가 |

각 단계에서 receive는 blocking `Recv` 1회/메시지이며 drain 및 기존 target metrics는 동일하다. reply는 매번 새 body를 encode하고 2-part submit한다. L1은 permit acquire/release를 더하고, L2는 envelope encode/parse, L3는 service-wire frame pack/unpack, L4는 typed JSON encode/decode를 추가한다. 같은 프로세스 계측과 CPU pinning은 재사용 runner에서 확인한다.
