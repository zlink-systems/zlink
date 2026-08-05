# 08. Observability diagnostics

[Reference index](README.en.md)

This category covers `IZLinkDiagnosticsOptions`/`IZLinkDiagnosticsRuntime`, which configure
trace·metric·log recording levels, and the `ZLinkFrameworkErrorKind` lookup table every category
uses to judge failures. The exact signatures are owned by the
[Topology monitoring exact interface](../../common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.en.md)
and the
[Framework error exact interface](../../common/spec/server/languages/dotnet/interfaces/10-monitoring-errors.ko.md)
(both Korean-only).

---

## `ConfigureDispatch().Diagnostics` (configuration time)

Sets the trace·metric recording level and sampling.

```csharp
services.AddZLinkFramework(options =>
{
    options.ConfigureDispatch().Diagnostics
        .SetLevel(ZLinkDiagnosticsLevel.Detailed)
        .SetSampleRate(0.1)
        .IncludeMessageSizes(true);
});
```

**Options.** The following modifiers attach to this call.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.SetLevel(ZLinkDiagnosticsLevel)` | no default stated in the exact interface (one of `Off`/`Errors`/`Normal`/`Detailed`) | how detailed the recording is |
| `.SetSampleRate(double)` | no default stated in the exact interface | `0.0`..`1.0`. Out of range throws `ArgumentOutOfRangeException` |
| `.IncludeMessageSizes(bool)` | no default stated in the exact interface | whether payload size distribution is included in telemetry. The payload content itself is never recorded |

Each modifier is a synchronous fluent call that returns `IZLinkDiagnosticsOptions` — not a
registration with no return value.

**Completion.** Trace is exposed through
`ActivitySource`, metrics through a `Meter` named `zlink.framework`, and logs through an
`ILogger` category — the application configures the exporter and the remote backend.

**When to use it.** Use it to set the default recording level at startup. To change only the
level while running, use `IZLinkDiagnosticsRuntime`.

---

## `IZLinkDiagnosticsRuntime.Level` (read·change)

Reads or changes the running process's diagnostics level.

```csharp
ZLinkDiagnosticsLevel current = diagnosticsRuntime.Level;
diagnosticsRuntime.Level = ZLinkDiagnosticsLevel.Detailed; // temporarily raises it during incident diagnosis
```

**Options.** This entry point has a single property.

| Property | Default | Meaning |
| --- | --- | --- |
| `Level` | the value registered through `ConfigureDispatch().Diagnostics.SetLevel(...)` | the level currently in effect |

**Completion.** A synchronous get/set. Changing the value is an atomic state change that
applies the new level starting with message processing that begins afterward, and it does not
affect records already in the telemetry queue.

**When to use it.** Use it to raise or lower recording detail at a specific moment without
redeploying.

---

## `ZLinkFrameworkErrorKind` lookup table

When a Framework operation fails, `ZLinkFrameworkException.Kind` tells the cause family. This
table is the shared basis every category's completion-kind description relies on.

| Kind | What to check in the application |
| --- | --- |
| `NotFound` | check whether the requested Actor, Spot, handler, route, or target exists |
| `AlreadyExists` | check whether create and registration need to be handled idempotently |
| `TypeMismatch` | check whether the stable type matches the requested application type |
| `NotConfigured` | check whether the needed role, handler, Store, or object client was registered at startup |
| `Rejected` | a Framework admission, filter, or runtime policy with no typed result rejected the operation |
| `Unavailable` | the target, route, Store, or worker cannot handle the operation right now |
| `CapacityExceeded` | placement, a queue, or a bounded resource has no room left |
| `DeadlineExceeded` | the operation did not complete within its deadline. Whether a side effect occurred follows that operation's own contract |
| `ShuttingDown` | the runtime is not accepting new admissions. Use a different serving instance |
| `ProtocolError` | check whether the protocol or reply contract matches the peer's |
| `InvalidOperation` | the requested operation is not allowed in the current object·session·runtime state |
| `DataLost` | a published relocation payload could not be found or failed validation. It does not arbitrarily roll back to the previous owner |
| `InternalFailure` | a Framework failure that does not fit the above classification. Check the cause using log·trace correlation information |

**Completion.** Only the Framework creates `ZLinkFrameworkException`, and `Message` is a
human-diagnosis description, not a target for programmatic branching.
`ZLinkConfigurationException` (startup validation failure) and the `ArgumentException` family
(invalid arguments) are a different layer from this kind classification. This kind does not tell
whether to retry — the application judges that directly by checking the operation's completion
condition, idempotency, and business state.

**When to use it.** Use it to look back at the kind named in each category entry's "Completion"
section and decide how to respond.

---

The full basis is the
[Topology monitoring exact interface](../../common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.en.md) and the
[Framework error exact interface](../../common/spec/server/languages/dotnet/interfaces/10-monitoring-errors.ko.md)
(both Korean-only).
