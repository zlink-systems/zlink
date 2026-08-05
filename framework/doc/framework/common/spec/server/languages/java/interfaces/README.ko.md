# Java public interface 정식 계약

[Java 계약 목차](../README.ko.md)

이 디렉토리는 Java server package의 정확한 public signature를 기능별로 고정한다. 공통 동작은
[server 공통 spec](../../../../README.ko.md)이 소유한다.

- [공통 runtime](common-runtime.ko.md)
- [구성과 host](configuration-host.ko.md)
- [Channel messaging](channel-messaging.ko.md)
- [Spot](spots.ko.md)
- [Actor](actors.ko.md)
- [STREAM session](stream-session.ko.md)
- [Location과 maintenance](location-maintenance.ko.md)
- [Monitoring](monitoring.ko.md)

Java와 Kotlin은 JVM service runtime 하나를 공유한다. Java 계약에는 Kotlin coroutine wrapper를 넣지 않으며,
Kotlin 계약은 Java type을 재사용하는지 또는 Kotlin 전용 extension을 제공하는지를 별도로 명시한다.

## 공개 API 구조

Java application은 `ZLinkFrameworkOptions`에서 host와 topology를 구성하고, Channel·Spot·Actor·STREAM client와
handler 계약으로 메시지를 처리한다. `ZLinkFrameworkRuntime`의 mode가 명시된 `Relocate`와 `Shutdown`이
object relocation과 host 종료를 각각 소유하고,
MeshName을 받는 partial termination operation은 제공하지 않는다. Location provider는 Framework가
만드는 opaque record에 대한 read, version 조건부 atomic batch와 bounded snapshot scan을 제공한다.
Relocation provider는 Framework가 미리 발급한 reference에 immutable blob을 저장한다.

`ZLinkTopologyState`는 등록한 topology의 가용성을, `ZLinkFrameworkRuntimeState`는 host 전체 상태를 나타낸다. Channel
호출은 process-local ChannelName만 받는다. Node를 직접 지정하는
`sendToNode(String, RoutingId, Object)`의 첫 번째 인자는 [MeshName](../../../../01-glossary.ko.md#meshname)이다.

ActorId와 User·Instance SpotId는 global logical ID다. 일반 message는 ID만 받고 current [authority](../../../../01-glossary.ko.md#authority)를
resolve하며 exact mutation과 session bind는 `ActorRef` 또는 `SpotRef`를 받는다. [MeshNode](../../../../01-glossary.ko.md#meshnode) object role은
`None`, `Client`, `Server`로 닫혀 있고 Client·Server는 Location Store가 필수다.

정확한 type, constructor, method, record component, enum value와 generic bound는 위 기능별 문서가 소유한다.
Core·bindings의 내부 type과 `runtime.internal` type은 application public signature에 노출하지 않는다.
Spring starter는 public bean의 type, singleton 수명과 identity만 계약으로 제공한다. Auto-configuration class,
bean factory method와 lifecycle adapter의 concrete type은 exact public interface에 포함하지 않는다.

Public generation, revision, epoch와 sequence ordinal의 유효 범위는 양수 `long`, 즉
`1..Long.MAX_VALUE`다. 최대값에 도달하면 Framework는 wrap이나 값 재사용 없이 terminal exhaustion으로
처리한다. `0`은 값이 확정되지 않은 상태를 표현하도록 해당 계약이 명시한 경우에만 사용한다.
