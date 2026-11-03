# ZLink Framework Java 공개 계약

<!-- framework-adapter-nav:start -->
[언어별 interface 목차](../README.ko.md) | [스펙 목차](../../README.ko.md)
<!-- framework-adapter-nav:end -->
이 디렉토리는 Java framework가 제공해야 하는 **정식 public contract**를 소유한다. 구현과
regression test는 이 계약을 따라야 한다.

Kotlin이 Java 계약을 그대로 사용하는 경우 이 문서를 따르며, Kotlin 전용 `suspend`와 `Flow`
표면은 [Kotlin 공개 계약](../kotlin/README.ko.md)이 별도로 고정한다.

Channel 호출은 process-local ChannelName만 사용한다. Object relocation은 mode와 target application
version을 명시하는 `Relocate`, host 종료의 단일 기준은 `Shutdown`이다. Planned maintenance는 source와 같은
version으로, rolling update는 호출자가 지정한 더 높은 version으로만 이동한다.
Location provider는 opaque record의 atomic 저장 primitive를 제공하고 Relocation provider는
Framework-issued reference에 immutable blob을 저장한다.

| 문서 | 범위 |
|---|---|
| [기능별 interfaces](interfaces/README.ko.md) | runtime, 구성, Channel, Spot, Actor, STREAM, Location·maintenance와 monitoring의 정확한 signature |
| [Stream Connector](../../../stream-connector/languages/java/03-stream-connector.ko.md) | client connector의 public 표면 |

**기능의 의미와 동작 규칙은 [공통 스펙](../../README.ko.md)이 소유한다.** 이 디렉토리는 그 의미가
이 언어에서 갖는 **정확한 public API**만 고정한다.

## 취소 표현

Java lifecycle callback과 host operation에 .NET `CancellationToken`을 모방한 범용 Framework token을 추가하지
않는다. `CompletionStage` waiter cancellation은 이미 시작한 shared operation을 중단하지 않으며, [Spot](../../00-foundation/02-glossary.ko.md#spot) closing은
context의 absolute deadline에 Framework가 stage completion 대기를 끝내는 방식으로 제한한다.

`ZLinkRelocationCancellation`, `ZLinkStoreCancellation`과 `ZLinkWorkerCancellation`은 범용 lifecycle token이
아니다. 각각 stale relocation attempt의 adapter completion 차단, provider I/O의 operation cancellation과 CPU·I/O
worker 실행 중단만 표현하는 SPI 전용 타입이다. 이 타입을 handler, Spot lifecycle, host termination이나 일반
message API에 재사용하지 않는다.

## Messaging hot path 전환 수 계측

[Messaging hot path §3](../../01-execution/08-messaging-hot-path.ko.md)는 binding 완료 알림부터 caller continuation의 첫 instruction까지 Framework가 만드는 전환을 최대 1회로 제한한다. unit 시험은 binding callback과 continuation의 thread identity가 달라진 횟수를 세고, completion dispatcher turn 표식으로 그 전환이 Framework worker에서 일어났는지 확인한다. task 게시와 깨우기는 실행 자원 전환 수에 더하지 않는다.

| 구간 | 상한 | 계측 지점 | 시험 식별자 |
|---|---:|---|---|
| binding 완료 알림 → caller continuation 첫 instruction | 1회 이하 | 시험 전용 identity 비교 [ZLinkOutsideLaneResumptionTest.java:48](../../../../../../../languages/java/zlink-framework-core/src/test/java/systems/zlink/framework/execution/ZLinkOutsideLaneResumptionTest.java#L48); [ZLinkServiceCompletionDispatcher.java:53](../../../../../../../languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceCompletionDispatcher.java#L53) | `unit:ZLinkOutsideLaneResumptionTest.messagingHotPathCompletionTransitionCountIsOne` (기대 1회) |

---
<!-- framework-adapter-nav:bottom:start -->
[언어별 interface 목차](../README.ko.md) | [스펙 목차](../../README.ko.md)
<!-- framework-adapter-nav:bottom:end -->
