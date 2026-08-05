---
title: "16. Options — 설정 목록과 기본값 · Java"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: E2E 테스트](15-e2e-testing.ko.md) | [다음: ZLink를 어디에 쓰나](17-alternative.ko.md)
<!-- framework-adapter-nav:end -->

# 16. Options — 설정 목록과 기본값

> **이 장의 계약 소유 문서** —
> [Java configuration과 host 공개 계약](../../../common/spec/server/languages/java/interfaces/configuration-host.ko.md)이
> 다룬다. 이 챕터는 그 표면을 목록으로 정리해 무엇을 정할 수 있고 정하지 않으면
> 어떻게 되는지를 보여준다.

이 챕터는 **무엇을 정할 수 있고 정하지 않으면 어떻게 되는지**를 모은다. 각 옵션이
무엇을 바꾸는지는 해당 기능 챕터가 설명하고, 여기서는 자리와 기본값을 본다.

## 1. 설정 적용 위치

| 자리 | 적용 범위 | 변경 시점 |
| --- | --- | --- |
| `ZLinkFrameworkConfigurer`의 `options` | process 전체의 기본값 | 컨텍스트 시작 전에만 |
| builder | 그 node · channel · STREAM node 하나 | 컨텍스트 시작 전에만 |
| runtime option bean | 이미 실행 중인 값 일부 | 실행 중(§7) |

```java
@Bean
ZLinkFrameworkConfigurer zlink(PlaySettings settings) {
    return options -> {
        // ① 루트 — 이 process의 모든 payload에 적용된다.
        options.codecs().use(ZLinkProtobufCodec.getDefault());
        options.setDefaultRequestTimeout(Duration.ofSeconds(30));

        // ② builder — 이 node 하나에만 적용된다.
        ZLinkMeshNodeBuilder mesh = options.addRouteMesh("play");
        mesh.listen(settings.meshEndpoint())
            .setRoutingIdPrefix("play")
            .setSpotCapacity(2_000);
        mesh.channelName("room").server();
    };
}
```

Spring 컨텍스트가 시작된 뒤에 builder를 다시 호출하는 표면은 없다. 잘못된 조합은 첫
호출까지 미루지 않고 **컨텍스트 시작 단계에서 예외로 막힌다.**

## 2. 루트 옵션

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `codecs()` | payload 직렬화 형식 | 내장 JSON |
| `setDefaultRequestTimeout(Duration)` | request reply 대기 상한 | 30초 |
| `addHandlersFromPackageOf(Class)` | handler 탐색 시작점 | 탐색하지 않음 |
| `configureMetadata()` | metadata 전달 정책 | — |
| `configureDispatch()` | 진단 수준과 message flow(§4) | `ERRORS_ONLY` |
| `configureInboundDispatch()` | host 전체 수신 상한(§3.3) | 자동 계산 |
| `configureLocations()` | location store 동작(§5) | §5 표 |
| `configureNetwork()` | listener의 bind · advertise host 기본값 | bind `0.0.0.0` |
| `configureWorkers()` | CPU worker 풀(§3.2) | §3.2 표 |
| `configureStreamCompression()` | STREAM 압축 | 압축 없음 |
| `useFilter(Class)` | handler filter 등록. 부른 순서가 실행 순서다 | 없음 |
| `addLocationStore` · `addRelocationStore` | 위치 결정과 이전 저장소 | 없으면 단일 node 구성 |
| `setApplicationVersion(long)` | rolling update의 버전 | 지정 안 함 |
| `setMaintenanceWave(String)` | 같은 점검 묶음 표시 | 지정 안 함 |
| `useVirtualThreadHandlers()` | handler를 virtual thread에서 실행 | 플랫폼 스레드 |
| `useHandlerExecutor(Executor)` | handler 실행기를 직접 지정 | framework 기본 |

`setDefaultRequestTimeout`은 **0 이하를 거부한다.**

**`useVirtualThreadHandlers()`와 `useHandlerExecutor(...)`는 함께 쓰지 않는다.** 둘 다
handler 실행기를 정하므로 뒤에 부른 쪽이 앞을 덮는다.

## 3. MeshNode 옵션

`addRouteMesh(name)`이 돌려주는 builder에 지정한다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `listen(endpoint)` · `listen(port)` · `listen()` | 다른 node가 접속할 자기 주소 | 지정해야 한다 |
| `setBindHost` · `setAdvertiseHost` | bind 주소와 광고 주소를 나눠 쓸 때 | `configureNetwork()` 값 |
| `setRoutingId(...)` · `setRoutingIdPrefix(String)` | 이 node의 식별자 | 자동 생성 |
| `objects()` | Object role — spot · actor 배치 | 배치하지 않음 |
| `channelName(name)` | channel 역할 등록 | — |
| `setPlacementWeight(int)` | 새 object 배치 선택 가중치 | 100 |
| `setActorCapacity` · `setSpotCapacity` | 이 node가 담을 상한 | 무제한 |
| `setActivationConcurrency(int)` | 동시에 진행할 cold activation 수 | runtime 기본값 |
| `setDefaultRequestTimeout(Duration)` | 이 node 호출의 reply 상한 | 루트 값 |
| `peerConnections()` | 수동 peer 연결 | location store 자동 발견 |
| `configureRouterSocket()` | 아래 §3.1 | 아래 표 |
| `configureSpotPublisher()` | Logical Multicast 발행 소켓 | runtime 기본값 |

