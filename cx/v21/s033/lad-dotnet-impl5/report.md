# .NET ladder 구현 보고

측정은 하지 않았다. .NET은 production owner queue 진입이 L6 handler activation/DI scope 및 L7 operation registry/completion과 결합되어 있어 L5~L7을 `L5_7` 한 단계로 묶었다. 네 경로 대조는 [call-shape-L5-7.md](call-shape-L5-7.md)에 기록했다.

| 단계 | 복원 기능 및 코드 경로 | 커밋 | 빌드·기능 확인 | 호출 형태 및 재개 후 비교 |
|---|---|---|---|---|
| L5~L7 (#4355~#4357) | owner mailbox/lane·worker dispatch, dispatch별 handler activation/DI scope, operation registry·deadline·completion. Production: `ZLinkManagedMeshNode.cs:7360-7441,2810-2874,5682-5749,12476-12556`; `ZLinkMeshOwnedMailbox.cs:72-120`; `ZLinkMeshNodeRouteDispatcher.cs:623-675,776-795`; `ZLinkRouteHandlerInvoker.cs:123-160`; `ZLinkHandlerDispatcher.cs:96-103`; `ZLinkRouteClient.cs:293-317,403-473`. Bench: `Client/Program.cs:331-374`; `ZLinkServer/Program.cs:27-37`. | de0507ae48 | `dotnet build WithGrpcBench.sln -c Release -p:ZLinkDotnetLadderLevel=L5_7 -m:1 --no-restore` 성공(0 오류, 25 경고). Client·ZLinkServer·Framework DLL이 관련 소스보다 최신. L5_7 서버 dry start/stop 중 `/ready` 응답 확인, 부하 없음. | 네 경로 차이는 L2~L4 누적 복원과 L5~L7 Framework 경로. 재개 후 L4 대 L5~L7, request-serial·request-backpressure·send-saturation 각 1K·4K의 6셀(묶음 예산 21%) |

`L5_7` 빌드는 L0~L4 raw bench 경로와 함께 컴파일되고 `ZLINK_DOTNET_LADDER_LEVEL`로 선택한다. `L5_7`은 public Framework route client/server 경로를 사용한다. 측정, measure lock, GitHub, push, internal-docs 스크립트는 사용하지 않았다.
