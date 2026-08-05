# Kotlin public interface 정식 계약

[Kotlin 계약 목차](../README.ko.md) · [Java interface](../../java/interfaces/README.ko.md)

Kotlin package는 Java와 JVM service runtime을 공유한다. 아래 문서는 Java type을 그대로 쓰는 범위와 Kotlin
전용 coroutine·DSL signature를 기능별로 고정한다.

- [공통 runtime](common-runtime.ko.md)
- [구성과 host](configuration-host.ko.md)
- [Channel messaging](channel-messaging.ko.md)
- [Spot](spots.ko.md)
- [Actor](actors.ko.md)
- [STREAM session](stream-session.ko.md)
- [Location과 maintenance](location-maintenance.ko.md)
- [Monitoring](monitoring.ko.md)

## 공개 API 구조

Kotlin application은 Java의 lifecycle, termination, factory relocation builder와 Location type을 직접 사용한다. Kotlin
package는 coroutine handler, suspending call, reified registration과 구성 DSL을 제공하며 같은 의미의 runtime
facade나 상태 type을 중복해서 정의하지 않는다.

Actor·Spot state 보존 adapter도 Java `ZLinkActorRelocationAdapter`와 `ZLinkSpotRelocationAdapter`가 정본이다. Kotlin은
`byte[]`를 `ByteArray`로 투영하고 `CompletionStage` completion을 그대로 사용하며 별도 state DTO,
state contract ID, suspending adapter와 reified state 보존 policy를 정의하지 않는다. Entry Spot의 coroutine
lifecycle class는 infrastructure membership relocation callback을 추가하지 않는다.
`SpotWide` application-signaled 경계의 completion만
`onRelocationReadyCompletedSuspending(...)` 기본 no-op bridge로 제공한다.

Channel extension은 process-local ChannelName만 받으며 MeshName과 [ChannelName](../../../../01-glossary.ko.md#channelname)을 함께 받는 선택 overload를
추가하지 않는다. Host `Relocate`·`Shutdown`은 Java의 relocation mode·options·result type을 그대로
사용하며 별도 drain facade를
제공하지 않는다. Location Store의 opaque key·value atomic batch와 Relocation Store의
Framework-issued reference 기반 immutable blob 계약도 Java public interface가 정본이다.

각 기능 문서는 Kotlin source signature와 application이 실제로 link하는 generated JVM signature를 구분한다.
Default argument, suspend continuation, extension receiver와 generic bound는 두 표현 사이에서 손실 없이 대응해야
한다. Node를 직접 지정하는 extension의 첫 번째 `String` 인자는 Java 계약과 같이 [MeshName](../../../../01-glossary.ko.md#meshname)이다.

Public generation, revision, epoch와 sequence ordinal은 Java 계약의 양수 `Long` 범위를 그대로 사용한다.
유효 범위는 `1..Long.MAX_VALUE`이며 최대값에서는 wrap이나 값 재사용 없이 terminal exhaustion으로 처리한다.
`0`은 값이 확정되지 않은 상태를 표현하도록 해당 계약이 명시한 경우에만 사용한다.

## RouteMesh object runtime 기준

Kotlin exact interface는 Java와 같은 global ActorId·SpotId, immutable `ActorRef`·`SpotRef`, ID-only 일반
messaging과 exact-ref mutation·session bind를 사용한다. Actor와 User Spot의 create/get-or-create는 single-user
fluent operation이다. [Spot](../../../../01-glossary.ko.md#spot) manager는 User Spot 전용이며 Instance Spot creation member를 제공하지 않는다.
Missing [Instance Spot](../../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot)의 cold activation은 Spot 전용 send/request call에서 `instanceSpot()` 또는
`instanceSpot(stableType)`을 명시한 경우에만 시작한다. Marker가 없으면 not-found이고, marker만 사용한 cold
activation은 selected Mesh의 distinct serving Instance type이 하나일 때만 type을 자동 선택한다. Existing
[authority](../../../../01-glossary.ko.md#authority)는 등록 type 수와 관계없이 저장된 type을 사용한다. Mesh object role은 None, Client, Server로
구분한다. 모든 server factory configure callback은 relocation 동작을 정확히 하나 선택한다. Kotlin extension은
이 계약을 축약하거나 local fallback을 추가하지 않는다.

Global ref의 JSON field는 `actorId` 또는 `spotId`, `objectGeneration`, `meshName`, `nodeRid`다.
`objectGeneration`은 decimal string이며 unknown field, duplicate field, required field 누락,
0 또는 `Long.MAX_VALUE` 초과 값은 거부한다. String identity는 exact value를 유지하며 normalization하지 않는다.
허용하는 shape는 다음 두 가지다. `objectGeneration`은 leading zero가 없는
`"1"`..`"9223372036854775807"`이고 JSON number token은 거부한다.

```json
{"actorId":"actor-7","objectGeneration":"41","meshName":"game","nodeRid":"game-0123456789abcdef0123456789abcdef"}
```

```json
{"spotId":"room-7","objectGeneration":"42","meshName":"game","nodeRid":"game-0123456789abcdef0123456789abcdef"}
```
