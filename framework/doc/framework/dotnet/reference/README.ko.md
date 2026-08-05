# .NET Framework 레퍼런스

작성 규칙은 [레퍼런스 문서 작성 가이드](../../../../../doc/principal/documentation/reference-writing-guide.ko.md)를
따른다. 이 문서는 그 가이드를 이 트리에 적용한 결과다.

이 문서는 기존 두 문서와 역할이 다르다.

- [Guide 13. 주요 타입 사용 색인](../guide/server/13-interface-catalog.ko.md) — 튜토리얼 관점에서 자주 쓰는
  interface를 소개한다.
- [.NET exact interface](../../common/spec/server/languages/dotnet/interfaces/README.ko.md) — 계약 원문
  소유 문서. interface 전체를 signature 그대로 싣는다.
- **이 레퍼런스** — "이 호출 하나를 완결하려면 무엇을 알아야 하는가"만 모은다. 계약 원문을 복제하지 않고
  exact interface를 인용한다.

## 항목 단위

레퍼런스 항목 하나 = **진입점(entry-point) 메서드 하나.** `RequestToChannel`, `SendToChannel`,
`Publish`처럼 terminal `Async()`/`Async<TReply>()`/`Yield<TReply>()`로 끝나는 awaitable 결과를 만드는
메서드가 항목이 된다.

그 메서드가 반환하는 builder의 `.Timeout(...)`, `.Metadata(...)` 같은 modifier는 별도 항목이 되지
않는다. 항목 안의 "옵션" 표에만 등장한다. Fluent builder를 구성 메서드 단위로 나열하는 문서는 호출자가
실제로 필요로 하는 "이 한 번의 호출이 무엇을 하는지"를 보여주지 못한다 — 이 규칙은 그 실패를 피하기 위한
것이다.

## Category

Public contract 감사 categorization([contract-inventory](../../../contract-inventory/route-mesh-v11-public-contract-trace.json))과
같은 8개 category를 챕터로 쓴다. 언어 사이에 이미 검증된 분류라 새로 만들지 않는다.

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

번호는 이 순서를 그대로 따른다(contract-inventory의 category 순서와 같다).

ko·en 모두 갖췄고 나머지 4개 framework 언어(C++·Java·Kotlin·Node.js)로도 같은 구조를
확장했다. `mkdocs.yml` nav에 올라가 있다.
