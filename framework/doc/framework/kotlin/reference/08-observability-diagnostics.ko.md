# 08. Observability diagnostics

[레퍼런스 목차](README.ko.md)

Diagnostics 옵션 값, host 상태 field와 `ZLinkFrameworkErrorKind` 대응표는
[Java 레퍼런스 08. Observability diagnostics](../../java/reference/08-observability-diagnostics.ko.md)와
완전히 같다 — Kotlin은 Java `ZLinkFrameworkErrorKind`·`ZLinkFrameworkException`을 그대로 사용하며
별도 enum이나 exception 계층을 추가하지 않는다. 정확한 signature는
[Kotlin monitoring exact interface](../../common/spec/server/languages/kotlin/interfaces/monitoring.ko.md)가
소유한다.

---

## `configureDispatch { }` (구성 시점, DSL)

Java `ZLinkDispatchOptions`의 diagnostics level과 sampling을 receiver 형식으로 설정한다.

```kotlin
options.configureDispatch {
    messageFlow(ZLinkMessageFlowLogMode.NORMAL) // 주요 전이까지 structured record로 기록한다.
    traceSampleRate(0.1)
    includeMessageSizes(true)
}
```

**옵션.** Level은 Java와 같은 `OFF`, `ERRORS`, `NORMAL`, `DETAILED` 네 값이다.
`ERRORS`가 기본값이다. Sampling rate와 message size 포함 여부도 같은 DSL에서 설정한다.

**완료 결과.** Framework는 application이 구성한 standard logger·trace·metric provider에
structured record를 기록한다. Provider 호출 실패는 원래 message operation의 terminal 결과를
바꾸지 않고 별도 진단으로 격리한다. Kotlin은 message-flow observer, runtime error sink,
file path나 raw event DTO를 추가하지 않는다.

**선택 기준.** Startup에서 진단 상세도와 sampling을 Kotlin DSL로 묶어 설정할 때 쓴다.

---

## Host 상태·error kind

Host 상태(`ZLinkFrameworkRuntimeStatus`, `ZLinkInboundDispatchStatus`)와
`ZLinkFrameworkErrorKind` 대응표는 Java 타입을 그대로 사용한다 — Kotlin 전용 data class나 enum을
추가하지 않는다. 두 항목의 field 의미와 대응표는
[Java 레퍼런스 08번](../../java/reference/08-observability-diagnostics.ko.md)을 참고한다.

---

전체 근거는
[Kotlin monitoring exact interface](../../common/spec/server/languages/kotlin/interfaces/monitoring.ko.md)와
[Java 레퍼런스 08. Observability diagnostics](../../java/reference/08-observability-diagnostics.ko.md)를
참고한다.
