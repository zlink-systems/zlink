# ZLink Framework Kotlin 공개 계약

이 디렉토리는 `zlink-framework-kotlin`이 Java runtime 위에 추가하는 Kotlin 전용
public contract를 소유한다. 그대로 사용하는 Java 타입과 메서드는
[Java 공개 계약](../java/README.ko.md)을 따르고 여기서 복사해 다시 정의하지 않는다.

Kotlin coroutine과 DSL 시그니처는 [기능별 interfaces](interfaces/README.ko.md)를 기준으로 한다.
Java API를 기다리는 server extension도 이 디렉토리의 정식 계약에 포함한다. Kotlin source와 contract test는 이 계약을
따라야 한다. Client Stream Connector의 coroutine wrapper와 `Flow` 표면은 별도
[Java/Kotlin Stream Connector 계약](../../../stream-connector/languages/java/03-stream-connector.ko.md)이
소유한다.

Host relocation은 Java의 mode·options·result type을 그대로 사용한다. Planned maintenance는 source와
같은 application version을, rolling update는 호출자가 지정한 더 높은 application version과 정확히
일치하는 target만 사용한다. Kotlin 전용 default mode나 target 선택 extension은 제공하지 않는다.

ChannelName 단일 호출, RouteMesh·ClientServer role builder, listener network identity, handler context와
전용 descriptor·runtime은 Java 정본 타입을 재사용하고 Kotlin DSL만 관용적으로 투영한다.

Global ActorId·SpotId, exact ActorRef·SpotRef, [User Spot](../../../01-glossary.ko.md#entry-spot-user-spot과-instance-spot) manager의 명시적인 create/get-or-create,
actor-free Instance Spot lifecycle도 Java 정본 타입을 재사용한다. Location provider는 Java의 opaque
key·value atomic batch를, Relocation provider는 Framework-issued reference 기반 immutable blob 계약을
그대로 구현한다. Kotlin은
ID-only direct call에 `send`와 `request` extension만 추가하며 Java member와 충돌하는 suspend
`requestToSpot`을 선언하지 않는다. 정확한 extension과 Store type 재사용은
[기능별 interfaces](interfaces/README.ko.md)가 고정한다.

공유 JVM runtime은 Java binding의 public raw socket API로 placement와 activation barrier를 구현한다.
Core service driver, private binding 진입점과 별도 Kotlin runtime은 사용하지 않는다. Ready owner 호출은
global ID로 current [authority](../../../01-glossary.ko.md#authority)를 resolve하며 process-local handle이나 별도 address를 사용하지 않는다.

공식 Redis location extension의 Kotlin 호출 경계와 Java type 재사용 규칙은
[Location과 maintenance](interfaces/location-maintenance.ko.md)가 고정한다.

## 취소 인자

Kotlin application callback과 call interface에는 framework `CancellationToken`이나 같은 목적의 별도 취소
인자를 두지 않는다. `suspend` 함수는 호출한 coroutine의 lifecycle을 따르며, 이 동작을
별도 token parameter로 중복 표현하지 않는다. timeout, host shutdown과 resource cleanup은
각 기능 계약을 따른다.

Java provider·adapter ABI에서 재사용하는 `ZLinkStoreCancellation`과 `ZLinkRelocationCancellation`은 Kotlin
lifecycle token이 아니라 해당 SPI operation의 fence다. Kotlin suspending lifecycle callback에는 이 타입을
투영하지 않는다.
