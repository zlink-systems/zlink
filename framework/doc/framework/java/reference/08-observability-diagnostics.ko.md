# 08. Observability diagnostics

[레퍼런스 목차](README.ko.md)

이 category는 trace·metric·log 기록 수준을 구성하는 `ZLinkDispatchOptions`/
`ZLinkDiagnosticsOptions`, host·topology 상태를 읽는 `ZLinkFrameworkRuntime`(host-lifecycle
category와 공유), 그리고 모든 category의 실패를 판단하는 `ZLinkFrameworkErrorKind` 대응표를
다룬다. 정확한 signature는
[Java 구성과 host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.ko.md)와
[Java 공통 runtime exact interface](../../common/spec/server/languages/java/interfaces/common-runtime.ko.md)가
소유한다.

---

## `configureDispatch().diagnostics()` (구성 시점)

Trace·metric 기록 수준과 sampling을 설정한다.

```java
options.configureDispatch()
    .messageFlow(ZLinkMessageFlowLogMode.KEY_TRANSITIONS)
    .traceSampleRate(0.1)
    .includeMessageSizes(true);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.messageFlow(ZLinkMessageFlowLogMode)` | 구현 기본값 | `OFF`/`ERRORS_ONLY`/`KEY_TRANSITIONS`/`VERBOSE`/`DIAGNOSTIC` 중 기록할 상세도 |
| `.traceSampleRate(double)` | 구현 기본값 | `0.0`..`1.0`. 범위를 벗어나면 configuration error |
| `.includeMessageSizes(boolean)` | `false` | Payload 크기 분포를 telemetry에 포함할지 여부. Payload 내용 자체는 절대 기록하지 않는다 |
| `.traceLogFile(path)` | 없음 | Diagnostics 기록을 남길 파일 경로 |
| `.traceLabel(id)` | 없음 | Diagnostics 기록에 붙일 label |
| `.setMessageFlowObserver(observerType)` / `.setMessageFlowObserver(ZLinkMessageFlowObserver)` | 없음 | `ZLinkMessageFlowEvent`를 받는 observer 등록 |

각 modifier는 `ZLinkDispatchOptions`를 반환하는 동기 fluent 호출이다 — 반환값 없는 등록이 아니다.
`unhandled()`가 반환하는 `ZLinkUnhandledDispatchOptions`로 handler가 없는 request/send/publish의
처리 방식(`REPLY_ERROR`/`LOG_AND_DROP`/`DROP`/`THROW`)도 이 builder에서 함께 설정한다.

**완료 결과.** Trace·metric·log 기록 대상(exporter, 원격 backend)은 application이 별도로 구성한다
(예: Micrometer `MeterRegistry`를 구성하는 `ZLinkMetricsCustomizer` bean).

**선택 기준.** Startup 시점에 기본 기록 수준을 정할 때 쓴다.

---

## `ZLinkFrameworkRuntime.status` / `observe` (읽기·관찰)

Host 전체 상태(lifecycle state, relocation·termination 결과, inbound dispatch backpressure)를
조회하거나 관찰한다. 정확한 signature와 예제는 host-lifecycle category의 `status`/`observe` 항목이
소유한다.

**선택 기준.** `ZLinkFrameworkRuntimeStatus.inboundDispatch()`(`ZLinkInboundDispatchStatus`)로
application HWM 사용량과 backpressure 상태를 확인한다. 특정 MeshName·ChannelName의 가용성은
topology-discovery category의 상태 조회 항목을 쓴다.

---

## `ZLinkFrameworkErrorKind` 대응표

Framework operation이 실패하면 `ZLinkFrameworkException.kind()`로 원인 계열을 판단한다. 이 표는
모든 category의 완료 kind 설명이 공유하는 근거다.

| Kind | Application에서 확인할 내용 |
| --- | --- |
| `NOT_FOUND` | 요청한 Actor, Spot, handler, route 또는 target이 존재하는지 확인한다 |
| `ALREADY_EXISTS` | create와 registration이 멱등하게 처리되어야 하는지 확인한다 |
| `TYPE_MISMATCH` | stable type과 요청한 application type이 일치하는지 확인한다 |
| `NOT_CONFIGURED` | 필요한 role, handler, Store 또는 object client가 startup에 등록되었는지 확인한다 |
| `REJECTED` | Typed 결과가 없는 Framework admission, filter 또는 runtime policy가 operation을 거부했다 |
| `UNAVAILABLE` | target, route, Store 또는 worker가 현재 operation을 처리할 수 없다 |
| `CAPACITY_EXCEEDED` | placement, queue 또는 bounded resource의 여유가 없다 |
| `DEADLINE_EXCEEDED` | operation이 정한 deadline 안에 완료되지 않았다. 결과의 side effect 여부는 해당 operation 계약을 따른다 |
| `SHUTTING_DOWN` | runtime이 신규 admission을 받지 않는 상태다. 다른 serving instance를 사용해야 한다 |
| `PROTOCOL_ERROR` | peer와 protocol 또는 reply 계약이 일치하는지 확인한다 |
| `INVALID_OPERATION` | 현재 object·session·runtime 상태에서는 요청한 operation이 허용되지 않는다 |
| `DATA_LOST` | 공개된 relocation payload를 찾을 수 없거나 검증에 실패했다. 이전 owner로 임의 rollback하지 않는다 |
| `INTERNAL_FAILURE` | 위 분류로 표현할 수 없는 Framework 실패다. Log와 trace의 correlation 정보로 원인을 확인한다 |

**완료 결과.** `ZLinkFrameworkException`은 Framework만 생성하며, `value()`는 언어와 무관한 공통
숫자 `0..12`를 반환한다(`fromValue(int)`로 역변환). `ZLinkConfigurationException`(startup 검증
실패)은 `ZLinkFrameworkException`의 subtype이지만 startup 전용 오류라는 별도 층이다. `Message`는
사람이 진단하기 위한 설명이지 programmatic 분기 대상이 아니다. 재시도 여부는 이 kind가 알려주지
않는다 — operation의 완료 조건, idempotency와 업무 상태를 확인해 application이 직접 판단한다.

**선택 기준.** 각 category 항목의 "완료 결과"에 나온 kind를 이 표로 되짚어 대응 방법을 정할 때
쓴다.

---

전체 근거는
[Java 구성과 host exact interface](../../common/spec/server/languages/java/interfaces/configuration-host.ko.md)와
[Java 공통 runtime exact interface](../../common/spec/server/languages/java/interfaces/common-runtime.ko.md)를
참고한다.