### 3.1 소켓 상한

`configureRouterSocket()`이 돌려주는 `ZLinkMeshNodeSocketConfig`의 값이다.

| 메서드 | 무엇을 정하나 |
| --- | --- |
| `setMaxMessageSize(long)` | 받아들일 message 하나의 최대 크기 |
| `setSendHighWaterMark(...)` | 상대별로 보내려고 보관할 byte |
| `setReceiveHighWaterMark(...)` | 상대별로 받아서 보관할 byte |
| `setReceiveTimeout` · `setSendTimeout` | 지정하면 그 방향의 대기 상한 |
| `setMailboxMessageBudget(long)` | 이 node의 service mailbox가 담을 message 수 |
| `setMailboxByteBudget(long)` | 이 node의 service mailbox가 담을 byte |

두 high-water mark의 동작 원리와 값을 고르는 기준은
[4. Backpressure](04-backpressure.ko.md)가 다룬다.
`0`은 기본값이 아니라 **무제한**이다.

**HWM 넷은 `long`이다.** byte 단위이므로 `int`로는 2 GiB를 넘길 수 없다.

### 3.2 CPU worker 풀

`configureWorkers()`가 돌려주는 `ZLinkWorkerOptions`의 값이다.
`context.runCpuWorker(...)`가 쓰는 단일 elastic 풀 하나를 정한다.

| 옵션 | 기본값 |
| --- | --- |
| `minThreads(int)` | 0 |
| `maxThreads(int)` | `max(2, CPU 수 × 2)` |
| `idleTimeout(Duration)` | 30초 |
| `maxQueueLength(int)` | 1024 |

**큐가 차면 submit이 즉시 실패한다.** 기다리거나 호출자 스레드에서 실행하는 정책은
없다. 큐 길이를 늘리기 전에 worker에 넘기는 작업의 실행 시간을 먼저 본다.

### 3.3 host 전체 수신 상한

`configureInboundDispatch()`가 돌려주는 `ZLinkInboundDispatchOptions`의 값이다.
연결마다 두는 상한(§3.1)과 성격이 다르다 — 아직 handler 실행을 시작하지 못한 message의
**payload 합계**에 적용한다.

| 메서드 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `setApplicationHwmBytes(long)` | host 전체 수신 상한 byte | 생략하면 자동 계산 |
| `setApplicationHwmProfile(ZLinkApplicationHwmProfile)` | 자동 계산이 쓸 비율 | `BALANCED` |
| `setProcessMemoryLimitBytes(long)` | 자동 계산의 기준이 되는 프로세스 메모리 상한 | 감지한 값 |

profile은 `COMPACT` · `LOW_LATENCY` · `BALANCED` · `THROUGHPUT` 넷이다.
`setApplicationHwmBytes(0)`은 **제한 없음**이고, 음수는 거절한다.
`setProcessMemoryLimitBytes`는 양수만 받는다. 둘 다 `ZLinkConfigurationException`이다.

