# 08. Observability diagnostics

[Reference index](README.en.md)

The diagnostics option values, host status fields, and the `ZLinkFrameworkErrorKind`
correspondence table are exactly the same as
[Java reference 08. Observability diagnostics](../../java/reference/08-observability-diagnostics.en.md)
(Korean-only) — Kotlin uses Java's `ZLinkFrameworkErrorKind`/`ZLinkFrameworkException` directly
and adds no separate enum or exception hierarchy. The exact signature is owned by the
[Kotlin monitoring exact interface](../../common/spec/server/languages/kotlin/interfaces/monitoring.en.md)
(Korean-only).

---

## `configureDispatch { }` (configuration time, DSL)

Configures the Java `ZLinkDispatchOptions` diagnostics level and sampling in receiver form.

```kotlin
options.configureDispatch {
    messageFlow(ZLinkMessageFlowLogMode.NORMAL) // Records major transitions as structured records.
    traceSampleRate(0.1)
    includeMessageSizes(true)
}
```

**Options.** The levels are the same four Java values: `OFF`, `ERRORS`, `NORMAL`, and `DETAILED`.
`ERRORS` is the default. The same DSL also configures the sampling rate and whether to include
message sizes.

**Completion result.** The Framework writes structured records to the standard logger, trace, and
metric providers configured by the application. A provider failure is isolated as separate
diagnostics and does not change the original message operation's terminal result. Kotlin adds no
message-flow observer, runtime error sink, file path, or raw event DTO.

**When to use.** Use this to group startup diagnostics level and sampling settings in Kotlin DSL
form.

---

## Host status/error kind

Host status (`ZLinkFrameworkRuntimeStatus`, `ZLinkInboundDispatchStatus`) and the
`ZLinkFrameworkErrorKind` correspondence table use the Java types directly — no Kotlin-only data
class or enum is added. See
[the Java reference's document 08](../../java/reference/08-observability-diagnostics.en.md)
(Korean-only) for the field meaning and correspondence table of both entries.

---

See the
[Kotlin monitoring exact interface](../../common/spec/server/languages/kotlin/interfaces/monitoring.en.md)
and
[Java reference 08. Observability diagnostics](../../java/reference/08-observability-diagnostics.en.md)
(Korean-only) for the full rationale.
