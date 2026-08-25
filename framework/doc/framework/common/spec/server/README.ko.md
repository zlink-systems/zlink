# Framework 공통 스펙

이 디렉터리의 문서는 Framework의 공통 공개 계약을 설명한다. 각 문서는 구현과
contract test에 필요한 입력, 상태, 정상 흐름, 실패와 완료 조건을 자체적으로
정의한다.

이 디렉터리와 언어별 interface가 Framework 공개 계약의 단일 기준이다.
이 디렉터리의 문서는 두 층으로 나뉜다(아래 주제별 목차의 "층" 열). **계약** 층은
Application이 관찰하는 동작을 정의하고, **구현 스펙** 층은 모든 언어의 service runtime이 그
계약을 같은 결과로 제공하기 위해 공통으로 따르는 구조 결정을 정의한다. 한 문서가 두 층을 함께
담을 수 있으며, 그때는 문장마다 계약인지 구현 스펙인지 밝힌다. 구현 스펙은 새 공개 동작을
추가하지 않지만 runtime이 따라야 하는 규범이다 — 각 결정을 어기면 Application이 보는 결과가
달라진다. 두 층이 충돌하면 그 충돌은 결함이다. 계약을 기준으로 구현 스펙을 고치고, 계약
자체를 바꿔야 하면
[공개 계약 절차](00-foundation/01-public-contract-governance.ko.md#5-공개-계약-절차)를
먼저 따른다.

같은 계약을 구현한 여러 언어의 sample과 E2E를 한 host에서 동시에 검증하는 runner 격리
규칙은 이 스펙이 아니라 검증 환경이 소유한다 —
[sample runner 격리 기준](../../sample/README.ko.md#샘플-실행-스크립트와-redis-격리-기준),
[E2E runner 실행 계약](../../e2e/README.ko.md#27-run_e2e-실행-계약) 참고.

## 이 스펙이 답하는 것

| 주제 | 독자의 질문 한 줄 | 진입 문서 |
|---|---|---|
| foundation | 이 스펙 전체가 어떤 규칙으로 쓰였고, 공통으로 쓰는 용어와 API 등록 방법은 무엇인가 | [00-foundation/README.ko.md](00-foundation/README.ko.md) |
| execution | handler는 언제 어떤 순서로 실행되고, 완료·취소·동시성은 어떤 구조로 보장되는가 | [01-execution/README.ko.md](01-execution/README.ko.md) |
| channel-transport | MeshNode 사이 물리 연결과 Channel로 메시지를 보내는 경로는 어떻게 구성되는가 | [02-channel-transport/README.ko.md](02-channel-transport/README.ko.md) |
| spot-actor | Spot과 Actor는 무엇이고, 메시지가 그 위치까지 도달하는 경로는 무엇인가 | [03-spot-actor/README.ko.md](03-spot-actor/README.ko.md) |
| session | 외부 연결 하나(session)는 Actor와 어떻게 연결되고, 끊기거나 이동할 때 무엇이 보장되는가 | [04-session/README.ko.md](04-session/README.ko.md) |
| location-relocation | Actor·Spot의 현재 위치는 어떻게 찾고, 다른 node로 옮길 때 무엇이 유지되는가 | [05-location-relocation/README.ko.md](05-location-relocation/README.ko.md) |
| observability | 운영자는 Framework의 현재 상태와 실패 원인을 무엇으로 확인하는가 | [06-observability/README.ko.md](06-observability/README.ko.md) |

## 읽는 순서

**처음 읽는 독자** (이 spec 전체가 처음인 경우)

1. foundation
2. channel-transport
3. spot-actor
4. session
5. location-relocation
6. observability
7. execution — 필요할 때만, 구현 세부를 확인하려는 경우

**새 언어 porting 담당자** (service runtime을 새로 구현하는 경우)

1. foundation
2. execution
3. channel-transport
4. spot-actor
5. session
6. location-relocation
7. observability

**application 개발자** (기존 언어 binding으로 Framework를 사용하는 경우)

1. foundation
2. channel-transport
3. spot-actor
4. session
5. observability
6. location-relocation — Host relocation을 직접 호출하는 경우만
7. execution — 보통 읽지 않는다. 구현 세부는 계약에 이미 반영되어 있다

## 주제별 목차

### 00-foundation

Framework 전체가 공유하는 계약 소유권 규칙, 용어, 상위 모델, 상호작용 대상과 완료 의미,
메시지·응답·오류 형태, 언어 중립 등록 API와 runtime 계층 경계를 담는다. 다른 모든 주제가
이 주제의 용어와 규칙을 전제로 한다.

| 문서 | 답하는 질문 | 층 |
|---|---|---|
| [01. public-contract-governance](00-foundation/01-public-contract-governance.ko.md) | Framework 공개 계약을 바꾸려면 어떤 절차를 거쳐야 하는가 | 계약 |
| [02. glossary](00-foundation/02-glossary.ko.md) | 이 스펙 전체에서 반복해서 나오는 용어는 정확히 무엇을 뜻하는가 | 계약 |
| [03. overview](00-foundation/03-overview.ko.md) | Framework는 무엇을 하는 계층이고, 언어마다 무엇을 따로 구현하는가 | 계약 |
| [04. interaction-model](00-foundation/04-interaction-model.ko.md) | Framework operation은 무엇을 대상으로 삼고, 언제 완료된 것으로 보는가 | 계약 |
| [05. message-model](00-foundation/05-message-model.ko.md) | 보낸 메시지와 그 응답·오류는 어떤 형태와 규칙을 따르는가 | 계약 |
| [06. framework-api](00-foundation/06-framework-api.ko.md) | Application은 root에 무엇을 등록해야 Framework를 시작할 수 있는가 | 계약 |
| [07. framework-error-model](00-foundation/07-framework-error-model.ko.md) | Send·Request가 실패하면 Application은 어떤 공통 오류를 받는가 | 계약 |
| [08. layering](00-foundation/08-layering.ko.md) | runtime 코드는 어떤 덩어리로 나뉘고, 어떤 값을 하나로 합치면 안 되는가 | 구현 스펙 |

### 01-execution

submit부터 handler 실행, 완료, 취소, 실행 직렬화, payload 소유권까지 — 하나의 호출이
받아들여진 뒤 handler에 도달해 끝나는 전체 실행 경로를 다룬다. 대부분 언어별 service
runtime이 공통으로 지켜야 하는 구현 스펙이다.

| 문서 | 답하는 질문 | 층 |
|---|---|---|
| [01. submit-and-completion](01-execution/01-submit-and-completion.ko.md) | 호출은 언제 수락되고 무엇이 그 호출을 완료시키는가 | 계약+구현 |
| [02. handler-turn-and-execution-gate](01-execution/02-handler-turn-and-execution-gate.ko.md) | handler에 동기화 코드가 없어도 상태가 안전한 이유는 무엇인가 | 계약+구현 |
| [03. cancellation-and-shutdown](01-execution/03-cancellation-and-shutdown.ko.md) | 취소와 종료는 이미 수락한 작업을 어떻게 처리하는가 | 계약 |
| [10. spot-timer](03-spot-actor/10-spot-timer.ko.md) | Spot timer는 언제 실행되고 늦은 tick은 어떻게 되는가 | 계약+구현 |
| [04. application-job-queue-and-backpressure](01-execution/04-application-job-queue-and-backpressure.ko.md) | 과부하일 때 무엇이 먼저 막히고 Application은 무엇을 관찰하는가 | 계약+구현 |
| [05. payload-ownership-and-codec](01-execution/05-payload-ownership-and-codec.ko.md) | message는 socket에서 handler까지 byte를 몇 번 복사하는가 | 계약+구현 |

session 주제에서 이관된 shared permit 규칙은 `05`의 "Ordinary ingress permit 순서" 절이
하나의 계약 문장으로 소유한다.

### 02-channel-transport

물리 연결(RouteMesh, ClientServer, listener identity)과 그 위에서 Node direct·Channel
select-one으로 대상을 고르는 방법, 연결 생존 확인과 wire 상의 byte·command 형식을
다룬다.

| 문서 | 답하는 질문 | 층 |
|---|---|---|
| [01. channel-topology](02-channel-transport/01-channel-topology.ko.md) | RouteMesh의 물리 연결과 ChannelName 논리 membership은 어떻게 구성하는가 | 계약 |
| [02. channel-messaging](02-channel-transport/02-channel-messaging.ko.md) | Node direct와 ChannelName select-one은 각각 대상을 어떻게 고르는가 | 계약 |
| [03. client-server-channel](02-channel-transport/03-client-server-channel.ko.md) | Client가 시작한 request에 Server는 어떻게 handler로 응답하는가 | 계약 |
| [04. network-listener-identity](02-channel-transport/04-network-listener-identity.ko.md) | listener의 bind 주소와 advertised 주소는 왜 다르고, 언제 각각 쓰이는가 | 계약 |
| [05. transport-liveness](02-channel-transport/05-transport-liveness.ko.md) | remote connection이 살아 있는지 어떻게 확인하고, 끊기면 어떻게 다시 잇는가 | 계약+구현 |
| [06. wire-protocol](02-channel-transport/06-wire-protocol.ko.md) | node 사이에 실제로 어떤 byte와 command가 오가는가 | 구현 스펙 |

### 03-spot-actor

Spot 세 종류(Entry·User·Instance)와 Actor의 identity·membership·relocation, 그 위에
Message가 도달하는 두 경로(Spot direct, Logical Multicast)와 언제 Location Store를
다시 조회하는지를 다룬다.

| 문서 | 답하는 질문 | 층 |
|---|---|---|
| [01. spot-model](03-spot-actor/01-spot-model.ko.md) | Entry·User·Instance Spot은 언제 만들어지고, 무엇이 같고 무엇이 다른가 | 계약 |
| [02. spot-messaging](03-spot-actor/02-spot-messaging.ko.md) | Spot으로 보낸 메시지는 어떤 경로로 실제 Spot까지 전달되는가 | 계약 |
| [03. mesh-node](03-spot-actor/03-mesh-node.ko.md) | MeshNode의 identity와 object 배치 조건, startup 순서는 무엇인가 | 계약 |
| [04. actor-model](03-spot-actor/04-actor-model.ko.md) | Actor의 identity, 위치, message queue와 lifecycle은 어떻게 정의되는가 | 계약 |
| [05. spot-actor-membership](03-spot-actor/05-spot-actor-membership.ko.md) | Actor는 어떻게 생성되고, Spot membership과 relocation은 어떤 순서로 이루어지는가 | 계약 |
| [06. spot-address-messaging](03-spot-actor/06-spot-address-messaging.ko.md) | global SpotId는 어떻게 만들고 조회하며, 그 Spot을 직접 호출하는가 | 계약 |
| [07. stage-wrapper-on-spot](03-spot-actor/07-stage-wrapper-on-spot.ko.md) | Spot 계약 위에 room·stage 같은 상위 실행 모델을 어떻게 만드는가 | 계약 |
| [08. routing](03-spot-actor/08-routing.ko.md) | Spot·Actor로 가는 message는 언제 위치를 다시 조회하고 언제 조회하지 않는가 | 계약+구현 |
| [09. object-lifecycle](03-spot-actor/09-object-lifecycle.ko.md) | Spot 세 종류를 코드에서 어떻게 구분하고, 없는 객체를 언제 만드는가 | 구현 스펙 |

### 04-session

STREAM 연결 하나(session)의 등록·수락·codec·오류 경계와, 그 연결을 Actor에 잇는 binding·
rebind·disconnect·relocation 중 Session의 책임을 다룬다.

| 문서 | 답하는 질문 | 층 |
|---|---|---|
| [01. stream-session](04-session/01-stream-session.ko.md) | 연결 하나가 수락된 뒤 packet은 어떤 경로로 callback까지 오는가 | 계약 |
| [02. session-actor-binding](04-session/02-session-actor-binding.ko.md) | Session과 Actor는 어떻게 연결되고, 연결 교체·이동 중에는 무엇이 보장되는가 | 계약+구현 |

### 05-location-relocation

Actor·Spot의 현재 위치를 찾는 방법(Location Store), relocation 뒤 완료되는 request를
복구하는 방법(Relocation Store), 계획된 이동(Host relocation, Actor Join 등)의 공통
순서와 자동 failover의 경계를 다룬다.

| 문서 | 답하는 질문 | 층 |
|---|---|---|
| [01. location-runtime](05-location-relocation/01-location-runtime.ko.md) | Framework는 object의 현재 위치를 어떻게 찾고 다른 node로 옮기는가 | 계약 |
| [02. location-store-redis](05-location-relocation/02-location-store-redis.ko.md) | Location Store를 직접 구현하려면 무엇을 보장해야 하는가 | 계약 |
| [03. relocation-store-redis](05-location-relocation/03-relocation-store-redis.ko.md) | relocation 관련 payload를 직접 저장하려면 무엇을 보장해야 하는가 | 계약 |
| [04. relocation-flow](05-location-relocation/04-relocation-flow.ko.md) | Actor·Spot을 다른 node로 옮기는 동안 owner와 message는 어떤 순서로 바뀌는가 | 계약+구현 |
| [05. host-relocation-flow](05-location-relocation/05-host-relocation-flow.ko.md) | Host `Relocate`는 workload를 어떤 순서로 옮기고 `Shutdown`은 무엇을 정리하는가 | 계약 |
| [06. failure-failover-policy](05-location-relocation/06-failure-failover-policy.ko.md) | 장애가 났을 때 Framework는 같은 작업을 자동으로 어디까지 계속하는가 | 계약 |

### 06-observability

운영자가 현재 상태를 조회하고, 시간에 따른 수치를 집계하며, message 한 건의 진행과
여러 message로 이어진 하나의 업무 흐름을 추적하는 방법을 다룬다. 간헐 실패를 쫓는 순서는
[README 「4. 간헐 실패를 쫓는 순서」](06-observability/README.ko.md#4-간헐-실패를-쫓는-순서)가,
tracing을 켠 채로 두는 비용 규칙은
[03. message-flow-tracing 「5. 실행 중 기록 수준 변경과 비용 규칙」](06-observability/03-message-flow-tracing.ko.md#5-실행-중-기록-수준-변경과-비용-규칙)이
정의한다.

| 문서 | 답하는 질문 | 층 |
|---|---|---|
| [01. runtime-monitoring](06-observability/01-runtime-monitoring.ko.md) | 운영자는 Framework runtime의 현재 상태를 어떻게 조회하고 원인을 log에서 찾는가 | 계약 |
| [02. runtime-metrics](06-observability/02-runtime-metrics.ko.md) | 처리량·대기·실패를 나타내는 metric의 이름과 단위, label은 무엇인가 | 계약 |
| [03. message-flow-tracing](06-observability/03-message-flow-tracing.ko.md) | message 한 건이 어느 단계까지 왔고 어디서 실패했는지 어떻게 확인하는가 | 계약 |
| [04. flow-correlation](06-observability/04-flow-correlation.ko.md) | request와 reply, 여러 message로 이어진 하나의 흐름을 어떻게 식별하는가 | 계약 |

## 언어별 interface

공통 server 계약이 각 언어에서 사용하는 정확한 public type, signature와 비동기
표현은 다음 문서가 소유한다.

- [C++](languages/cpp/README.ko.md)
- [.NET](languages/dotnet/README.ko.md)
- [Java](languages/java/README.ko.md)
- [Kotlin](languages/kotlin/README.ko.md)
- [Node.js](languages/node/README.ko.md)

## HTTP client

- [HTTP client 스펙 목차](../http-client/README.ko.md)
- [12 HTTP client 통합 계약](../http-client/12-http-client.ko.md)
- [언어별 HTTP client 계약](../http-client/language-interfaces.ko.md)

## Stream connector

- [32 Stream connector](../stream-connector/32-stream-connector.ko.md)
- [언어별 Stream connector 계약](../stream-connector/README.ko.md#언어별-public-api)

## 인용 표기

인용은 **절 제목**으로 한다. 링크를 누르면 그 절로 바로 이동한다.

```markdown
[Actor 모델 「3. Actor queue」](03-spot-actor/04-actor-model.ko.md#3-actor-queue)
```

**줄 번호로 인용하지 않는다.** `§123` 형태는 문서 맨 위로만 이동해 독자가 그 자리를 다시
찾아야 하고, 인용한 문서가 한 줄만 바뀌어도 가리키는 곳이 틀어진다. 절 제목은 그 절이
사라지거나 이름이 바뀔 때만 깨지며, 그때는 링크 검사에서 드러난다.

anchor는 제목을 소문자로 바꾸고 공백을 `-`로 이은 값이다. 확인은 다음으로 한다.

```bash
mkdocs build --strict   # doc/site에서 실행
```

## 옛 문서가 어디로 갔는가

이 스펙은 전역 번호(`00`~`52`) 하나에 문서 하나씩 붙던 옛 구성을 주제 디렉터리로
재구성했다. 옛 번호로 문서를 찾던 링크나 기억은 아래 표로 새 위치를 찾는다. 한 옛 문서가
여러 새 문서로 나뉜 경우 절 범위를 함께 적는다.

옛 문서 자체는 지우지 않고 [`archive/`](archive/)에 보관한다. 규범 계약은 아래 표의 새
문서가 소유하며, `archive/`의 문서는 이력을 확인할 때만 본다.

| 옛 문서 | 새 위치 |
|---|---|
| `00-public-contract-governance` | [00-foundation/01-public-contract-governance](00-foundation/01-public-contract-governance.ko.md) |
| `01-glossary` | [00-foundation/02-glossary](00-foundation/02-glossary.ko.md) |
| `02-overview` | [00-foundation/03-overview](00-foundation/03-overview.ko.md) |
| `03-interaction-model` | [00-foundation/04-interaction-model](00-foundation/04-interaction-model.ko.md) |
| `04-message-model` | [00-foundation/05-message-model](00-foundation/05-message-model.ko.md) |
| `05-async-execution-policy` | §1.1~§1.4·§2·§6 → [01-execution/01-submit-and-completion](01-execution/01-submit-and-completion.ko.md) · §1.1(Yield)·§3·§3.1 → [02-handler-turn-and-execution-gate](01-execution/02-handler-turn-and-execution-gate.ko.md) · §4 → [03-cancellation-and-shutdown](01-execution/03-cancellation-and-shutdown.ko.md) · §5 → [03-spot-actor/10-spot-timer](03-spot-actor/10-spot-timer.ko.md) · §10 → [01-execution/04-application-job-queue-and-backpressure](01-execution/04-application-job-queue-and-backpressure.ko.md) |
| `06-framework-api` | [00-foundation/06-framework-api](00-foundation/06-framework-api.ko.md) |
| `07-channel-topology` | [02-channel-transport/01-channel-topology](02-channel-transport/01-channel-topology.ko.md) |
| `08-channel-messaging` | [02-channel-transport/02-channel-messaging](02-channel-transport/02-channel-messaging.ko.md) |
| `09-client-server-channel` | [02-channel-transport/03-client-server-channel](02-channel-transport/03-client-server-channel.ko.md) |
| `10-network-listener-identity` | [02-channel-transport/04-network-listener-identity](02-channel-transport/04-network-listener-identity.ko.md) |
| `11-spot-model` | [03-spot-actor/01-spot-model](03-spot-actor/01-spot-model.ko.md) |
| `12-spot-messaging` | [03-spot-actor/02-spot-messaging](03-spot-actor/02-spot-messaging.ko.md) |
| `13-mesh-node` | [03-spot-actor/03-mesh-node](03-spot-actor/03-mesh-node.ko.md) |
| `14-actor-model` | [03-spot-actor/04-actor-model](03-spot-actor/04-actor-model.ko.md) |
| `15-spot-actor` | [03-spot-actor/05-spot-actor-membership](03-spot-actor/05-spot-actor-membership.ko.md) |
| `16-spot-address-messaging` | [03-spot-actor/06-spot-address-messaging](03-spot-actor/06-spot-address-messaging.ko.md) |
| `17-stage-wrapper-on-spot` | [03-spot-actor/07-stage-wrapper-on-spot](03-spot-actor/07-stage-wrapper-on-spot.ko.md) |
| `18-object-routing` | [03-spot-actor/08-routing](03-spot-actor/08-routing.ko.md) |
| `19-stream-session` | [session/01-stream-session](04-session/01-stream-session.ko.md) |
| `20-session-actor-dispatch` | [session/02-session-actor-binding](04-session/02-session-actor-binding.ko.md) |
| `21-location-runtime` | [05-location-relocation/01-location-runtime](05-location-relocation/01-location-runtime.ko.md) |
| `22-location-store-redis` | [05-location-relocation/02-location-store-redis](05-location-relocation/02-location-store-redis.ko.md) |
| `23-relocation-store-redis` | [05-location-relocation/03-relocation-store-redis](05-location-relocation/03-relocation-store-redis.ko.md) |
| `24-runtime-monitoring` | [06-observability/01-runtime-monitoring](06-observability/01-runtime-monitoring.ko.md) |
| `25-runtime-metrics` | [06-observability/02-runtime-metrics](06-observability/02-runtime-metrics.ko.md) |
| `26-message-flow-tracing` | [06-observability/03-message-flow-tracing](06-observability/03-message-flow-tracing.ko.md) |
| `27-flow-correlation` | [06-observability/04-flow-correlation](06-observability/04-flow-correlation.ko.md) |
| `28-relocation-flow` | [05-location-relocation/04-relocation-flow](05-location-relocation/04-relocation-flow.ko.md) |
| `29-transport-liveness` | [02-channel-transport/05-transport-liveness](02-channel-transport/05-transport-liveness.ko.md) |
| `30-host-relocation-flow` | [05-location-relocation/05-host-relocation-flow](05-location-relocation/05-host-relocation-flow.ko.md) |
| `31-failure-failover-policy` | [05-location-relocation/06-failure-failover-policy](05-location-relocation/06-failure-failover-policy.ko.md) |
| `32-framework-error-model` | [00-foundation/07-framework-error-model](00-foundation/07-framework-error-model.ko.md) |
| `33-core-hwm-application-job-flow` | [01-execution/05-application-job-queue-and-backpressure](01-execution/04-application-job-queue-and-backpressure.ko.md) |
| `40-internal-layering` | [00-foundation/08-layering](00-foundation/08-layering.ko.md) |
| `41-internal-serialization` | [01-execution/02-handler-turn-and-execution-gate](01-execution/02-handler-turn-and-execution-gate.ko.md) |
| `42-internal-progress-isolation` | §1~§4·§7 → [01-execution/02-handler-turn-and-execution-gate](01-execution/02-handler-turn-and-execution-gate.ko.md) · §5·§6 → [05-application-job-queue-and-backpressure](01-execution/04-application-job-queue-and-backpressure.ko.md) |
| `43-internal-completion` | [01-execution/01-submit-and-completion](01-execution/01-submit-and-completion.ko.md) |
| `44-internal-relocation-continuity` | [05-location-relocation/04-relocation-flow](05-location-relocation/04-relocation-flow.ko.md) |
| `45-internal-routing-and-cache` | §1·§1.1·§2 → [03-spot-actor/08-routing](03-spot-actor/08-routing.ko.md) · §3~§7 → [02-channel-transport/02-channel-messaging](02-channel-transport/02-channel-messaging.ko.md) |
| `46-internal-dispatch-loop` | §1·§2·§6·§8 → [01-execution/05-application-job-queue-and-backpressure](01-execution/04-application-job-queue-and-backpressure.ko.md) · §7 → [04-spot-timer](03-spot-actor/10-spot-timer.ko.md) · §3·§4 → [02-handler-turn-and-execution-gate](01-execution/02-handler-turn-and-execution-gate.ko.md) |
| `47-internal-object-lifecycle` | [03-spot-actor/09-object-lifecycle](03-spot-actor/09-object-lifecycle.ko.md) |
| `48-internal-session-binding` | [session/02-session-actor-binding](04-session/02-session-actor-binding.ko.md) |
| `49-internal-liveness-and-state` | §1 → [02-channel-transport/05-transport-liveness](02-channel-transport/05-transport-liveness.ko.md) · §2 → [03-spot-actor/03-mesh-node](03-spot-actor/03-mesh-node.ko.md) · §3~§5 → [06-observability](06-observability/README.ko.md) |
| `50-internal-message-ownership` | [01-execution/06-payload-ownership-and-codec](01-execution/05-payload-ownership-and-codec.ko.md) |
| `51-internal-service-wire-protocol` | [02-channel-transport/06-wire-protocol](02-channel-transport/06-wire-protocol.ko.md) |
| `52-internal-relocation-handoff` | [05-location-relocation/04-relocation-flow](05-location-relocation/04-relocation-flow.ko.md) |
