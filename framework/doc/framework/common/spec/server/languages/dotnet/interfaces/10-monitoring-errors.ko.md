# .NET Framework 오류 공개 인터페이스

[.NET exact interface 목차](README.ko.md) ·
[공통 Framework API](../../../../06-framework-api.ko.md) ·
[Message flow](../../../../26-message-flow-tracing.ko.md)

## 1. 범위

이 문서는 .NET application이 Framework operation 실패를 처리할 때 사용하는 public exception과
안정된 오류 분류를 고정한다. 오류 종류는 실패가 발생한 내부 함수나 state machine 단계가 아니라,
application이 선택할 수 있는 대응 방법을 기준으로 구분한다.

Authority owner, fence, generation, descriptor revision, relocation staging, worker queue와 timer scheduler의
내부 상태는 public 오류 계약에 포함하지 않는다. 세부 원인과 stack trace는 .NET logging과 tracing에
기록한다.

## 2. Framework 오류

잘못된 public 인자는 .NET 표준 `ArgumentException` 계열로 거부한다. Startup configuration이 유효하지
않으면 `ZLinkConfigurationException`이 발생한다. 실행 중 request, lifecycle과 one-way operation에서
발생한 Framework 실패는 `ZLinkFrameworkException`으로 전달한다.

```csharp
public enum ZLinkFrameworkErrorKind
{
    NotFound = 0,
    AlreadyExists = 1,
    TypeMismatch = 2,
    NotConfigured = 3,
    Rejected = 4,
    Unavailable = 5,
    CapacityExceeded = 6,
    DeadlineExceeded = 7,
    ShuttingDown = 8,
    ProtocolError = 9,
    InvalidOperation = 10,
    DataLost = 11,
    InternalFailure = 12
}

public sealed class ZLinkFrameworkException : Exception
{
    public ZLinkFrameworkErrorKind Kind { get; }
}

public sealed class ZLinkConfigurationException : InvalidOperationException
{
}
```

`ZLinkFrameworkException`은 Framework만 생성한다. Application은 exception을 상속하거나 오류 분류를
변경하지 않는다. `ZLinkConfigurationException`도 Framework가 startup validation 결과로 생성한다.
`Message`는 사람이 진단하기 위한 설명이며 programmatic 분기에 사용하지 않는다.

Public exception은 재시도 여부를 제공하지 않는다. Application은 operation의 완료 조건, idempotency와
업무 상태를 확인해 새 operation을 시작할지 결정한다.

## 3. 오류 kind 의미

| Kind | Application에서 확인할 내용 |
|---|---|
| `NotFound` | 요청한 Actor, Spot, handler, route 또는 target이 존재하는지 확인한다. |
| `AlreadyExists` | create와 registration이 멱등하게 처리되어야 하는지 확인한다. |
| `TypeMismatch` | stable type과 요청한 application type이 일치하는지 확인한다. |
| `NotConfigured` | 필요한 role, handler, Store 또는 object client가 startup에 등록되었는지 확인한다. |
| `Rejected` | Typed 결과가 없는 Framework admission, filter 또는 runtime policy가 operation을 거부했다. |
| `Unavailable` | target, route, Store 또는 worker가 현재 operation을 처리할 수 없다. |
| `CapacityExceeded` | placement, queue 또는 bounded resource의 여유가 없다. |
| `DeadlineExceeded` | operation이 정한 deadline 안에 완료되지 않았다. 결과의 side effect 여부는 해당 operation 계약을 따른다. |
| `ShuttingDown` | Runtime이 신규 admission을 받지 않는 상태다. 다른 serving instance를 사용해야 한다. |
| `ProtocolError` | peer와 protocol 또는 reply 계약이 일치하는지 확인한다. |
| `InvalidOperation` | 현재 object·session·runtime 상태에서는 요청한 operation이 허용되지 않는다. |
| `DataLost` | 공개된 relocation payload를 찾을 수 없거나 검증에 실패했다. 이전 owner로 임의 rollback하지 않는다. |
| `InternalFailure` | 위 분류로 표현할 수 없는 Framework 실패다. Log와 trace의 correlation 정보로 원인을 확인한다. |

Generation stale, object moving, worker queue 상태와 relocation 처리 단계는 Framework가 내부 처리를
판정할 때 사용하는 원인이다. Application이 다른 대응을 선택할 수 없으면 별도 public kind로
구분하지 않는다.

## 4. Diagnostics 경계

Framework는 application message의 흐름과 runtime 오류를 .NET 표준 diagnostics로 제공한다.

- Trace는 `ActivitySource`를 사용한다.
- Metric은 `System.Diagnostics.Metrics.Meter`를 사용하며 meter 이름은 `zlink.framework`다.
- Log는 `Microsoft.Extensions.Logging.ILogger` category를 사용한다.

Application은 [topology와 host monitoring](10-topology-monitoring.ko.md)의
`IZLinkDiagnosticsOptions`로 level, sampling과 message size 기록 여부를 설정한다. Exporter, log provider,
file과 원격 backend는 application이 구성한다.

Public callback 기반 message-flow observer, runtime error sink와 raw socket event DTO는 제공하지 않는다.
Trace·metric·log에 포함되는 안정된 operation 이름과 attribute는 공통
[Message flow](../../../../26-message-flow-tracing.ko.md)와
[Runtime metrics](../../../../25-runtime-metrics.ko.md)가 정의한다.

Timer handler 실패는 해당 Spot ID와 timer 이름을 포함한 structured log와 trace error로 기록한다.
Scheduler delivery index, handler runtime type, exception type과 stack trace를 public DTO로 제공하지 않는다.
