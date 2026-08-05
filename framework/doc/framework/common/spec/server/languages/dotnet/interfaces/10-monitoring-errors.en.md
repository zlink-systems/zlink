# .NET Framework Error Public Interface

[.NET exact interface table of contents](README.en.md) ·
[Common Framework API](../../../../06-framework-api.en.md) ·
[Message Flow](../../../../26-message-flow-tracing.en.md)

## 1. Scope

This document fixes the public exception and stable error classification
a .NET application uses to handle a Framework operation failure. Error
kinds are distinguished based on the response action available to the
application, not the internal function or state-machine step where the
failure occurred.

The internal state of authority owner, fence, generation, descriptor
revision, relocation staging, worker queue, and timer scheduler isn't
included in the public error contract. Detailed causes and stack traces
are recorded in .NET logging and tracing.

## 2. Framework Errors

An invalid public argument is rejected with the .NET standard
`ArgumentException` family. If startup configuration is invalid,
`ZLinkConfigurationException` is thrown. A Framework failure occurring
during a running request, lifecycle, or one-way operation is delivered as
`ZLinkFrameworkException`.

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

Only the framework creates `ZLinkFrameworkException`. The application
doesn't subclass the exception or change the error classification.
`ZLinkConfigurationException` is also created by the framework as a
result of startup validation. `Message` is a description for human
diagnosis and isn't used for programmatic branching.

The public exception doesn't provide whether it's retryable. The
application decides whether to start a new operation by checking the
operation's completion condition, idempotency, and business state.

## 3. Error Kind Meaning

| Kind | What the application should check |
|---|---|
| `NotFound` | Confirm whether the requested Actor, Spot, handler, route, or target exists. |
| `AlreadyExists` | Confirm whether create and registration must be handled idempotently. |
| `TypeMismatch` | Confirm whether stable type matches the requested application type. |
| `NotConfigured` | Confirm whether the needed role, handler, Store, or object client was registered at startup. |
| `Rejected` | Framework admission, filter, or runtime policy with no typed result rejected the operation. |
| `Unavailable` | The target, route, Store, or worker currently can't process the operation. |
| `CapacityExceeded` | Placement, queue, or a bounded resource has no room left. |
| `DeadlineExceeded` | The operation didn't complete within its set deadline. Whether the result has a side effect follows that operation's contract. |
| `ShuttingDown` | The runtime isn't accepting new admission. A different serving instance must be used. |
| `ProtocolError` | Confirm whether the protocol or reply contract matches the peer. |
| `InvalidOperation` | The requested operation isn't allowed in the current object/session/runtime state. |
| `DataLost` | The published relocation payload can't be found or failed verification. There's no arbitrary rollback to a previous owner. |
| `InternalFailure` | A Framework failure that can't be expressed with the above classification. Confirm the cause using the correlation information in logs and traces. |

Generation staleness, object moving, worker queue state, and the
relocation processing stage are causes the framework uses to judge
internal handling. If the application can't choose a different response,
they aren't distinguished as a separate public kind.

## 4. Diagnostics Boundary

The framework provides application message flow and runtime errors
through .NET standard diagnostics.

- Trace uses `ActivitySource`.
- Metric uses `System.Diagnostics.Metrics.Meter`, with meter name
  `zlink.framework`.
- Log uses the `Microsoft.Extensions.Logging.ILogger` category.

The application configures level, sampling, and whether to record
message size with
[Topology And Host Monitoring](10-topology-monitoring.en.md)'s
`IZLinkDiagnosticsOptions`. Exporter, log provider, file, and remote
backend are configured by the application.

A public callback-based message-flow observer, runtime error sink, and
raw socket event DTO aren't provided. The stable operation names and
attributes included in trace/metric/log are defined by the common
[Message Flow](../../../../26-message-flow-tracing.en.md) and
[Runtime Metrics](../../../../25-runtime-metrics.en.md).

A timer handler failure is recorded as a structured log and trace error
that includes that Spot ID and timer name. Scheduler delivery index,
handler runtime type, exception type, and stack trace aren't provided as
a public DTO.
