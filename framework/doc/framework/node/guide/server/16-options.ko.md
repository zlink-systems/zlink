---
title: "16. Options — 설정 목록과 기본값 · Node/TypeScript"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: E2E 테스트](15-e2e-testing.ko.md) | [다음: ZLink를 어디에 쓰나](17-alternative.ko.md)
<!-- framework-adapter-nav:end -->

# 16. Options — 설정 목록과 기본값

> **이 장의 계약 소유 문서** —
> [Node.js foundation과 configuration 공개 계약](../../../common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md)이
> 다룬다. 이 챕터는 그 표면을 목록으로 정리해 무엇을 정할 수 있고 정하지 않으면
> 어떻게 되는지를 보여준다.

이 챕터는 **무엇을 정할 수 있고 정하지 않으면 어떻게 되는지**를 모은다. 각 옵션이
무엇을 바꾸는지는 해당 기능 챕터가 설명하고, 여기서는 자리와 기본값을 본다.

**시간 값은 모두 밀리초 숫자다.** 이름이 `...Ms`로 끝나는 것이 그 표시다.

## 1. 설정 적용 위치

| 자리 | 적용 범위 | 변경 시점 |
| --- | --- | --- |
| `zlinkFramework()` builder | process 전체의 기본값 | 모듈 초기화 전에만 |
| 하위 builder | 그 node · channel · STREAM node 하나 | 모듈 초기화 전에만 |
| runtime option 토큰 | 이미 실행 중인 값 일부 | 실행 중(§7) |

```typescript
ZLinkModule.forRootFactory({
  useFactory: () => {
    const builder = zlinkFramework();

    // ① 루트 — 이 process 전체에 적용된다.
    builder.codecs().use(ZLinkProtobufCodec.default);
    builder.setMessageFollowDuration(30_000);

    // ② builder — 이 node 하나에만 적용된다.
    const mesh = builder.addRouteMesh('play')
      .listen(config.meshEndpoint)
      .setRoutingIdPrefix('play')
      .setSpotLimit(2_000);
    mesh.channel('room').server();

    return builder.build();   // 돌려주지 않으면 아무것도 켜지지 않는다.
  }
});
```

모듈 초기화 뒤에 builder를 다시 부르는 표면은 없다. 잘못된 조합은 첫 호출까지 미루지
않고 **초기화 단계에서 예외로 막힌다.**

## 2. 루트 옵션

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `codecs()` | payload 직렬화 형식 | 내장 JSON |
| `configureNetwork()` | listener의 bind · advertise host 기본값 | bind `0.0.0.0` |
| `configureWorker(options)` | CPU worker 풀(§3.2) | §3.2 표 |
| `configureDispatch()` | 진단 수준·message flow(§4), Core HWM·Application job queue(§3.3) | `"errors"`, 두 profile 모두 `Balanced` |
| `configureLocations()` | location store 동작(§5) | §5 표 |
| `configureStreamCompression()` | STREAM 압축 | 압축 없음 |
| `addLocationStore` · `addRelocationStore` | 위치 결정과 이전 저장소 | 없으면 단일 node 구성 |
| `setApplicationVersion(bigint)` | rolling update의 버전 | 지정 안 함 |
| `setMaintenanceWave(string)` | 같은 점검 묶음 표시 | 지정 안 함 |
| `setActorTransferTimeout(ms)` | Actor 이전의 상한 | runtime 기본값 |
| `setMessageFollowDuration(ms)` | 이동 중 대상으로 온 message를 따라 보내는 기간 | 30,000 |
| `addRouteMesh` · `addClientServerChannel` · `addFanoutChannel` · `addStreamNode` | topology 등록 | — |

**`setApplicationVersion`은 `bigint`를 받는다.** 숫자 리터럴에 `n`을 붙인다 — `12n`.

## 3. MeshNode 옵션

