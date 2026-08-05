---
title: "11. Monitoring — 상태 관측과 진단 · Java"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: Location](10-location.ko.md) | [다음: 운영 — 메트릭 · drain · readiness](12-operations.ko.md)
<!-- framework-adapter-nav:end -->

# 11. Monitoring — 상태 관측과 진단

> **이 장의 계약 소유 문서** —
> [Java monitoring 공개 인터페이스](../../../common/spec/server/languages/java/interfaces/monitoring.ko.md)가
> 다룬다. 이 챕터는 그 계약이 노출하는 네 관측 표면을 사용법 중심으로 설명한다.

handler 호출만으로는 운영을 다 볼 수 없다. 연결이 준비되었는지, 어느 peer가 빠졌는지,
메시지가 어디서 실패했는지도 framework 표면에서 읽어야 한다. Java framework는 이를 **세
갈래**로 노출한다 — 상태 snapshot과 `Flow.Publisher`, 메시지 흐름 기록, Micrometer 계기다.

runtime event를 bean handler로 받는 표면은 없다. 관측은 전부 아래 세 갈래를 통한다.

## 1. 관측 표면

| 무엇을 보나 | 표면 | 어디서 다루나 |
|---|---|---|
| Host lifecycle(relocate · drain · readiness) | `ZLinkFrameworkRuntime.status()` · `observe()` | [12. 운영](12-operations.ko.md) §6.1 |
| MeshNode의 node · peer · channel 준비 상태 | `ZLinkRouteMeshRuntime.snapshot(...)` · `observe(...)` | [12. 운영](12-operations.ko.md) §5 |
| ClientServer channel의 target 상태 | `ZLinkClientServerRuntime` | 이 챕터 §2 |
| pub/sub channel의 publisher 상태 | `ZLinkFanoutRuntime` | 이 챕터 §2 |
| Location store 상태와 topology | `ZLinkLocationRuntimeQuery` | [10. Location](10-location.ko.md) §4 |
| 메시지 수신 · dispatch · 실패와 흐름 | `configureDispatch()`의 message flow | 이 챕터 §3 |
| CCU · 큐 깊이 같은 수치 | Micrometer `MeterRegistry` | 이 챕터 §4 |

넷 다 **bean으로 주입받는다.** Spring 컨테이너에 등록되어 있으므로 생성자 인자로
선언하면 된다.

## 2. 상태 snapshot과 status stream

상태 표면은 모두 같은 모양이다 — `snapshot(...)`이 immutable record 한 장을,
`observe(...)`가 그 이후 변화를 `Flow.Publisher`로 준다.

```java
@Service
public class MeshWatcher {
    private final ZLinkRouteMeshRuntime meshRuntime;

    public MeshWatcher(ZLinkRouteMeshRuntime meshRuntime) {
        this.meshRuntime = meshRuntime;
    }

    public boolean ready() {
        // 지금 값 한 장.
        ZLinkMeshNodeSnapshot snapshot = meshRuntime.snapshot("game.room");
        return meshRuntime.isReady("game.room");
    }

    public void watch(Flow.Subscriber<ZLinkObservedStatus<ZLinkMeshNodeSnapshot>> subscriber) {
        // capacity를 넘기면 느린 구독자는 중간 값을 건너뛴다.
        // 건너뛴 개수는 각 항목의 loss()에 담겨 온다.
        meshRuntime.observe("game.room", 64).subscribe(subscriber);
    }
}
```

**`observe(...)`는 `ZLinkObservedStatus<T>`를 준다.** `status()`가 변경 뒤의 완전한
snapshot이고 — 바뀐 field만 담은 event가 아니라 매번 전체 record다 — `loss()`가 이 구독이
지금까지 놓친 개수다. 이전 값과 비교할 일이 있으면 구독하는 쪽에서 보관한다.

`loss()`는 둘로 나뉜다. `coalescedCount()`는 합치기로 건너뛴 중간 상태 수이고,
`discardedTerminalCount()`는 보관 상한을 넘겨 영영 못 보게 된 terminal 상태 수다. 앞의
것은 최신 값을 받았다는 뜻이지만 뒤의 것은 그렇지 않다.

| | `snapshot(...)` | `observe(...)` |
| --- | --- | --- |
| 무엇을 주나 | 호출 시점의 record 하나 | 변경마다 완전한 record |
| 언제 쓰나 | 운영 endpoint 응답, 단발 확인 | 상태 전이를 기록하거나 반응할 때 |
| 놓칠 수 있나 | 해당 없음 | capacity를 넘기면 중간 값을 건너뛴다 |

`Flow.Publisher`는 JDK 표준 reactive stream이다. Reactor를 쓴다면
`JdkFlowAdapter.flowPublisherToFlux(...)`로, Kotlin이면 `asFlow()`로 감싼다.

Peer 상태는 Node RID와 현재 상태, 사용할 수 없는 이유만 담는다. 연결 의도(connection
intent) · discovery source · lifecycle generation은 framework 내부 상태라 공개하지 않는다.

## 3. 메시지 흐름 추적

메시지 하나가 어디서 어떻게 끝났는지는 message flow로 본다. 수준은 `configureDispatch()`가
정한다.

