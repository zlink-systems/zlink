# Node.js Framework 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. dotnet 레퍼런스(parity 참조 lane)와 같은 8개 category·순서를 그대로 쓰고, 각 항목은 Node.js
exact interface(TypeScript declaration)를 직접 대조해 작성했다.

- [Node.js exact interface](../../common/spec/server/languages/node/interfaces/README.ko.md) — 계약
  원문 소유 문서.
- **이 레퍼런스** — "이 호출 하나를 완결하려면 무엇을 알아야 하는가"만 모은다.

## Category

| Category | 상태 | 대응 exact interface |
|---|---|---|
| [Host lifecycle](01-host-lifecycle.ko.md) | 작성 완료 | 03-location-observability §4, 07-nestjs-host |
| [Topology discovery](02-topology-discovery.ko.md) | 작성 완료 | 01-foundation-configuration, 07-nestjs-host, 03-location-observability §5~6 |
| [Messaging execution](03-messaging-execution.ko.md) | 작성 완료 | 02-channel-messaging |
| [Spot instance](04-spot-instance.ko.md) | 작성 완료 | 04-spots, 06-stream-worker, 07-nestjs-host |
| [Actor relocation](05-actor-relocation.ko.md) | 작성 완료 | 05-actors |
| [Stream session](06-stream-session.ko.md) | 작성 완료 | 02-channel-messaging §6, 06-stream-worker |
| [Location authority](07-location-authority.ko.md) | 작성 완료 | 08-location-maintenance, 03-location-observability §2, 07-nestjs-host |
| [Observability diagnostics](08-observability-diagnostics.ko.md) | 작성 완료 | 01-foundation-configuration §3, 03-location-observability §1·§3 |

**Node.js 고유 표기.** Node framework는 `@zlink-systems/framework`(raw builder·client)와
`@zlink-systems/nestjs`(NestJS `DynamicModule`, decorator, DI token)로 나뉜다. 이 레퍼런스는 실제
application이 주로 마주치는 NestJS 표면(`zlinkFramework()` builder, `@zlinkXxxHandler` decorator,
`ZLINK_*` DI token)을 기준으로 서술하고, raw client(`ZLinkRouteClient`, `ZLinkActorManager` 등)는 그
DI token으로 주입받아 그대로 호출한다.

ko·en 모두 갖췄다. `mkdocs.yml` nav에 올라가 있다.
