# Kotlin 공통 runtime 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Java 공통 runtime](../../java/interfaces/common-runtime.ko.md)

Kotlin은 Java의 `ZLinkTopologyState`, `ZLinkFrameworkRuntimeState`,
`ZLinkFrameworkRelocationMode`, `ZLinkFrameworkRelocationOptions`, relocation·termination
outcome·reason·result와 `ZLinkFrameworkRuntime`을 그대로 사용한다. 같은 enum, options, result wrapper와
runtime facade를 추가하지 않는다. 별도 drain facade와 MeshName을 받는 partial termination member는 없으며,
Kotlin은 Java host의 `relocate()`와 `shutdown()`을 그대로 사용한다.

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

Kotlin은 Java runtime facade가 제공하는 listener 상태 조회도 그대로 사용한다. `ZLinkListenerKind`와
`ZLinkListenerStatus`는 Java type이며, `frameworkRuntime.listenerStatus(kind, name)`은
[공통 listener 상태 조회](../../../02-channel-transport/04-network-listener-identity.ko.md#31-publisher가-확인하는-listener-상태)를
그대로 호출하고 Kotlin 전용 wrapper를 더하지 않는다.

## Kotlin source signature

```kotlin
public suspend fun <T> CompletionStage<T>.await(): T
```

이 함수는 Java `CompletionStage`의 성공 값과 실패 원인을 보존한다. Coroutine 취소는 대기 중인 continuation만
끝내며 이미 시작한 shared host operation을 취소하지 않는다.

Kotlin은 Java의 host relocation mode와 result enum을 사용한다. Target 선택, deadline,
commit과 shutdown은 [Host relocation flow](../../../05-location-relocation/05-host-relocation-flow.ko.md)가 정한다.
`CompletionStage.await()` 취소는 대기 중인 continuation에만 적용한다.

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

## generated JVM signature

아래 JVM signature는 Kotlin source contract의 generated form이다.

```java
public final class systems.zlink.framework.kotlin.ZLinkCoroutineTurnAwaitKt {
 public static final <T> java.lang.Object await(java.util.concurrent.CompletionStage<T>, kotlin.coroutines.Continuation<? super T>);
}
```