`addRouteMesh(name)`이 돌려주는 builder에 지정한다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `listen(endpoint)` · `listen(port?)` | 다른 node가 접속할 자기 주소 | 지정해야 한다 |
| `setBindHost` · `setAdvertiseHost` | bind 주소와 광고 주소를 나눠 쓸 때 | `configureNetwork()` 값 |
| `routingId(...)` · `setRoutingIdPrefix(string)` | 이 node의 식별자 | 자동 생성 |
| `objects()` | Object role — spot · actor 배치 | 배치하지 않음 |
| `channel(name)` | channel 역할 등록 | — |
| `setPlacementWeight(number)` | 새 object 배치 선택 가중치 | 100 |
| `setActorLimit` · `setSpotLimit` | 이 node가 담을 상한 | 무제한 |
| `setActivationConcurrency(number)` | 동시에 진행할 cold activation 수 | runtime 기본값 |
| `setDefaultRequestTimeout(ms)` | 이 node 호출의 reply 상한 | 30,000 |
| `peerConnections()` | 수동 peer 연결 | location store 자동 발견 |
| `configureRouterSocket()` | 아래 §3.1 | 아래 표 |
| `configureSpotPublisher()` | Logical Multicast 발행 소켓 | runtime 기본값 |

### 3.1 소켓 상한

`configureRouterSocket()`이 돌려주는 `ZLinkMeshNodeSocketConfig`의 값이다.

| 필드 | 무엇을 정하나 |
| --- | --- |
| `maxMessageSize` | 받아들일 message 하나의 최대 크기 |
| `sendHighWaterMark` · `receiveHighWaterMark` | 상대별로 보관할 byte |
| `receiveTimeoutMs` · `sendTimeoutMs` | 지정하면 그 방향의 대기 상한 |

> **spec에 있고 구현에 없는 값이 둘 있다.** 공개 계약은 `mailboxMessageBudget` ·
> `mailboxByteBudget`을 이 config에 두지만 현재 builder 표면에는 없다.

동작 원리와 값을 고르는 기준은
[4. Backpressure](04-backpressure.ko.md)가 다룬다.
`0`은 기본값이 아니라 **무제한**이다.

### 3.2 CPU worker 풀

`configureWorker({...})`로 한 번에 넘긴다. 다른 언어처럼 메서드를 이어 부르는 형태가
아니다.

| 필드 | 무엇을 정하나 |
| --- | --- |
| `minThreads` · `maxThreads` | 풀 크기 |
| `idleTimeoutMs` | 유휴 스레드를 접는 시간 |
| `maxQueueLength` | 대기 큐 길이 |

**I/O worker는 이 풀을 쓰지 않는다.** `runIoWorker(...)`는 이벤트 루프에서 돈다.

### 3.3 Core HWM과 Application job queue

`configureDispatch()`가 돌려주는 `ZLinkDispatchOptionsBuilder`의 값이다. Core HWM은
ordinary queue의 accounted byte를 제한하고, Application job queue는 handler 시작을 기다리는
job 수를 host instance 전체에서 제한한다.

| 메서드 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `coreHwmMemoryLimitBytes(bigint | undefined)` | Core budget 계산에 전달할 memory limit hint | `undefined` |
| `coreHwmBudgetBytes(bigint | undefined)` | profile보다 우선하는 manual Core budget | `undefined`(Auto) |
| `coreHwmProfile(ZLinkCoreHwmProfile)` | Core Auto-budget profile | `Balanced` |
| `applicationJobQueueProfile(ZLinkApplicationJobQueueProfile)` | queued job Auto profile | `Balanced` |
| `maxQueuedApplicationJobs(bigint | undefined)` | 정확한 manual queued-job 상한 | `undefined`(Auto) |

