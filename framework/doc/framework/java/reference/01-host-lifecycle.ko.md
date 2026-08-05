# 01. Host lifecycle

[레퍼런스 목차](README.ko.md)

이 category는 Spring Boot host 등록 진입점과 `ZLinkFrameworkRuntime`이 제공하는 진입점을 다룬다.
정확한 signature는
[Java 공통 runtime exact interface](../../common/spec/server/languages/java/interfaces/common-runtime.ko.md)와
[Java 구성과 host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.ko.md)가
소유한다.

---

## `@EnableZLinkFramework` + `ZLinkFrameworkConfigurer` (구성 시점)

Framework root를 Spring application context에 한 번 등록한다. 다른 모든 항목의 전제 조건이다.

```java
@Configuration
@EnableZLinkFramework
public class GameFrameworkConfig {

    @Bean
    ZLinkFrameworkConfigurer gameConfigurer() {
        return options -> {
            ZLinkMeshNodeBuilder play = options.addRouteMesh("play")
                .listen(5501)
                .setRoutingIdPrefix("play")
                .setPlacementWeight(100);
        };
    }
}
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `@EnableZLinkFramework` | 필수 | Spring starter의 auto-configuration을 활성화한다 |
| `ZLinkFrameworkConfigurer` bean (`configure(ZLinkFrameworkOptions)`) | 필수 | topology, handler, Location Store 등 모든 등록의 진입점이다. 여러 bean을 등록하면 모두 순서대로 적용된다 |

**완료 결과.** 반환값 없이 동기로 등록된다. Spring context 초기화(bean 생성) 시점에 구성을 검증하고,
실패하면 `ZLinkConfigurationException`으로 startup 자체를 실패시킨다 — 잘못된 구성이 message 처리
중에 처음 나타나지 않는다.

**선택 기준.** 모든 host가 정확히 한 번(하나 이상의 configurer bean으로) 등록한다.
`ZLinkFrameworkOptions`의 topology·handler 등록 세부는 topology-discovery category를 참고한다.
Application은 `ZLinkFrameworkRuntime`을 직접 생성·시작하지 않는다 — Spring starter가
`SmartLifecycle.start()`로 소유한다.

---

## `relocate`

현재 host가 들고 있는 stateful object(User Spot·Actor)를 다른 eligible node로 이전한다. 계획된
점검이나 rolling update 전에 호출한다.

```java
ZLinkFrameworkRelocationOptions options = new ZLinkFrameworkRelocationOptions(
    ZLinkFrameworkRelocationMode.ROLLING_UPDATE,
    2L,
    Duration.ofMinutes(5));

ZLinkFrameworkRelocationResult result = runtime.relocate(options)
    .toCompletableFuture().get();

