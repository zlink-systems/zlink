# Java Framework 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. dotnet 레퍼런스(parity 참조 lane)와 같은 8개 category·순서를 그대로 쓰고, 각 항목은 Java
exact interface(`javap` inventory)를 직접 대조해 작성했다.

- [Guide](../guide/server/README.ko.md) — Spring Boot 튜토리얼 관점.
- [Java exact interface](../../common/spec/server/languages/java/interfaces/README.ko.md) — 계약 원문
  소유 문서.
- **이 레퍼런스** — "이 호출 하나를 완결하려면 무엇을 알아야 하는가"만 모은다.

## Category

| Category | 상태 | 대응 spec |
|---|---|---|
| [Host lifecycle](01-host-lifecycle.ko.md) | 작성 완료 | 06-framework-api, 28-graceful-drain-handoff |
| [Topology discovery](02-topology-discovery.ko.md) | 작성 완료 | 07-channel-topology, 09-client-server-channel, 10-network-listener-identity, 21-location-runtime |
| [Messaging execution](03-messaging-execution.ko.md) | 작성 완료 | 04-message-model, 05-async-execution-policy, 08-channel-messaging |
| [Spot instance](04-spot-instance.ko.md) | 작성 완료 | 12-spot-messaging, 15-spot-actor, 16-spot-address-messaging, 17-stage-wrapper-on-spot |
| [Actor relocation](05-actor-relocation.ko.md) | 작성 완료 | 14-actor-model, 15-spot-actor, 20-session-actor-dispatch, 28-graceful-drain-handoff |
| [Stream session](06-stream-session.ko.md) | 작성 완료 | 19-stream-session, 20-session-actor-dispatch |
| [Location authority](07-location-authority.ko.md) | 작성 완료 | 21-location-runtime, 22-location-store-redis, 23-relocation-store-redis, 28-graceful-drain-handoff |
| [Observability diagnostics](08-observability-diagnostics.ko.md) | 작성 완료 | 24-runtime-monitoring, 25-runtime-metrics, 26-message-flow-tracing, 27-flow-correlation, 29-transport-liveness |

**Java 고유 표기.** Java framework는 Spring Boot starter로 배선한다 — 등록 시점 호출은
`@EnableZLinkFramework`와 `ZLinkFrameworkConfigurer` bean, 실행 중 client·runtime 접근은 Spring이
주입하는 bean(`ZLinkFrameworkRuntime`, `ZLinkClient` 등)이다. Kotlin framework는 이 Java 표면을
그대로 공유하며 별도 reference 트리를 두지 않는다(가이드만 Kotlin 고유 idiom을 다룬다).

ko·en 모두 갖췄다. `mkdocs.yml` nav에 올라가 있다.
