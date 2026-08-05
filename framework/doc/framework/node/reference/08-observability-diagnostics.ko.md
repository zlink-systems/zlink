# 08. Observability diagnostics

[레퍼런스 목차](README.ko.md)

이 category는 trace·metric·log 기록 수준을 구성하는 `ZLinkDispatchOptionsBuilder`/
`ZLinkDiagnosticsOptions`, 모든 category의 실패를 판단하는 `ZLinkFrameworkErrorKind` 대응표와
handler filter 계약을 다룬다. 정확한 signature는
[기초 타입과 구성 exact interface](../../common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md)와
[Location 운영 조회와 observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.ko.md)가
소유한다.

---

## `configureDispatch()` (구성 시점)

Trace·metric 기록 수준과 sampling을 설정한다.

```ts
zlinkFramework().configureDispatch()
  .messageFlow(ZLinkMessageFlowLogMode.KeyTransitions)
  .traceSampleRate(0.1)
  .includeMessageSizes(true);
```

**옵션.** 이 호출에는 다음 modifier가 붙는다.

| Modifier | 기본값 | 의미 |
| --- | --- | --- |
| `.messageFlow(mode)` | 구현 기본값 | `Off`/`ErrorsOnly`/`KeyTransitions`/`Verbose`/`Diagnostic` 중 기록할 상세도 |
| `.traceSampleRate(rate)` | 구현 기본값 | `0.0`..`1.0`. 범위를 벗어나면 configuration error |
| `.includeMessageSizes(include)` | `false` | Payload 크기 분포를 telemetry에 포함할지 여부. Payload 내용 자체는 절대 기록하지 않는다 |
| `.traceLogFile(path)` | 없음 | Diagnostics 기록을 남길 파일 경로 |
| `.traceLabel(label)` | 없음 | Diagnostics 기록에 붙일 label |
| `.setMessageFlowObserver(observerType: Type<ZLinkMessageFlowObserver>)` | 없음 | Message-flow event를 받는 observer 등록 |
| `.setRuntimeErrorSink(sinkType: Type<ZLinkRuntimeErrorSink>)` | 없음 | Runtime 내부 callback·observer 오류를 받는 sink 등록 |

각 modifier는 `ZLinkDispatchOptionsBuilder`를 반환하는 동기 fluent 호출이다 — 반환값 없는 등록이
아니다. `zlinkFramework().options({ dispatch: { unhandled, diagnostics } })`로도 같은 값을 한
번에 지정할 수 있다.

**완료 결과.** 반환값 없이 동기로 등록된다. `unhandled`(요청 없는 request/send/publish 처리
방식: `ReplyError`/`LogAndDrop`/`Drop`/`Throw`)도 같은 `ZLinkDispatchOptions`에 속한다.

**선택 기준.** Startup 시점에 기본 기록 수준을 정할 때 쓴다.

---

## `ZLinkHandlerFilterContext` (filter 안에서 읽기)

Handler filter(topology-discovery category의 `filters` 항목)가 dispatch 종류와 공개 metadata를
읽는다.

```ts
async invoke(context: ZLinkHandlerFilterContext, next: ZLinkHandlerFilterNext) {
  if (context.dispatchKind === ZLinkHandlerDispatchKind.ChannelRequest) { ... }
  await next();
}
```

**옵션.** `ZLinkMessageContext`를 상속해 `dispatchKind`(`NodeDirectSend`/`NodeDirectRequest`/
`ChannelSend`/`ChannelRequest`/`ClassicFanout`)를 추가로 제공한다. `ChannelSend`/`ChannelRequest`는
RouteMesh와 ClientServer를 모두 포함한다. RouteMesh와 Node direct는 `meshName`을 제공하고,
ClientServer와 classic fanout은 제공하지 않는다.

**완료 결과.** 읽기 전용 property 접근이다 — 별도 완료 kind가 없다.

**선택 기준.** Filter 안에서 어떤 dispatch 경로인지 분기해야 할 때 쓴다.

---

## `ZLinkFrameworkErrorKind` 대응표

Framework operation이 실패하면 `ZLinkFrameworkException.kind`로 원인 계열을 판단한다. 이 표는
모든 category의 완료 kind 설명이 공유하는 근거다.

| Kind | Application에서 확인할 내용 |
| --- | --- |
| `NotFound` | 요청한 Actor, Spot, handler, route 또는 target이 존재하는지 확인한다 |
| `AlreadyExists` | create와 registration이 멱등하게 처리되어야 하는지 확인한다 |
| `TypeMismatch` | stable type과 요청한 application type이 일치하는지 확인한다 |
| `NotConfigured` | 필요한 role, handler, Store 또는 object client가 startup에 등록되었는지 확인한다 |
| `Rejected` | Typed 결과가 없는 Framework admission, filter 또는 runtime policy가 operation을 거부했다 |
| `Unavailable` | target, route, Store 또는 worker가 현재 operation을 처리할 수 없다 |
| `CapacityExceeded` | placement, queue 또는 bounded resource의 여유가 없다 |
| `DeadlineExceeded` | operation이 정한 deadline 안에 완료되지 않았다. 결과의 side effect 여부는 해당 operation 계약을 따른다 |
| `ShuttingDown` | runtime이 신규 admission을 받지 않는 상태다. 다른 serving instance를 사용해야 한다 |
| `ProtocolError` | peer와 protocol 또는 reply 계약이 일치하는지 확인한다 |
| `InvalidOperation` | 현재 object·session·runtime 상태에서는 요청한 operation이 허용되지 않는다 |
| `DataLost` | 공개된 relocation payload를 찾을 수 없거나 검증에 실패했다. 이전 owner로 임의 rollback하지 않는다 |
| `InternalFailure` | 위 분류로 표현할 수 없는 Framework 실패다. Log와 trace의 correlation 정보로 원인을 확인한다 |

**완료 결과.** `ZLinkFrameworkException`은 Framework만 생성하며 `message`는 사람이 진단하기 위한
설명이지 programmatic 분기 대상이 아니다. `ZLinkConfigurationException`(startup 검증 실패)과
`TypeError`(잘못된 인자)는 이 kind 분류와 다른 층이다. 재시도 여부는 이 kind가 알려주지 않는다 —
operation의 완료 조건, idempotency와 업무 상태를 확인해 application이 직접 판단한다.

**선택 기준.** 각 category 항목의 "완료 결과"에 나온 kind를 이 표로 되짚어 대응 방법을 정할 때
쓴다.

---

전체 근거는
[기초 타입과 구성 exact interface](../../common/spec/server/languages/node/interfaces/01-foundation-configuration.ko.md)와
[Location 운영 조회와 observability exact interface](../../common/spec/server/languages/node/interfaces/03-location-observability.ko.md)를
참고한다.
