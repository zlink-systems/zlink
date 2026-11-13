# .NET ladder 구현 보고

측정은 하지 않았다. L5 owner dispatch 진입이 L6 activation/DI scope와 L7 operation registry/completion 경로에 결합되어 있어 세 단계를 `L5_7` 한 커밋으로 복원한다. 네 경로 대조는 [call-shape-L5-7.md](call-shape-L5-7.md)에 기록했다.

| 단계 | 복원 기능 및 코드 경로 | 커밋 | 빌드·기능 확인 | 호출 형태 및 재개 후 비교 |
|---|---|---|---|---|
| L5~L7 (#4355~#4357) | owner mailbox/lane·worker dispatch, dispatch별 handler activation/DI scope, operation registry·deadline·completion. Production: `ZLinkManagedMeshNode.cs:7360-7441,5682-5749,12476-12556`; `ZLinkMeshOwnedMailbox.cs:72-120`; `ZLinkMeshNodeRouteDispatcher.cs:220-265`; `ZLinkHandlerDispatcher.cs:86-103`; `ZLinkChannelRequestDispatchPipeline.cs:115-143`; `ZLinkRouteClient.cs:293-317,403-473`. Bench: `Client/Program.cs:331-374`; `ZLinkServer/Program.cs:27-37`. | 빌드·dry start/stop 뒤 기록 | Release solution build 및 `/ready` 확인 대기 중 | 네 경로의 차이는 모두 Framework L5~L7 호출 경로. 재개 후 L4 대 L5~L7, request-serial·request-backpressure·send-saturation 각 1K·4K의 6셀(21% 묶음 예산) |

`L5_7` 빌드는 L0~L4 raw bench 경로와 함께 컴파일되며 `ZLINK_DOTNET_LADDER_LEVEL`로 선택한다. Framework client/server 경로는 `L5_7`에서 사용한다. 측정, measure lock, GitHub, push, internal-docs 스크립트는 사용하지 않았다.
