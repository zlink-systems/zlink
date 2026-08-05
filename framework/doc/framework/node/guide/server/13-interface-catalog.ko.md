---
title: "13. 주요 타입 사용 색인 · Node/TypeScript"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: 운영 — 메트릭 · drain · readiness](12-operations.ko.md) | [다음: 샘플 고르기](14-samples.ko.md)
<!-- framework-adapter-nav:end -->

# 13. 주요 타입 사용 색인

> **이 장의 계약 소유 문서** —
> [Node.js exact interface 목차](../../../common/spec/server/languages/node/interfaces/README.ko.md)가
> 정확한 signature를 소유한다. 이 챕터는 application에서 자주 쓰는 public 표면을
> 기능별로 찾는 안내서다.

Node 표면은 **어디서 import하는지**로 먼저 갈린다.

| 패키지 | 무엇이 있나 |
| --- | --- |
| `@zlink-systems/framework` | 계약 타입 — interface · type · enum |
| `@zlink-systems/nestjs` | 등록 · 데코레이터 · 주입 토큰 |

계약 타입은 `import type`으로 가져와도 된다. 런타임 값이 필요한 것은 데코레이터와
토큰, `zlinkFramework()`뿐이다.

## 1. 주입 토큰

client와 runtime은 **토큰으로 주입받는다.** 타입만 적으면 Nest가 무엇을 넣을지 모른다.

```typescript
constructor(
  @Inject(ZLINK_ROUTE_CLIENT) private readonly client: ZLinkRouteClient
) {}
```

| 토큰 | 주입되는 것 |
| --- | --- |
| `ZLINK_ROUTE_CLIENT` | ChannelName · Node RID로 send · request |
| `ZLINK_CHANNEL_CLIENT` | Spot·Actor 안에서 나가는 호출 |
| `ZLINK_FANOUT_CLIENT` | classic fanout publish |
| `ZLINK_SPOT_PUBLISHER_CLIENT` | Spot 밖에서 Logical Multicast 발행 |
| `ZLINK_SPOT_MANAGER` | Spot 생성 · 조회 · close |
| `ZLINK_ACTOR_MANAGER` · `ZLINK_ACTOR_CLIENT` | Actor 생성 · 조회, ActorId 호출 |
| `ZLINK_SPOT_OUTBOUND` | Spot context의 outbound |
| `ZLINK_FRAMEWORK_RUNTIME` | host 상태와 relocate · shutdown |
| `ZLINK_ROUTE_MESH_RUNTIME` · `ZLINK_CLIENT_SERVER_RUNTIME` · `ZLINK_FANOUT_RUNTIME` | 각 표면의 상태 |
| `ZLINK_ROUTE_MESH_RUNTIME_OPTIONS` · `ZLINK_CHANNEL_RUNTIME_OPTIONS` | 실행 중 가중치 조정 |
| `ZLINK_LOCATION_RUNTIME_QUERY` | location 상태와 topology |
| `ZLINK_MESSAGE_METADATA_POLICY` | metadata 정책 |
| `ZLINK_HTTP_CLIENT_REGISTRY` | HTTP client |

## 2. handler 데코레이터

받는 것마다 데코레이터가 하나씩 있다. 첫 인자는 handler group, 그다음은 packet 이름이나
topic이다.

| 데코레이터 | 짝이 되는 계약 |
| --- | --- |
| `zlinkRequestHandler(group, packet)` | `ZLinkRequestHandler<TReq, TRes>` |
| `zlinkSendHandler(group, packet)` | `ZLinkSendHandler<TMsg>` |
| `zlinkPublishHandler(group, packet)` | `ZLinkFanoutHandler<TEvent>` |
| `zlinkSpotPacketHandler(...)` | `ZLinkSpotPacketHandler<TSpot, TMsg>` |
| `zlinkSpotSubscriptionHandler(...)` | `ZLinkSpotSubscriptionHandler<TSpot, TEvent>` |
| `zlinkSpotTimerHandler(...)` | `ZLinkSpotTimerHandler<TSpot>` |
| `zlinkSpotActorSendHandler(...)` · `zlinkSpotActorRequestHandler(...)` | member Actor 앞 packet · request |
| `zlinkEntrySpotPacketHandler(...)` · `zlinkEntrySpotSubscriptionHandler(...)` | Entry Spot 앞 |
| `zlinkEntrySpotActorSendHandler(...)` · `zlinkEntrySpotActorRequestHandler(...)` | Entry Spot의 Actor 앞 |
| `zlinkHandler(...)` | 위 갈래를 직접 지정할 때 |

**packet 이름은 보내는 쪽과 정확히 같아야 한다.** 상수 모듈로 묶어 공유한다.

## 3. 등록 표면

