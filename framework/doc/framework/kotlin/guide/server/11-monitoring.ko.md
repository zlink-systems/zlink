---
title: "11. Monitoring — 상태 관측과 진단 · Kotlin"
---

<!-- framework-adapter-nav:start -->
[가이드 홈](../../../index.ko.md) | [이전: Location](10-location.ko.md) | [다음: 운영 — 메트릭 · drain · readiness](12-operations.ko.md)
<!-- framework-adapter-nav:end -->

# 11. Monitoring — 상태 관측과 진단

> **이 장의 계약 소유 문서** — 관측 표면은
> [Java 11. Monitoring](../../../java/guide/server/11-monitoring.ko.md)과 같다.
> 이 장은 Kotlin에서 달라지는 자리만 적는다.

Java 장을 먼저 읽는다. 상태 표면 넷, message flow 수준, Micrometer 연동, readiness
판정은 모두 그대로 적용된다. Kotlin이 바꾸는 것은 **받는 모양** 셋뿐이다.

## 1. status stream을 `Flow`로 받기

Java는 `Flow.Publisher`를 준다. `asFlow()` 확장으로 코루틴 `Flow`로 바꿔 받는다.

```kotlin
@Service
class MeshWatcher(private val meshRuntime: ZLinkRouteMeshRuntime) {

    suspend fun watch() {
        // capacity를 넘기면 느린 수집자는 중간 값을 건너뛴다 — Java와 같다.
        meshRuntime.observe("game.room", 64).asFlow().collect { observed ->
            record(observed.status())
            // observed.loss()가 이 구독이 놓친 개수다.
        }
    }
}
```

`asFlow()`는 `zlink-framework-kotlin`이 제공한다. 구독 취소는 `Flow` 수집을 끝내면
함께 정리된다 — `Subscription`을 직접 다루지 않는다.

호스트 상태도 같은 방식이다.

```kotlin
runtime.observe().asFlow().collect { observed ->
    val status = observed.status()
    logger.info("host lifecycle: {} {}", status.state(), status.relocationResult())
}
```

## 2. 페이지 조회를 `Flow`로 받기

Location topology처럼 페이지로 끊어 오는 조회는 확장 함수가 이어 붙여 준다.

```kotlin
// Java: listTopology(filter, page)를 cursor가 빌 때까지 반복한다.
// Kotlin: topology(...)가 그 반복을 Flow로 감싼다.
query.topology(ZLinkLocationTopologyFilter("play"), pageSize = 100)
    .collect { entry -> render(entry) }
```

`pageSize`는 기본 100이다. **페이지 경계는 여전히 존재한다** — `Flow`가 감췄을 뿐
한 번에 다 가져오지 않는다.

## 3. message flow diagnostics 설정

Kotlin은 Java와 같은 네 수준을 receiver DSL로 설정한다.

```kotlin
options.configureDispatch {
    messageFlow(ZLinkMessageFlowLogMode.ERRORS) // 기본값: 실패와 backpressure만 기록한다.
    traceSampleRate(1.0)
    includeMessageSizes(true)
}
```

Level은 `OFF`, `ERRORS`, `NORMAL`, `DETAILED`다. Framework는 application이 구성한 standard
logger·trace·metric provider에 structured record를 기록한다. Message-flow observer나 runtime
error sink를 Kotlin 람다로 등록하는 public API는 없다. Provider 실패는 원래 operation과 격리한다.

## 4. 자주 발생하는 문제

- **`asFlow()`가 안 보인다** → `zlink-framework-kotlin` 의존성과
  `systems.zlink.framework.kotlin` import를 확인한다.
- **`Flow` 수집을 멈췄는데 구독이 남는다** → 수집 코루틴이 취소되면 구독도 끝난다.
  scope를 살려 둔 채 수집만 중단하지 않았는지 본다.
- **나머지 증상** → [Java 11. Monitoring](../../../java/guide/server/11-monitoring.ko.md) §6을 본다.

## 5. 관련 문서

- 관측 표면 전체: [Java 11. Monitoring](../../../java/guide/server/11-monitoring.ko.md)
- Kotlin 레이어가 얹는 것: [1. 개요](01-overview.ko.md) §2
- Kotlin 전용 계약: [Kotlin monitoring 공개 계약](../../../common/spec/server/languages/kotlin/interfaces/monitoring.ko.md)
- 진단 옵션 목록: [Java 16. Options](../../../java/guide/server/16-options.ko.md) §4
