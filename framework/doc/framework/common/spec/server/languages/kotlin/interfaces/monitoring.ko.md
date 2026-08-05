# Kotlin monitoring 공개 인터페이스

[인터페이스 목차](README.ko.md) · [Java monitoring](../../java/interfaces/monitoring.ko.md)

Kotlin은 Java의 RouteMesh, ClientServer, automatic fanout과 host runtime status type을 사용한다.
`Flow` projection은 Java publisher를 coroutine cancellation에 연결할 뿐 별도 status나 event value를
정의하지 않는다. 각 항목은 일부 field만 담은 event가 아니라 변경 뒤의 완전한 status다.

Host의 inbound dispatch 상태도 Java `ZLinkInboundDispatchStatus`를 그대로 사용한다. Kotlin 전용
data class나 `Flow` 집계를 추가하지 않는다. 모든 byte 값은 음수가 아닌 Java `long`이고
`pendingPayloadBytes == queuedPayloadBytes + activePayloadBytes`를 만족한다.

Endpoint, lifecycle generation과 descriptor source는 Framework가 stale descriptor와 connection을
판정할 때만 유지한다. Admission·claim·reservation, pending work와 connection intent도 Kotlin
projection에 추가하지 않는다.

RouteMesh peer는 Java `ZLinkPeerState`를 그대로 사용한다. `NOT_CONNECTED`는 연결이
필요하지만 ready connection이 없는 상태이고, `NOT_REQUIRED`는 두 Object Client 모두 RouteMesh
Channel Server membership이 없어 연결이 필요하지 않은 정상 상태다. Channel Client membership만
등록한 경우도 같다. 어느 한쪽에라도 weight `0`을 포함한 Channel Server membership이 있으면 연결
부재를 `NOT_CONNECTED`로 표시한다. 두 상태를 Kotlin 전용 boolean이나 문자열로 합치지 않는다.
`NOT_REQUIRED`는 ready peer 수, liveness·health failure 집계에서 제외한다.

Topology runtime은 Java `ZLinkFrameworkRuntime`의 `routeMeshRuntime()`, `clientServerRuntime()`과
`fanoutRuntime()`을 그대로 사용한다. Kotlin wrapper accessor를 추가하지 않으며 Spring에서 주입받은 topology
bean은 해당 Java accessor의 반환값과 reference identity가 같다.
Java monitoring 계약과 마찬가지로 MeshNode status에는 Logical Multicast 통계, publish target 수 또는
target별 수락·실패 field가 없다. Kotlin 전용 projection으로 이를 추가하지 않는다.

ClientServer target과 fanout publisher는 Java `ZLinkPeerState`와 `ZLinkTopologyReason`을 그대로
사용한다. Kotlin 전용 connection 상태 enum을 만들지 않는다.
같은 ChannelName에 Client와 Server를 함께 등록한 [snapshot](../../../../01-glossary.ko.md#snapshot)의 local role은 Java
`ZLinkClientServerRole.CLIENT_AND_SERVER`로 나타낸다. 이는 별도 role registration 두 개의 aggregate
projection일 뿐 builder role이나 registration key가 아니다. Kotlin 전용 enum이나 변환 값을 만들지 않는다.

Fanout ready 의미도 Java 계약을 그대로 사용한다. Publisher 전용 SUB socket의 native-ready만으로
[ready](../../../../01-glossary.ko.md#ready)가 되지 않으며, 같은 socket에서 첫 valid application
record 또는 liveness beacon까지 받아야 한다. 15초 inbound timeout은 해당 publisher의 peer state를
`NOT_CONNECTED`로 바꾼다.

Runtime 내부 callback이나 observer에서 발생한 오류는 Framework가 structured log로 기록한다.
Kotlin application이 구현하거나 등록하는 error sink와 raw event DTO는 public contract가 아니다.

[RouteMesh](../../../../01-glossary.ko.md#routemesh) placement status는 새 object 수락 가능 여부와
현재 process의 active Actor·Spot 수만 제공한다. Node-wide placement weight, stable type별 capacity,
pending activation과 reservation failure는 내부 배치 판단 값이므로 공개하지 않는다.
`isAvailable`은 host가 `SERVING`이고 Object Server이며, placement weight가 양수이고, Actor 또는
Spot capacity와 activation concurrency에 모두 여유가 있을 때만 `true`다. Activation의 현재 값과
limit은 Kotlin projection에도 추가하지 않는다.

## Framework 오류 값

Kotlin은 Java `ZLinkFrameworkErrorKind`를 그대로 사용한다. Enum 이름과 숫자는 public exception
분류의 일부이며 다음 값을 고정한다.

```text
NOT_FOUND = 0
ALREADY_EXISTS = 1
TYPE_MISMATCH = 2
NOT_CONFIGURED = 3
REJECTED = 4
UNAVAILABLE = 5
CAPACITY_EXCEEDED = 6
DEADLINE_EXCEEDED = 7
SHUTTING_DOWN = 8
PROTOCOL_ERROR = 9
INVALID_OPERATION = 10
DATA_LOST = 11
INTERNAL_FAILURE = 12
```

Remote framework error는 `ZLinkFrameworkException`으로 전달한다. Public argument validation은 JVM 표준
`IllegalArgumentException`, startup 구성 충돌은 `ZLinkConfigurationException`을 사용한다.
Public exception은 재시도 여부를 제공하지 않는다.

## Kotlin source signature

```kotlin
fun ZLinkDispatchOptions.onMessageFlow(
    observer: (ZLinkMessageFlowEvent) -> Unit,
): ZLinkDispatchOptions
```

Java `Publisher` status stream을 Kotlin `Flow`로 읽을 때는
[Location과 maintenance](location-maintenance.ko.md)가 소유하는 공통 `asFlow()` bridge를 사용한다.
이 bridge의 cancellation은 해당 subscriber 등록만 해제한다. 공유 runtime, monitoring publisher
또는 이미 시작한 host operation을 취소하지 않는다. `onMessageFlow` generated JVM member는
[구성과 host](configuration-host.ko.md)의 multifile class inventory에 포함한다.