if (result.outcome() == ZLinkFrameworkRelocationOutcome.RELOCATED) {
    runtime.shutdown().toCompletableFuture().get();
}
```

**옵션.** `ZLinkFrameworkRelocationOptions`의 component는 다음과 같다.

| Component | 기본값 | 의미 |
| --- | --- | --- |
| `mode` | 필수 | `PLANNED_MAINTENANCE`(source와 같은 application version만 target) 또는 `ROLLING_UPDATE`(지정한 version만 target) |
| `targetApplicationVersion` | `PLANNED_MAINTENANCE`에서는 `null`(source 값 사용), `ROLLING_UPDATE`에서는 필수(source보다 커야 함) | 목표 application version. 조합이 맞지 않으면 시작 전 `IllegalArgumentException`으로 거부한다 |
| `deadline` | `null`이면 Framework 기본 deadline | eligible target 수렴을 기다리는 상한 |

**완료 결과.** `ZLinkFrameworkRelocationResult.outcome()`이 `RELOCATED`면 모든 object 이전이 끝나고
host는 `RELOCATED` 상태가 된다(새 operation은 받지 않지만 infrastructure는 유지한다). `BLOCKED`면
`reason()`에 `TARGET_UNAVAILABLE`·`STORE_UNAVAILABLE`·`DEADLINE_EXCEEDED` 등이 담긴다. 각 호출은
shared operation 결과를 따르는 전용 `CompletableFuture` view를 반환한다 —
`toCompletableFuture().cancel(...)`은 그 waiter만 해제하며 host operation 자체는 계속 진행된다.

**선택 기준.** 배포 전 무중단 이전이 필요할 때 쓴다. 이전 없이 바로 종료하려면 `shutdown`을 직접
호출한다. 같은 mode·target version으로 중복 호출하면 진행 중인 operation에 합류하고, 다른 값으로
호출하면 `BLOCKED/OPERATION_IN_PROGRESS`로 완료한다.

---

## `shutdown`

Host를 종료한다. Relocation을 시작하지 않는다 — 이전이 필요하면 먼저 `relocate`를 호출한다.

```java
ZLinkFrameworkTerminationResult result = runtime.shutdown(Duration.ofSeconds(30))
    .toCompletableFuture().get();
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `shutdown()` | 없음(overload) | Framework 기본 deadline 사용 |
| `shutdown(Duration deadline)` | — | 종료 정리 상한. 초과하면 `FORCE_STOPPED`로 완료한다 |

**완료 결과.** `ZLinkFrameworkTerminationResult.outcome()`이 `STOPPED`(정상 정리) 또는
`FORCE_STOPPED`(deadline 초과·정리 실패)다. `SERVING`에서 호출하면 남은 application 처리와
resource를 정리하고, `RELOCATED`에서 호출하면 infrastructure 연결만 정리한다.

**선택 기준.** Host를 종료할 때 항상 호출한다. `RELOCATING` 도중 호출하면 진행 중인 atomic
relocation unit의 결과만 확정하고 나머지는 시작하지 않는다 — 그 relocation을 기다리던 호출자는
`BLOCKED/SHUTDOWN_REQUESTED`를 받는다.

---

## `status` / `observe` (읽기·관찰)

Host의 현재 상태를 한 번 읽거나, 상태 변화를 실시간으로 관찰한다.

```java
ZLinkFrameworkRuntimeStatus status = runtime.status();
boolean canAcceptNewOperations = status.isReady() && status.acceptingWork();

runtime.observe().subscribe(new Flow.Subscriber<>() {
    // onNext(observed)에서 observed.status(), observed.loss()를 확인한다
});
```

**옵션.** 이 진입점에는 modifier가 없다 — `status()`는 인자가 없고, `observe()`는
`Flow.Publisher<ZLinkObservedStatus<ZLinkFrameworkRuntimeStatus>>`를 반환한다.

**완료 결과.** `status()`는 동기 호출이다. `isReady()`는 `state() == SERVING`일 때만 `true`이고,
`acceptingWork()`는 새 application operation 수락 여부를 나타낸다 — 두 값이 다를 수 있으므로 둘 다
확인한다. `observe()`는 terminal 완료 없이 스트리밍하며, `ZLinkObservedStatus.loss()`
(`coalescedCount`/`discardedTerminalCount`)로 관찰 유실 여부를 판단한다.

**선택 기준.** 지금 이 순간의 상태 한 번만 필요할 때 `status()`를, 상태 전이를 놓치지 않고 계속
받으려면 `observe()`를 쓴다. Spring Boot Actuator `HealthIndicator`를 구현할 때도 이 `status()`를
읽어 `Health.up()`/`Health.outOfService()`를 결정한다 — Framework가 별도 health builder를 제공하지
않으므로 이 판단은 application의 `HealthIndicator` bean이 직접 작성한다.

---

전체 근거는
[Java 공통 runtime exact interface](../../common/spec/server/languages/java/interfaces/common-runtime.ko.md)와
[Java 구성과 host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.ko.md)를
참고한다.
