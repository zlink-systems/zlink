# 08. Observability diagnostics

[레퍼런스 목차](README.ko.md)

Diagnostics 옵션 값, host 상태 field와 `ZLinkFrameworkErrorKind` 대응표는
[Java 레퍼런스 08. Observability diagnostics](../../java/reference/08-observability-diagnostics.ko.md)와
완전히 같다 — Kotlin은 Java `ZLinkFrameworkErrorKind`·`ZLinkFrameworkException`을 그대로 사용하며
별도 enum이나 exception 계층을 추가하지 않는다. Kotlin이 추가하는 것은 message-flow observer
등록을 receiver lambda로 감싸는 확장 함수 하나뿐이다. 정확한 signature는
[Kotlin monitoring exact interface](../../common/spec/server/languages/kotlin/interfaces/monitoring.ko.md)가
소유한다.

---

## `onMessageFlow { }` (구성 시점, DSL)

`ZLinkDispatchOptions.setMessageFlowObserver(...)`를 람다로 감싼 확장 함수다.

```kotlin
options.configureDispatch {
    onMessageFlow { event: ZLinkMessageFlowEvent ->
        logger.info("flow: ${event.outcome()} ${event.packetName()}")
    }
}
```

**옵션.** `observer: (ZLinkMessageFlowEvent) -> Unit`를 받는다. `ZLinkDispatchOptions`를 반환하므로
`configureDispatch { }` DSL(topology-discovery category) 안에서 다른 modifier와 이어 쓸 수 있다.

**완료 결과.** Java `ZLinkMessageFlowObserver`를 등록하는 것과 완전히 같은 효과다 — 반환값 없이
동기로 등록된다.

**선택 기준.** Java 레퍼런스 08번의 `configureDispatch().diagnostics()` 항목과 같은 상황에서,
observer를 별도 클래스로 만들지 않고 람다로 바로 쓰고 싶을 때 이 확장 함수를 쓴다.

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