| 이름 | 하는 일 |
| --- | --- |
| `ZLinkModule.forRootFactory({ useFactory })` | 등록 진입점. factory가 builder를 돌려준다 |
| `zlinkFramework()` | builder를 만든다 |
| `zlinkModule(__dirname, options)` | 그 디렉터리의 handler·Spot·Actor를 provider로 모은다 |
| `zlinkDiscoverProviders(...)` | provider 탐색을 직접 제어할 때 |
| `ZLinkFrameworkOptionsBuilder` | builder 타입 |
| `ZLinkMeshNodeBuilder` · `ZLinkMeshChannelBuilder` · `ZLinkMeshObjectRoleBuilder` | MeshNode와 역할 |
| `ZLinkFanoutChannelBuilder` · `ZLinkStreamNodeBuilder` | fanout channel · STREAM node |
| `ZLinkMeshPeerConnections` · `ZLinkEndpointConnections` | 수동 peer 연결 |
| `ZLinkMeshNodeSocketConfig` | 소켓 상한 |

## 4. Spot과 Actor

| 계약 | 성격 |
| --- | --- |
| `ZLinkSpot` · `ZLinkEntrySpot<TActor>` · `ZLinkInstanceSpot` | 구현한다 |
| `ZLinkSpotContext` · `ZLinkEntrySpotContext` · `ZLinkInstanceSpotContext` | `readonly context`로 받는다 |
| `ZLinkSpotCommonContext` | 위 셋의 공통 부분 |
| `ZLinkSpotManager` · `ZLinkSpotCreateResult` · `ZLinkSpotCreateState` | 생성과 결과 |
| `ZLinkSpotCreateResponse` · `ZLinkSpotActorJoinResult` | admission 응답 |
| `ZLinkSpotRelocationAdapter` · `ZLinkSpotRelocationReadyCall` | 상태 이전 |
| `ZLinkTimer` · `ZLinkTimerOptions` · `ZLinkTimerTick` | timer |
| `ZLinkWorkerCall<T>` | `runCpuWorker` · `runIoWorker`의 반환 |
| `ZLinkActor` · `ZLinkActorContext` · `ZLinkActorManager` | Actor |
| `ZLinkActorCreateResult` · `ZLinkActorJoinCompletion` | 생성 · join 결과 |
| `ZLinkActorRelocationAdapter` | Actor 상태 이전 |

**Spot·Actor는 `readonly context!`로 context를 받는다.** 생성자 인자가 아니라 framework가
채우는 property다.

**결과 타입은 discriminated union이다.** `result.status === 'rejected'`처럼 판별자로
가른다. 다른 언어의 sealed class나 `std::variant`에 해당한다.

## 5. STREAM session

| 계약 | 성격 |
| --- | --- |
| `ZLinkSession` | 구현한다 |
| `ZLinkSessionContext` · `ZLinkSessionDispatchContext` | context |
| `ZLinkSessionActor` · `ZLinkSessionActors` | bind된 Actor |
| `ZLinkStreamError` | 오류 통지 |
| `ZLinkBoundSession` | Actor에 묶인 session으로 push |

## 6. 관측과 실패

| 계약 | 성격 |
| --- | --- |
| `ZLinkFrameworkRuntime` · `ZLinkFrameworkRuntimeStatus` | host 상태 |
| `ZLinkRouteMeshRuntime` · `ZLinkRouteMeshStatus` | MeshNode 상태 |
| `ZLinkDispatchOptionsBuilder` · `ZLinkMessageFlowLogMode` | 진단 수준 |
| `ZLinkMessageFlowObserver` | 흐름 기록 |
| `ZLinkFrameworkException` | 실패. `kind` · `isRetriable` |
| `ZLinkFrameworkErrorKind` | 실패 갈래 |

## 7. Node에서 다른 자리

다른 언어를 먼저 본 독자가 걸리는 셋이다.

| 자리 | Node |
| --- | --- |
| timeout 인자 | `Duration`이 아니라 **밀리초 숫자** — `timeout(3_000)` |
| 취소 전달 | `CancellationToken`이 아니라 **`AbortSignal`** |
| 결과 판별 | 타입 검사가 아니라 **판별자 property** — `result.status` |

## 8. 어디서 오는가

| 얻는 방법 | 해당 표면 |
| --- | --- |
| 구현한다 | `ZLinkSpot` · `ZLinkEntrySpot` · `ZLinkActor` · `ZLinkSession` · `*Handler` 계약 |
| `readonly context!`로 받는다 | `ZLinkSpotContext` 계열 · `ZLinkActorContext` · `ZLinkSessionContext` |
| `@Inject(토큰)`으로 받는다 | §1의 토큰 전부 |
| `zlinkFramework()` builder가 돌려준다 | `ZLinkMeshNodeBuilder` 계열 |
| 호출이 돌려준다 | `*Call` · `*Result` · `*Status` |

## 9. 관련 문서

- 정확한 signature: [Node.js exact interface 목차](../../../common/spec/server/languages/node/interfaces/README.ko.md)
- 등록 진입점: [2. 시작하기](02-getting-started.ko.md)
- NestJS host 계약: [Node.js NestJS host 공개 계약](../../../common/spec/server/languages/node/interfaces/07-nestjs-host.ko.md)
