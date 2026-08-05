# 08. Observability diagnostics

[Reference index](README.en.md)

This category covers `dispatch_options_t`/`dispatch_diagnostics_options_t`, which configure the
trace/metric/log recording level; `framework_runtime_t`, which reads host/topology status;
`logging_builder_t`, which configures structured logging; and the `framework_error_kind_t`
correspondence table used to judge failures across every category. The exact signatures are owned
by the
[Monitoring exact interface](../../common/spec/server/languages/cpp/interfaces/08-monitoring.en.md)
and the
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.en.md)
(Korean-only).

---

## `configure_dispatch().diagnostics` (configuration time)

Sets the trace/metric recording level and sampling.

```cpp
options.configure_dispatch()
  .message_flow(zlink::framework::message_flow_log_mode_t::key_transitions)
  .trace_sample_rate(0.1)
  .include_message_sizes(true);
```

**Options.** This call carries the following modifiers.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.message_flow(message_flow_log_mode_t)` | `off` | The detail level to record: one of `off`/`errors_only`/`key_transitions`/`verbose`/`diagnostic` |
| `.trace_sample_rate(double)` | Implementation default | `0.0`..`1.0`. NaN or out of range is a configuration error |
| `.include_message_sizes(bool)` | `false` | Whether to include the payload size distribution in telemetry. The payload content itself is never recorded |
| `.trace_log_file(path)` | None | The file path to leave diagnostics records in |
| `.trace_label(id)` | None | The label to attach to diagnostics records |
| `.set_message_flow_observer(shared_ptr<message_flow_observer_t>)` / `.set_message_flow_observer(function<void(const message_flow_event_t&)>)` | None | Registers an observer that receives `message_flow_event_t` (§2) |
| `.message_flow_live(shared_ptr<atomic<message_flow_log_mode_t>>)` | None | Links a shared atomic for changing the mode while running. `app_t::set_message_flow_mode(...)` updates this value |

Each modifier is a synchronous fluent call returning `dispatch_options_t` — not a registration
with no return value.

**Completion result.** The application configures the trace/metric/log recording destination
(exporter, remote backend) separately. `send` and `publish` have no reply path, so `reply_error`
cannot be used in the unhandled policy.

**When to use.** Use this to set the default recording level at startup. To change only the level
while running, use `app_t::set_message_flow_mode` in the host-lifecycle category.

---

## `framework_runtime_t::status` / `observe` (read/observe)

Queries or observes the host-wide status (lifecycle state, relocation/termination results,
inbound dispatch backpressure).

```cpp
zlink::framework::framework_runtime_status_t status = framework_runtime.status();
bool can_accept_new_operations = status.is_ready && status.accepting_work;

auto observation = framework_runtime.observe(
  /*capacity=*/64,
  [](const auto &observed) {
      // check observed.status.inbound_dispatch, observed.status.state
  });
```

**Options.** This entry point has no modifiers.

**Completion result.** `status()` is a synchronous call that returns a value immediately.
`observe(...)` delivers `observed_status_t<framework_runtime_status_t>` to the callback, and the
`loss` field tells you whether observations were lost.
`framework_runtime_status_t::inbound_dispatch` (`inbound_dispatch_status_t`) shows the
application HWM usage and backpressure status.

**When to use.** Use this to diagnose the host's overall lifecycle state or inbound backpressure.
Use the status-query entry in the topology-discovery category for a specific
MeshName/ChannelName's availability.

---

## Logging configuration (`app_t::logging()`, configuration time)

Configures the structured logging provider (console, file, callback sink).

```cpp
app.logging()
  .use_console()
  .use_rotating_file("logs/app.log")
  .set_min_level(zlink::framework::log_level_t::info)
  .use_async();
```

**Options.** Commonly used modifiers are as follows.

| Modifier | Default | Meaning |
| --- | --- | --- |
| `.use_console()` | Disabled | Writes logs to standard output |
| `.use_file(path)` / `.use_rotating_file(path, options)` | None | Writes logs to a file. Rotating options are `max_file_size`/`max_files` |
| `.use_callback_sink(sink)` / `.use_provider(name, sink)` | None | Delivers logs to an application-provided sink |
| `.use_async(options)` | Synchronous | Writes logs through an async queue (capacity/overflow policy) |
| `.set_min_level(log_level_t)` | Implementation default | Does not record anything below this level |

**Completion result.** Registers synchronously with no return value. A handler declares
`logger_t<THandler>` as a dependency to receive a category logger via DI — no separate service
registration is needed.

**When to use.** Use this to configure the standard logging provider and health surface. If a
handler needs a custom category, take `logger_factory_t` as a dependency and create one.

---

## `framework_error_kind_t` correspondence table

When a Framework operation fails, `framework_exception_t::kind()` tells you the cause family.
This table is the shared basis for the completion-kind descriptions in every category.

| Kind | What the application should check |
| --- | --- |
| `not_found` | Whether the requested Actor, Spot, handler, route, or target exists |
| `already_exists` | Whether create and registration must be handled idempotently |
| `type_mismatch` | Whether the stable type matches the requested application type |
| `not_configured` | Whether the required role, handler, Store, or object client was registered at startup |
| `rejected` | Framework admission, a filter, or a runtime policy without a typed result rejected the operation |
| `unavailable` | Whether the target, route, Store, or worker can currently handle the operation |
| `capacity_exceeded` | Whether placement, queue, or a bounded resource has no room left |
| `deadline_exceeded` | The operation did not complete within the deadline. Whether the result had side effects follows that operation's contract |
| `shutting_down` | The runtime is not accepting new admission. Use a different serving instance |
| `protocol_error` | Whether the protocol or reply contract matches the peer |
| `invalid_operation` | The requested operation is not allowed in the current object/session/runtime state |
| `data_lost` | A published relocation payload could not be found, or failed validation. There is no arbitrary rollback to the previous owner |
| `internal_failure` | A Framework failure not expressible by the categories above. Check log and trace correlation information for the cause |

**Completion result.** Only the Framework creates a `framework_exception_t`, and `what()` is a
description for human diagnosis, not a programmatic branching target. `code()` adds diagnostic
information when there is a platform cause such as a timeout or transport, but it does not
substitute for the common failure classification. Configuration validation failures (such as a
`std::invalid_argument` before startup) and argument errors are a different layer from this kind
classification. This kind does not tell you whether to retry — the application decides that
directly, checking the operation's completion condition, idempotency, and business state.

**When to use.** Use this table to look back at the kind given in each category entry's
"Completion result" and decide how to respond.

---

See the
[Monitoring exact interface](../../common/spec/server/languages/cpp/interfaces/08-monitoring.en.md)
and the
[Channel messaging exact interface](../../common/spec/server/languages/cpp/interfaces/03-channel-messaging.en.md)
(Korean-only) for the full rationale.