```java
@Bean
ZLinkFrameworkConfigurer zlink(PlaySettings settings) {
    return options -> {
        options.configureDispatch()
            .messageFlow(ZLinkMessageFlowLogMode.ERRORS_ONLY) // 기본값 — 실패와 backpressure만.
            .traceLogFile(settings.logDir() + "/flow.jsonl")  // 앱 로그와 분리해 따로 쓴다.
            .traceLabel(settings.instanceName());             // 어느 instance의 기록인지 표시한다.
    };
}
```

| 수준 | 남기는 것 |
| --- | --- |
| `OFF` | 남기지 않는다 |
| `ERRORS_ONLY`(기본) | dispatch 실패와 backpressure |
| `KEY_TRANSITIONS` | 위 + 수신 · dispatch · 완료 같은 주요 전이 |
| `VERBOSE` | 위 + 개별 메시지 단위 기록 |
| `DIAGNOSTIC` | 위 + 진단용 상세 |

**운영에서는 `ERRORS_ONLY`로 두고 필요할 때만 올린다.** `VERBOSE` 이상은 메시지마다
기록을 남기므로 처리량이 많은 구간에서 그 자체가 부하가 된다.

기록을 프로그램에서 받으려면 observer를 등록한다.

```java
options.configureDispatch().setMessageFlowObserver(error -> {
    // runtime 스레드에서 실행된다 — 여기서 blocking하거나 framework 표면을 다시 부르지 않는다.
    auditSink.append(error);
    return CompletableFuture.completedFuture(null);
});
```

처리기가 없는 dispatch의 동작은 `unhandled()`가 정한다 — `setRequest` · `setSend` ·
`setPublish`로 갈래마다, `setSendLogLevel` · `setPublishLogLevel`로 기록 수준을 정한다.

## 4. 메트릭

Framework 계기는 Micrometer로 나간다. Spring Boot Actuator를 쓰면 registry가 이미
컨테이너에 있으므로 **추가 배선이 필요 없다.** 이름을 바꾸거나 공통 tag를 붙이려면
`ZLinkMetricsCustomizer` bean을 둔다.

```java
@Bean
ZLinkMetricsCustomizer zlinkMetrics(PlaySettings settings) {
    return registry -> registry.config()
        .commonTags("node", settings.instanceName());
}
```

계기 이름은 `zlink.`으로 시작한다. 정확한 이름 · 종류 · 단위 · label은
[Runtime metric과 집계 규칙](../../../common/spec/25-runtime-metrics.ko.md)이 소유한다.

> **현재 Java runtime이 방출하는 계기는 계약의 일부뿐이다.** 계약이 정의한 47개 중
> 14개만 나오고, request 관련 셋은 계약 이름(`zlink.mesh_node.request.*`)이 아니라
> `zlink.channel.request.*`로 나온다. 대시보드를 만들기 전에 실제 방출 이름을 확인한다.

## 5. readiness와 liveness

Java에는 별도 health check 표면이 없다. **runtime 상태로 판정한다.**

```java
@Component
public class ZLinkReadinessIndicator implements HealthIndicator {
    private final ZLinkFrameworkRuntime runtime;

    @Override
    public Health health() {
        return runtime.status().isReady()
            ? Health.up().build()
            : Health.outOfService().build();
    }
}
```

**store 연결처럼 잠깐 끊길 수 있는 의존성은 readiness에만 반영한다.** liveness에 넣으면
store가 잠시 끊겼을 때 오케스트레이터가 프로세스를 죽인다.

## 6. 자주 발생하는 문제

- **`observe(...)`를 구독했는데 값이 안 온다** → `Flow.Publisher`는 구독해야 흐른다.
  `subscribe(...)`를 부르고 `Subscription.request(n)`으로 수요를 알렸는지 본다.
- **상태 전이 일부가 안 보인다** → `observe(...)`의 capacity를 넘겨 건너뛴 것이다.
  capacity를 늘리고 구독자가 더 빨리 소비하게 한다.
- **observer 안에서 데드락이 난다** → runtime 스레드에서 실행된다. 안에서 blocking
  대기를 하거나 framework 표면을 다시 부르지 않는다.
- **flow 기록이 비어 있다** → 기본 수준이 `ERRORS_ONLY`라 정상 흐름은 남지 않는다.
  `KEY_TRANSITIONS` 이상으로 올린다.
- **메트릭이 안 보인다** → Actuator와 registry가 컨텍스트에 있는지 본다. framework는
  registry에 계기를 올릴 뿐 registry 자체를 만들지 않는다.
- **store가 잠깐 끊겼는데 프로세스가 재시작된다** → store 상태가 liveness에 들어가 있다.
  readiness로 옮긴다.

## 7. 관련 문서

- 정식 계약: [Java monitoring 공개 인터페이스](../../../common/spec/server/languages/java/interfaces/monitoring.ko.md)
- 메트릭과 drain · readiness 운영: [12. 운영](12-operations.ko.md)
- 진단 옵션 목록: [16. Options](16-options.ko.md) §4
- 계기 이름 규약: [Runtime metric과 집계 규칙](../../../common/spec/25-runtime-metrics.ko.md)