> 이 단위와 상한은 계약으로 확정되었을 뿐 **아직 runtime이 사용하지 않는다.**
> [4. Backpressure §6](04-backpressure.ko.md#6-framework-runtime-적용-범위)을 본다.

## 4. 진단

`configureDispatch()`가 돌려주는 표면이다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `messageFlow(ZLinkMessageFlowLogMode)` | 기록 수준 | `ERRORS_ONLY` |
| `traceLogFile(String)` | 앱 로그와 분리해 쓸 파일 | 분리하지 않음 |
| `traceLabel(String)` | 기록에 붙일 instance 이름 | 없음 |
| `setMessageFlowObserver(...)` | 기록을 프로그램으로 받기 | 없음 |
| `unhandled()` | 처리기가 없는 dispatch의 동작 | 아래 |

`unhandled()`는 `setRequest` · `setSend` · `setPublish`로 갈래마다 동작을 정하고,
`setSendLogLevel` · `setPublishLogLevel`로 기록 수준을 정한다.

수준별로 무엇이 남는지는 `11. Monitoring` 장이 다룬다.

## 5. Location 옵션

`configureLocations()`가 돌려주는 `ZLinkLocationOptions`의 값이다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `setOwnerLeaseRenewInterval(Duration)` | owner lease 갱신 주기 | 5초 |
| `setOwnerLeaseTtl(Duration)` | lease 유효 기간 | 30초 |
| `setOwnerLeaseRenewTimeout(Duration)` | 갱신 호출의 상한 | 3초 |
| `setOwnerLeaseFencingMargin(Duration)` | 이전 owner를 배제하는 여유 | 5초 |
| `setPollingInterval(Duration)` | store 조회 주기 | 1초 |
| `setStoreFailureGrace(Duration)` | store 장애를 견디는 기간 | 30초 |
| `setRouteCacheMaxAge(Duration)` | 경로 캐시 유효 기간 | 15초 |
| `setMessageFollowDuration(Duration)` | 이동 중 대상으로 온 message를 따라 보내는 기간 | 30초 |
| `setMaxActiveOutboundRelocations(int)` | 동시에 내보낼 relocation 수 | 64 |
| `setMaxActiveInboundRelocations(int)` | 동시에 받아들일 relocation 수 | 64 |
| `setMaxConcurrentRelocationCaptures(int)` | 동시에 capture할 수 | 8 |
| `setMaxConcurrentRelocationRestores(int)` | 동시에 restore할 수 | 8 |
| `setMaxRelocationPayloadInFlightBytes(long)` | 이동 중 payload 총량 상한 | 256 MiB |

> **owner lease 기본값이 세 언어에서 모두 다르다.** 갱신 주기 대비 TTL 배수가 Java는
> 6배(5초 : 30초), C++은 3배(5초 : 15초), Node는 1.5배(10초 : 15초)다. 같은 mesh에
> 여러 언어 node를 섞는다면 값을 맞춰 지정한다.
> 저장소의 같은 갭 기록 G6이 이 차이를 다룬다.

## 6. STREAM 옵션

`addStreamNode(name)`이 돌려주는 builder에 지정한다.

| 옵션 | 무엇을 정하나 | 기본값 |
| --- | --- | --- |
| `bind(endpoint)` · `bind(port)` · `bind()` | client가 접속할 주소 | 지정해야 한다 |
| `setBindHost` · `setAdvertiseHost` | bind 주소와 광고 주소 | `configureNetwork()` 값 |
| `enableActorDispatch(String meshName)` | session이 Actor로 relay할 수 있게 한다 | 하지 않음 |
| `registerSession(Class)` | 연결마다 만들 session 타입 | 지정해야 한다 |
| `setTlsServer(cert, key[, requireClientCert])` | TLS 구성 | 평문 |

**`enableActorDispatch`는 mesh 이름을 함께 받는다.** Actor를 어느 mesh에서 찾을지
지정하는 인자이며, 다른 언어에는 없는 자리다.

## 7. 실행 중 바꿀 수 있는 것

시작 뒤에 바꿀 수 있는 값은 **가중치 둘뿐**이다. `ZLinkRouteMeshRuntimeOptions` bean을
주입받아 쓴다.

| 값 | 표면 | 무엇에 쓰나 |
| --- | --- | --- |
| 배치 가중치 | `mesh(name).setPlacementWeight(int)` | 새 object 배치 대상에서 빼거나 되돌린다 |
| channel 가중치 | `channel(name).setWeight(int)` | 새 select-one 대상에서 빼거나 되돌린다 |

둘 다 `0`으로 두면 **새 배정만 멈춘다.** 이미 있는 object와 연결은 그대로 살아 있다.

## 8. 반드시 정해야 하는 것

| 값 | 어디에 |
| --- | --- |
| MeshNode의 `listen` 주소 | `addRouteMesh(...).listen(...)` |
| STREAM node의 `bind` 주소와 session 타입 | `addStreamNode(...)` |
| fanout publisher의 endpoint | `addFanoutChannel(...).enablePublisher(...)` |
| Spot · Actor를 배치할 node의 Object role | `objects().server()` |
| 여러 node를 쓸 때의 location store | `addLocationStore(...)` |
| handler 탐색 시작점 | `addHandlersFromPackageOf(...)` |

## 9. 자주 발생하는 문제

- **handler가 등록되지 않는다** → `addHandlersFromPackageOf(...)`를 부르지 않았거나
  탐색 시작점이 handler package를 덮지 않는다. handler에 `@Component`를 붙여도 등록되지
  않는다.
- **`0`으로 두었더니 memory가 계속 는다** → high-water mark의 `0`은 기본값이 아니라
  무제한이다.
- **timeout을 0으로 넣었더니 시작이 실패한다** → 정상이다.
  `setDefaultRequestTimeout`은 0 이하를 거부한다.
- **CPU worker submit이 바로 실패한다** → 큐가 찼다. `maxQueueLength`를 늘리기 전에
  worker 작업의 실행 시간을 본다. 기다리는 정책은 없다.
- **virtual thread 설정이 안 먹는다** → `useHandlerExecutor(...)`를 뒤에 불러 덮었을
  수 있다. 둘 중 하나만 쓴다.
- **가중치를 0으로 했는데 기존 연결이 끊긴다고 생각했다** → 가중치는 **새 배정만**
  막는다.
- **두 언어 node를 섞었더니 owner 판정이 다르다** → `ownerLeaseTtl` 기본값이 언어마다
  다르다(§5). 값을 명시해 맞춘다.

## 10. 관련 문서

- 정식 계약: [Java configuration과 host 공개 계약](../../../common/spec/server/languages/java/interfaces/configuration-host.ko.md)
- 상한이 무엇을 바꾸는지: [4. Backpressure](04-backpressure.ko.md)
- 가중치로 트래픽을 빼는 절차: [12. 운영](12-operations.ko.md)
