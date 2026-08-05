# 01. Host lifecycle

[레퍼런스 목차](README.ko.md)

Host 등록(`@EnableZLinkFramework` + `ZLinkFrameworkConfigurer` bean)과
`ZLinkFrameworkRuntime`(`relocate`/`shutdown`/`status`/`observe`)은 Java 타입을 그대로 사용한다 —
정확한 signature와 옵션 표는
[Java 레퍼런스 01. Host lifecycle](../../java/reference/01-host-lifecycle.ko.md)을 그대로 따른다.
Kotlin이 추가하는 것은 `CompletionStage<T>`를 coroutine으로 잇는 브리지 하나뿐이다. 정확한
signature는
[Kotlin 공통 runtime exact interface](../../common/spec/server/languages/kotlin/interfaces/common-runtime.ko.md)가
소유한다.

---

## `CompletionStage<T>.await()`

Java의 `relocate(...)`/`shutdown(...)`이 반환하는 `CompletionStage<T>`를 suspend function 안에서
기다린다.

```kotlin
val relocation = frameworkRuntime.relocate(
    ZLinkFrameworkRelocationOptions(
        ZLinkFrameworkRelocationMode.ROLLING_UPDATE,
        12L,
        Duration.ofSeconds(30),
    )
).await()

if (relocation.outcome() == ZLinkFrameworkRelocationOutcome.RELOCATED) {
    frameworkRuntime.shutdown().await()
}
```

**옵션.** 이 함수에는 modifier가 없다 — 확장 함수 `await()`를 그대로 호출한다.

**완료 결과.** Java `CompletionStage`의 성공 값과 실패 원인을 그대로 보존한다. Coroutine 취소는
대기 중인 continuation만 끝내며, 이미 시작한 shared host operation(relocate·shutdown) 자체를
취소하지 않는다 — `relocate`/`shutdown`의 완료 kind·조건은 Java 레퍼런스와 동일하다.

**선택 기준.** Kotlin coroutine 코드에서 host lifecycle operation을 기다릴 때 항상 이 확장 함수를
쓴다. 별도 `relocateAsync`, `shutdownAsync`, `awaitStopped` 같은 전용 suspend wrapper는 없다 —
Java 반환값에 `await()`만 이어 붙인다.

---

## `status()` / `observe().asFlow()` (읽기·관찰)

Host의 현재 상태 조회는 Java `status()`를 그대로 호출한다(coroutine bridge가 필요 없는 동기
호출). 상태 변화 스트리밍은 Java `Flow.Publisher`를 Kotlin `Flow`로 잇는 공통 `asFlow()` bridge를
쓴다.

```kotlin
val status = frameworkRuntime.status()

frameworkRuntime.observe().asFlow().collect { observed ->
    // observed.status(), observed.loss()를 확인한다
}
```

**옵션.** 이 진입점에는 modifier가 없다.

**완료 결과.** `asFlow()`의 cancellation은 해당 subscriber 등록만 해제한다 — 공유 runtime,
monitoring publisher나 이미 시작한 host operation을 취소하지 않는다. `ZLinkObservedStatus`의
`status()`/`loss()` 의미는 Java 레퍼런스와 동일하다.

**선택 기준.** Java 레퍼런스의 `status`/`observe` 항목과 같다 — coroutine 코드에서 자연스럽게
쓰려면 `Flow` 변환만 추가한다.

---

전체 근거는
[Kotlin 공통 runtime exact interface](../../common/spec/server/languages/kotlin/interfaces/common-runtime.ko.md)와
[Java 레퍼런스 01. Host lifecycle](../../java/reference/01-host-lifecycle.ko.md)을 참고한다.
