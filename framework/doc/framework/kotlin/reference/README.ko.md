# Kotlin Framework 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. dotnet 레퍼런스(parity 참조 lane)와 같은 8개 category·순서를 그대로 쓴다.

`zlink-framework-kotlin`은 Java `zlink-framework` runtime을 그대로 재사용하는 얇은 coroutine idiom
레이어다([Kotlin 문서 §0](../README.ko.md#0-kotlin-표면-한눈에)). 그래서 이 레퍼런스는 Java와
**다른 모양으로 호출하는 진입점만** 문서화한다 — Java builder를 그대로 호출하는 구성 항목(topology
등록, Store 등록, `configureLocations()` 등)은 새로 만들지 않고
[Java 레퍼런스](../../java/reference/README.ko.md)의 해당 항목을 그대로 가리킨다. 완료 kind,
기본값, 범위 같은 계약 내용은 전부 Java와 같으며 이 문서에서 반복하지 않는다.

정확한 signature는
[Kotlin exact interface](../../common/spec/server/languages/kotlin/interfaces/README.ko.md)가
소유한다.

## Category

| Category | 상태 | Kotlin 고유 진입점 |
|---|---|---|
| [Host lifecycle](01-host-lifecycle.ko.md) | 작성 완료 | `CompletionStage<T>.await()` 브리지 |
| [Topology discovery](02-topology-discovery.ko.md) | 작성 완료 | `routeMesh { }`/`channel { }` DSL, `useCoroutineHandlers(...)` |
| [Messaging execution](03-messaging-execution.ko.md) | 작성 완료 | `ZLinkKotlinClient`/`ZLinkKotlinRouteClient`/`ZLinkKotlinFanoutClient`의 `await()`/`yield()` |
| [Spot instance](04-spot-instance.ko.md) | 작성 완료 | `ZLinkSuspendingSpot`/`ZLinkSuspendingEntrySpot`/`ZLinkSuspendingInstanceSpot`, `ZLinkKotlinSpotManager` |
| [Actor relocation](05-actor-relocation.ko.md) | 작성 완료 | `ZLinkSuspendingActor`/`ZLinkSuspendingActorFactory`, `ZLinkKotlinActorManager`/`ZLinkKotlinActorClient` |
| [Stream session](06-stream-session.ko.md) | 작성 완료 | `ZLinkSuspendingSession`, `ZLinkKotlinSessionClient`, `bindOrGetActor(...)` |
| [Location authority](07-location-authority.ko.md) | 작성 완료 | `suspend fun status()`/`listTopology(...)`, `Flow<T>` projection |
| [Observability diagnostics](08-observability-diagnostics.ko.md) | 작성 완료 | `onMessageFlow { }`, Java `ZLinkFrameworkErrorKind` 재사용 |

ko·en 모두 갖췄다. `mkdocs.yml` nav에 올라가 있다.
