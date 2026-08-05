# C++ Framework 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. dotnet 레퍼런스(parity 참조 lane)와 같은 8개 category·순서를 그대로 쓰고, 각 항목은 C++
exact interface를 직접 대조해 작성했다.

- [Guide 13. 주요 타입 사용 색인](../guide/server/13-interface-catalog.ko.md) — 튜토리얼 관점.
- [C++ exact interface](../../common/spec/server/languages/cpp/interfaces/README.ko.md) — 계약 원문
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

**C++ 고유 범위 결정.** C++ guide에는 dotnet 8개 category 밖의 HTTP Hosting(18~21장)이 있다. 이는
dotnet framework의 parity 범위 밖 C++ 전용 확장이므로 이 레퍼런스에는 포함하지 않는다 — HTTP
hosting 레퍼런스가 필요하면 별도 category로 논의한다.

ko·en 모두 갖췄다. `mkdocs.yml` nav에 올라가 있다.
