# Kotlin 공통 runtime 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Java 공통 runtime](../../java/interfaces/common-runtime.ko.md)

Kotlin은 Java의 `ZLinkTopologyState`, `ZLinkFrameworkRuntimeState`,
`ZLinkFrameworkRelocationMode`, `ZLinkFrameworkRelocationOptions`, relocation·termination
outcome·reason·result와 `ZLinkFrameworkRuntime`을 그대로 사용한다. 같은 enum, options, result wrapper와
runtime facade를 추가하지 않는다. 별도 drain facade와 MeshName을 받는 partial termination member는 없으며,
Kotlin은 Java host의 `Relocate`와 `Shutdown`을 그대로 사용한다.

Kotlin artifact가 직접 선언하는 call, handler와 coroutine lifecycle adapter의 source owner는
`systems/zlink/framework/kotlin/contracts/`다. Source를 이 경계로 옮겨도 package 선언은
`systems.zlink.framework.kotlin`을 유지하므로 이 문서의 public FQN은 바뀌지 않는다. Java contract를
Kotlin source에 다시 선언하지 않는다.

Handler filter도 Java의 `ZLinkHandlerFilter`, `ZLinkHandlerFilterContext`,
`ZLinkHandlerFilterNext<T>`와 `ZLinkHandlerDispatchKind`를 그대로 사용한다. Kotlin
전용 filter context나 dispatch enum을 추가하지 않는다. 기존 Kotlin filter 구현은
`invoke`의 첫 인자를 `ZLinkMessageContext`에서 `ZLinkHandlerFilterContext`로 바꿔야
한다.
Host가 continuity preflight를 통과해 relocation unit을 준비하는 동안에는 Java enum의 `RELOCATING(2)`를
관측하며, relocation이 완료되면 `RELOCATED(3)`으로 전환한다. `shutdown()`을 시작하면
`DRAINING(4)`으로 전환한다.

## Kotlin source signature

```kotlin
public suspend fun <T> CompletionStage<T>.await(): T
```

이 함수는 Java `CompletionStage`의 성공 값과 실패 원인을 보존한다. Coroutine 취소는 대기 중인 continuation만
끝내며 이미 시작한 shared host operation을 취소하지 않는다.

User Spot은 Spot과 current member Actor 전체를 하나의 aggregate로 이전하며 participant 총수에
고정 상한을 두지 않는다. 따라서 존재 자체가 relocation blocker가
아니다. `disableRelocation()` 선택, target 부재와 state capability 불일치의 relocation reason은 Java와 동일하다.
Local manual RouteMesh peer, ClientServer client endpoint, fanout subscriber endpoint 또는 manual fanout publisher가
하나라도 있으면 Java enum의 `MANUAL_TOPOLOGY_UNSUPPORTED(8)`로 `Blocked`된다. Automatic RouteMesh는 source의
Core peer table에서 descriptor와 같은 RID·lifecycle generation이 admitted·ready가 된 뒤에만
`RELOCATING(2)`으로 전환한다. `shutdown()`에는 manual topology 제한을 적용하지 않는다.

`PLANNED_MAINTENANCE(0)`는 `targetApplicationVersion == null`이어야 하며 source와 같은 version만
target 후보로 사용한다. `ROLLING_UPDATE(1)`는 source보다 큰 target version을 반드시 지정하고 그 값과
정확히 같은 version만 후보로 사용한다. Framework는 version, source가 아닌 `SERVING` Object Server,
stable type·factory·adapter capability, capacity와 다른 maintenance wave, RID·lifecycle generation이
일치하는 `ADMITTED` Core peer, placement weight 순서로 target을 선택한다. 다른 version으로 fallback하지
않는다. Version·wave·capacity 또는 exact-ready target이 없으면 deadline까지 다시 확인한 뒤
`BLOCKED/TARGET_UNAVAILABLE`이다. Factory·policy·adapter가 맞지 않으면
`BLOCKED/STATE_INCOMPATIBLE`, Store 조회 실패는 `BLOCKED/STORE_UNAVAILABLE`이다.

같은 mode와 effective target version의 동시 호출은 첫 호출이 시작한 shared operation에 참여하고 같은
terminal result를 받는다. 첫 options의 deadline이 shared operation deadline을 고정한다. Mode 또는 target
version이 다르면 기존 operation을 변경하지 않고 `BLOCKED/OPERATION_IN_PROGRESS(10)`를 반환한다.
Kotlin은 이 규칙을 축약하는 default mode나 별도 target 선택 extension을 제공하지 않는다.

Java와 같이 모든 target을 `Prepared`로 만들고 relocation commit을 publish하기 전 deadline은 durable abort와
source normalization 뒤 `Blocked/DeadlineExceeded`다. Commit 뒤에는 source로 rollback하지 않으며
같은 target process가 실행 중일 때만 남은 단계를 처리한다. Target process가 종료되면 다른 runtime이
relocation을 자동으로 이어받지 않으며 `Relocated`를 반환하지 않는다. `RELOCATING`에서 `shutdown()`을 호출하면 실행 중인 atomic
unit만 끝내고 relocation waiter는 `Blocked/ShutdownRequested`를 받는다. Kotlin enum이나 result를 추가하지 않는다.

```kotlin
val relocation = frameworkRuntime.relocate(
    ZLinkFrameworkRelocationOptions(
        ZLinkFrameworkRelocationMode.ROLLING_UPDATE,
        12L, // 이 version과 정확히 일치하는 Ready node만 선택한다.
        Duration.ofSeconds(30)
    )
).await()
val stopped = frameworkRuntime.shutdown().await()
```

별도 `relocateAsync`, `shutdownAsync`, `drain` 또는 `awaitStopped` extension은 없다. Relocation 결과는
`ZLinkFrameworkRelocationResult`, host 종료 결과는 `ZLinkFrameworkTerminationResult`이며 `CompletionStage.await()`로 기다린다.

## Exact generated JVM signature

아래 JVM signature는 Kotlin source contract의 generated form이다.

```java
public final class systems.zlink.framework.kotlin.ZLinkCoroutineTurnAwaitKt {
  public static final <T> java.lang.Object await(java.util.concurrent.CompletionStage<T>, kotlin.coroutines.Continuation<? super T>);
}
```
