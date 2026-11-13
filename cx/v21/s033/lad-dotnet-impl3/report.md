# .NET ladder 구현 보고

측정은 하지 않았다. `L4` Release 빌드에는 `L0`–`L4` 런타임 선택 경로를 포함했다. 두 단계의 네 경로 비교는 [call-shape-L3.md](call-shape-L3.md), [call-shape-L4.md](call-shape-L4.md)에 기록했다.

| 단계 | 복원 기능 및 코드 경로 | 커밋 | 검증 및 호출 형태 | 재개 후 비교 |
|---|---|---|---|---|
| L3 (#4353) | service header·multipart frame encode/decode. Production: `ZLinkManagedMeshNode.cs:7360-7380,10922-10965,7446-7472,12370-12397,11456-11480,11963-11988`. Bench: `Client/Program.cs:523-542,591-610,702-740`; `ZLinkServer/Program.cs:230-240,289-358`. | `b8eb5589e7188abb9c198315bcd0cf9c01cb8dc5` | L3 Release 빌드 성공, 산출물이 수정 소스보다 최신. L3 server 시작·`/ready` 200·종료 확인, 메시지 부하는 없음. blocking receive 1회와 outer 2-part 제출 유지. | L2 대 L3: request-serial, request-backpressure, send-saturation 각 1K·4K의 6셀 |
| L4 (#4354) | Framework typed payload serializer로 요청·명령 serialize, target deserialize, response serialize, source deserialize. Production: `ZLinkClientCallCodec.cs:46-60`, `ZLinkRouteHandlerInvoker.cs:58-63,134-139`, `ZLinkChannelReplyWriter.cs:5-16`, `ZLinkEnvelopeCodec.cs:751-810`. Service wire는 L3 경로 유지. Bench: `Client/Program.cs:519-554,643-670,702-740`; `ZLinkServer/Program.cs:230-240,289-383`. | `a7fd48d1b1be6c4d64796e81cdf94db5da494f71` | L4 Release 빌드 성공, 산출물이 수정 소스보다 최신. L4 server 시작·`/ready` 200·종료 확인, 메시지 부하는 없음. typed serialization은 bench의 production 설정인 `ZLinkProtobufCodec.Default`를 사용. | L3 대 L4: request-serial, request-backpressure, send-saturation 각 1K·4K의 6셀 |

Build 명령은 `dotnet build WithGrpcBench.sln -c Release -p:ZLinkDotnetLadderLevel=L3 -m:1 --no-restore` 및 동일한 `L4` 명령이었다. 초기 병렬 빌드는 다른 dotnet 빌드와 산출물 `.deps.json` 쓰기가 겹쳐 실패했으며, 직렬 재빌드로 해결했다. 기존 MSBuild 중복 import 경고가 출력됐고 최종 두 빌드는 오류 없이 완료됐다. unit/contract test는 실행하지 않았다. `measure.lock`, benchmark, GitHub, push는 사용하지 않았다.