Memory limit과 Core budget은 양수만 허용한다. Manual queued-job 상한은
`1..2,147,483,647`이며 `0n`은 unlimited가 아니라 startup configuration error다. 두 profile은
같은 label을 사용하지만 독립된 enum과 계산이다. 포화 동작과 운영값 측정은
[4. Backpressure](04-backpressure.ko.md)와 [공통 perf §23](../../../common/perf/README.ko.md#23-core-hwm과-application-job-queue-운영값-측정)이 다룬다.

## 4. 진단

`configureDispatch()`가 돌려주는 표면이다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `messageFlow(mode)` | 기록 수준 | `"errors"` |
| `traceSampleRate(rate)` | 표본 비율 | 1.0 |
| `includeMessageSizes(include)` | payload byte를 함께 남길지 | 남기지 않음 |

수준은 `"off"` · `"errors"` · `"normal"` · `"detailed"` 넷인 string union이다.

## 5. Location 옵션

`configureLocations()`가 돌려주는 `ZLinkLocationOptions`의 값이다. 모두 메서드 체인이다.

| 옵션 | 기본값 |
| --- | --- |
| `ownerLeaseRenewIntervalMs(...)` | 5,000 |
| `ownerLeaseTtlMs(...)` | 15,000 |
| `ownerLeaseRenewTimeoutMs(...)` | 3,000 |
| `ownerLeaseFencingMarginMs(...)` | 5,000 |
| `pollingIntervalMs(...)` | 1,000 |
| `storeFailureGraceMs(...)` | 30,000 |
| `routeCacheMaxAgeMs(...)` | 15,000 |
| `messageFollowDurationMs(...)` | 30,000 |

> **갱신 주기 대비 TTL 배수가 3배다**(5초 : 15초). 값을 바꿀 때는
> `renew interval + renew timeout < TTL - fencing margin` 관계를 유지한다.

## 6. STREAM 옵션

`addStreamNode(name)`이 돌려주는 builder에 지정한다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `bind(endpoint)` · `bind(port?)` | client가 접속할 주소 | 지정해야 한다 |
| `setBindHost` · `setAdvertiseHost` | bind 주소와 광고 주소 | `configureNetwork()` 값 |
| `enableActorDispatch()` | session이 Actor로 relay할 수 있게 한다 | 하지 않음 |
| `registerSession(sessionType)` | 연결마다 만들 session class(또는 session factory class) | 지정해야 한다 |
| `setTlsServer(certPath, keyPath, requireClientCert?)` | TLS 구성 | 평문 |

**`registerSession`은 session class 자체를 받거나, 연결별 생성 로직이 필요하면
factory class를 받는다.** 둘 다 다른 언어와 마찬가지로 class 참조로 넘긴다.

## 7. 실행 중 바꿀 수 있는 것

시작 뒤에 바꿀 수 있는 값은 **가중치 둘뿐**이다. `ZLINK_ROUTE_MESH_RUNTIME_OPTIONS`
토큰으로 주입받아 쓴다.

| 값 | 표면 | 무엇에 쓰나 |
| --- | --- | --- |
| 배치 가중치 | `mesh(name).placementWeight = 0` | 새 object 배치 대상에서 빼거나 되돌린다 |
| channel 가중치 | `channel(name).weight = 0` | 새 select-one 대상에서 빼거나 되돌린다 |

**Node는 property 대입이다.** 다른 언어의 setter 메서드와 모양이 다르다.

둘 다 `0`으로 두면 **새 배정만 멈춘다.** 이미 있는 object와 연결은 그대로 살아 있다.

## 8. 반드시 정해야 하는 것

| 값 | 어디에 |
| --- | --- |
| `useFactory`가 builder를 돌려주는 것 | `ZLinkModule.forRootFactory({ useFactory })` |
| MeshNode의 `listen` 주소 | `addRouteMesh(...).listen(...)` |
| STREAM node의 `bind` 주소와 session factory | `addStreamNode(...)` |
| fanout publisher의 endpoint | `addFanoutChannel(...).enablePublisher(...)` |
| Spot · Actor를 배치할 node의 Object role | `objects().server()` |
| 여러 node를 쓸 때의 location store | `addLocationStore(...)` |

## 9. 자주 발생하는 문제

- **아무것도 안 켜진다** → `useFactory`가 완성된 옵션을 돌려주지 않았다. 마지막에
  `return builder.build()`가 있어야 한다.
- **timeout이 이상하게 짧거나 길다** → 인자가 **밀리초 숫자**다. 초로 착각해 `3`을
  넣으면 3밀리초다.
- **`setApplicationVersion`에서 타입 오류가 난다** → `bigint`를 받는다. `12n`으로 쓴다.
- **`0`으로 두었더니 memory가 계속 는다** → high-water mark의 `0`은 무제한이다.
- **level 값이 안 맞는다** → Node는 lowercase string(`"errors"`)을 사용한다.
- **`registerSession`이 factory만 받는다고 알고 있었다** → session class를 직접 넘겨도
  된다. factory class는 연결별로 다른 생성 로직이 필요할 때만 쓴다.
- **가중치를 0으로 했는데 기존 연결이 끊긴다고 생각했다** → 가중치는 **새 배정만** 막는다.
- **여러 언어 node를 섞었더니 owner 판정이 다르다** → lease 기본값이 언어마다 다르다(§5).

## 10. 관련 문서

- 정식 계약: [Node.js foundation과 configuration 공개 계약](../../../common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md)
- 상한이 무엇을 바꾸는지: [4. Backpressure](04-backpressure.ko.md)
- 가중치로 트래픽을 빼는 절차: [12. 운영](12-operations.ko.md)
- 주입 토큰 목록: [13. 주요 interface 사용 색인](13-interface-catalog.ko.md) §1
