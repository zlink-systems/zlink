---
title: "Rust Bindings Implementation Blueprint"
---

<!-- bindings-nav:start -->
[Spec index](../README.en.md) | [Previous: Go](../go/README.en.md)
<!-- bindings-nav:end -->

# Rust Bindings Implementation Blueprint

> **What this chapter defines** — the Rust crate's public shape and the
> `contracts`/`runtime` source placement rules.

This document defines the Rust crate shape. It is not an
exhaustive list of every public item. The concrete public contract source
is `bindings/rust/src/contracts/`. `bindings/rust/src/lib.rs` is the crate
projection that re-exports the intended public API.

The `contracts` source tree, private runtime tree, public export projection, rustdoc, tests, samples,
perf runner, and runtime project `core/include/zlink.h` capabilities into an idiomatic Rust API. This
document defines where each public contract and runtime helper is owned.

 This binding follows
This binding follows the common bindings architecture map using Rust
naming conventions. The `contracts` and private `runtime` modules organize
source ownership, and `lib.rs` decides which module paths become the
public crate API.

| Section | Covers |
|---|---|
| [Public contract source](#public-contract-source) | The contracts/runtime/native source locations and documentation's role |
| [Repository layout](#repository-layout) | The file-granularity policy and the aligned directory tree |
| [API change principles](#api-change-principles) | Public mapping and contract/runtime boundary requirements |
| [Library shape](#library-shape) | RAII ownership, `Result`, Builders, the `unsafe` boundary |
| [Contract / runtime placement rules](#contract--runtime-placement-rules) | The boundary between public declarations and private helpers |
| [Contract file layout](#contract-file-layout) | Files per category under `contracts/` |
| [Runtime file layout](#runtime-file-layout) | Files per category under `runtime/` |
| [Creation entry points](#creation-entry-points) | The list of crate root constructors and contract methods |
| [Contract category map](#contract-category-map) | Category → module mapping |
| [Standard interface rules](#standard-interface-rules) | recv signatures, builder start methods, naming constraints |
| [Crate layout](#crate-layout) | The classification of public modules |
| [Byte HWM and Auto-HWM](#byte-hwm-and-auto-hwm) | Mapping between Rust `u64` and Core byte HWM |
| [Receive flow state](#receive-flow-state) | The receive-flow state type, setter, and monitor surface |
| [Required capability coverage](#required-capability-coverage) | The user-facing capabilities that must be guaranteed once aligned |
| [Spot Get-Or-Create](#spot-get-or-create) | The `get_or_create_spot` contract |
| [Receive and Subscribe shape](#receive-and-subscribe-shape) | Storage reuse, distinguishing no-data |
| [Error and validation policy](#error-and-validation-policy) | FFI-boundary validation and type-based errors |
| [Performance policy](#performance-policy) | Hot-path constraints |
| [Implementation checklist](#implementation-checklist) | What to confirm before declaring alignment, and required verification commands |
| [Actor and Spot Route results](#actor-and-spot-route-results) | The route-result value types and Actor-directed send/request |

## Public contract source

- Public contract source: `bindings/rust/src/contracts/`.
- Crate projection: `lib.rs`'s public re-exports and the rustdoc for public modules.
- Runtime implementation: private modules under `bindings/rust/src/runtime/`.
- Native bridge: private modules under `bindings/rust/src/runtime/native/` — raw handles, callback trampolines, request-progress helpers, part-loop helpers.
- Concrete crate-private resource storage lives in `bindings/rust/src/internal.rs`. Contract files may reference this storage's types but never import runtime resource types directly. FFI declarations and native calls stay under `runtime/`.
- Documentation's role: this README defines the shape and semantic coverage. The public crate export owns the exact member list. Each public item must still map to one of the common contract categories.

Applications, perf, and samples do not depend on private modules or raw
FFI bindings.

## Repository layout

Use these paths consistently when changing the Rust binding.

- Public contract: `bindings/rust/src/contracts/`.
- Crate projection: `bindings/rust/src/lib.rs`.
- Runtime implementation: private modules under `bindings/rust/src/runtime/`.
- crate-private concrete storage: `bindings/rust/src/internal.rs`.
- Native bridge/artifacts: private modules under `bindings/rust/src/runtime/native/`, `bindings/rust/native/`, `bindings/rust/include/`.
- Codec crate: none provided. The Rust binding keeps only the raw `Message` and byte-payload API.
- Tests: `bindings/rust/tests/`.
- Samples: `bindings/rust/samples/`.
- Perf: `bindings/rust/perf/`.

`lib.rs`'s public re-exports must be intentional. A Rust module path
becomes part of the public API the moment it is exported. The `contracts`
and `runtime` source trees are just an implementation organization unless
`lib.rs` explicitly exposes a module. `zlink::runtime` and a raw native
bridge module are never exposed.

The tree below is the aligned implementation structure. Public structs,
enums, traits, errors, free functions, and builder contracts belong to
`contracts/` and are intentionally re-exported from `lib.rs`. FFI bindings,
native struct mirrors, callback trampolines, request-progress helpers,
marshalling, and the unsafe part loop stay private under `runtime/`. The
crate-private storage module holds only the concrete state a public
wrapper must own, and neither declares nor calls the FFI surface.

File granularity follows the common policy in `../README.md`. Keep one
file per independent public concept, or per tightly coupled
operation/model group. Merge a very small marker trait, callback alias,
enum-only module, or pass-through helper module into the nearest contract
file when doing so makes the public shape easier to read.

```text
bindings/rust/
+-- src/
|   +-- lib.rs
|   +-- internal.rs
|   +-- contracts/
|   |   +-- core/
|   |   |   +-- context.rs
|   |   |   +-- routing_id.rs
|   |   +-- messaging/
|   |   |   +-- message.rs
|   |   |   +-- received.rs
|   |   |   +-- topic_message.rs
|   |   |   +-- subscription_event.rs
|   |   +-- sockets/
|   |   |   +-- socket.rs
|   |   |   +-- pair_socket.rs
|   |   |   +-- dealer_socket.rs
|   |   |   +-- router_socket.rs
|   |   |   +-- pubsub_sockets.rs
|   |   |   +-- stream_socket.rs
|   |   |   +-- socket_options.rs
|   |   |   +-- socket_operations.rs
|   |   +-- eventing/
|   |   |   +-- monitor.rs
|   |   |   +-- poller.rs
|   |   +-- service/
|   |   |   +-- spot/
|   |   |   |   +-- spot_node.rs
|   |   |   |   +-- spot.rs
|   |   |   |   +-- actor.rs
|   |   |   |   +-- spot_operations.rs
|   |   |   |   +-- spot_models.rs
|   |   +-- errors/
|   |   |   +-- errors.rs
|   |   |   +-- results.rs
|   +-- runtime/
|   |   +-- messaging/
|   |   |   +-- message.rs
|   |   |   +-- domain.rs
|   |   |   +-- request_progress.rs
|   |   +-- sockets/
|   |   |   +-- socket_base.rs
|   |   |   +-- pair_socket.rs
|   |   |   +-- dealer_socket.rs
|   |   |   +-- router_socket.rs
|   |   |   +-- pub_socket.rs
|   |   |   +-- sub_socket.rs
|   |   |   +-- xpub_socket.rs
|   |   |   +-- xsub_socket.rs
|   |   |   +-- stream_socket.rs
|   |   +-- eventing/
|   |   |   +-- poller.rs
|   |   |   +-- timer.rs
|   |   +-- service/
|   |   |   +-- spot/
|   |   |   |   +-- spot_node.rs
|   |   |   |   +-- spot.rs
|   |   |   |   +-- actor.rs
|   |   +-- errors/
|   |   |   +-- native_errors.rs
|   |   +-- native/
|   |   |   +-- native.rs
+-- tests/
+-- samples/
+-- perf/
+-- native/
+-- include/
```

The public consumer projection is `lib.rs`. Tests, samples, and perf use
the public crate projection. If an item is publicly re-exported, a
reviewer must be able to point to the common contract category it belongs
to. If a module exists only for native calls or to maintain an unsafe
invariant, it stays private under `runtime/`.

## API change principles

The public projection of a Core capability follows these principles.

1. Choose the common contract category that will own the public behavior.
2. Add the public type, method, or function to the module that safely owns it, and update the `lib.rs` re-export projection if it must be visible at the crate root.
3. Add a concrete public type or method first, and add a trait only if a genuinely substitutable behavior is needed.
4. Keep `unsafe`, raw handles, callback userdata, and the part loop inside private modules.
5. A fallible operation returns a `Result` carrying typed error information.
6. Add a test that uses the public crate projection.
7. Update samples and perf only through the public API.
8. Run formatting and clippy-style checks where available.

The contract/runtime boundary meets these requirements.

1. Move public behavior declarations to `src/contracts/<category>/`.
2. Move native-backed implementations to `src/runtime/<category>/`.
3. Keep unsafe FFI, native loading, raw handles, and callback trampolines under `src/runtime/native/`.
4. Public code constructs resources through a crate-root constructor or contract method.
5. Crate exports do not expose a runtime module as public API.
6. Deprecated wrappers, duplicate operation-start names, and compatibility-only naming aliases are not
   part of the public surface.
7. Tests, samples, and perf use only the public crate export.
8. Rustdoc does not expose private implementation modules as public API.

The following Rust-specific shortcuts are not allowed.

- The `contracts` modules do not re-export `runtime` or `runtime::native`.
- Contract files never import a runtime resource type to describe a public service model.
- A public re-export barrel is never the source of a resource's behavior — declarations are split between a named contract module and a runtime implementation module.
- `lib.rs` exports contract names and constructors, and never exports a runtime module.
- Public rustdoc never exposes a runtime implementation module path as a public type.

## Library shape

This binding should feel like a safe Rust crate on top of the native
runtime.

- A public resource type owns its native lifetime and releases the resource via `Drop`.
- A fallible operation returns `Result<T, ZlinkError>`, or a more specific typed result when that improves clarity.
- Concrete values — message, routing id, received metadata, topic message, snapshot, option, enum, error — stay concrete types.
- A trait is used only when the caller needs substitutable behavior or a generic bound. Not every concrete handle gets a trait by default.
- A Builder is required for multipart send, publish, request, reply, SPOT, and actor operations, and it hides native state.
- `unsafe` and raw FFI are confined to private modules.

### Safe FFI RAII wrapper placement

A native-backed Rust resource uses Rust's common safe FFI RAII wrapper
pattern. A public `struct` handle owns the native lifetime, public
inherent `impl` methods expose safe Rust operations, and `Drop` releases
the native resource.

The public inherent `impl` surface lives in the matching `contracts/`-owned
file. Even when a method body immediately delegates to a runtime helper,
the public method list must be readable from the contract file. A runtime
module hides C API calls, `unsafe` blocks, raw handles, downcasts, errno
mapping, and native struct conversion behind `pub(crate)` helper
functions. A public resource's public methods are never left discoverable
only in the runtime module.

A trait is used only when the caller needs substitutable behavior or a
generic bound. A trait is never created just to make a native-backed
handle look like an interface. For a C-handle wrapper with a single
implementation, prefer the combination of a `pub struct` in `contracts/`
with public inherent methods, plus private helpers in `runtime/`.

## Contract / runtime placement rules

- Public structs, enums, traits, errors, and builder contracts belong to their matching `contracts/` category, and are re-exported from `lib.rs` when public.
- A public free function, associated helper function, convenience method, or builder helper method belongs to a public module when the caller can use it directly.
- The public inherent `impl` block of a native-backed public resource lives in the contract-owned file; its body may thinly delegate to a `pub(crate)` runtime helper.
- Runtime handle owners, the request pump, callback adapters, and part-loop helpers stay private or `pub(crate)`.
- FFI bindings, raw pointers, native struct mirrors, marshalling helpers, and platform loading code stay inside a private FFI/runtime owner.
- `lib.rs` and public rustdoc modules project the contract categories and never expose a runtime module.
- A runtime concrete type is a construction target behind a crate root constructor or contract method. `lib.rs` may import a runtime module only to wire up such a constructor, and public signatures use contract names.

## Contract file layout

The contract source uses the same classification as the [.NET bindings blueprint](../dotnet/README.en.md), with Rust naming conventions. Keeping the same conceptual file grouping lets a developer who knows another binding find the same public concept quickly in Rust too.

- `core/`: `context.rs`, `routing_id.rs`, and core option/value files.
- `messaging/`: `message.rs`, `received.rs`, `topic_message.rs`, `subscription_event.rs`, common operation payload types.
- `sockets/`: socket types/traits, socket option types, send/request/reply builder contracts, stream packet values, socket flags.
- `eventing/`: monitor, monitor event/status, poller, poll event, and timer contracts.
- `service/`: SPOT node, Spot, Actor, topology model, service operation builders, under a `spot/` submodule.
- `errors/`: public error types, the result domain, error-code mapping.

Avoid a single consolidated `models.rs` or a runtime export barrel for
public resource behavior. Small DTO-style structs and enums may be grouped
if they can sit with the contract that gives them meaning, but a
native-backed resource and an operation builder each need a named contract
file.

## Runtime file layout

The runtime source mirrors the same conceptual categories but contains
only implementation.

- `core/`: the context implementation and context option helpers.
- `messaging/`: message materialization, request progress, request execution, native buffer conversion helpers.
- `sockets/`: the socket base type, the socket kernel, the socket implementation for every socket family, callback adapters, operation implementation types.
- `eventing/`: the poller/timer/monitor implementations and event materialization helpers.
- `service/`: SPOT node, Spot, Actor, topology, and service operation implementations.
- `errors/`: native error conversion and validation helpers.
- `native/`: FFI bindings, native loading, raw handles, unsafe boundary code.

A runtime module may import contract types, but a contract module never
imports a runtime module. The crate root may instantiate a runtime
implementation inside a constructor, but exports the contract name, not
the runtime implementation module.

## Creation entry points

Public creation is provided only through crate root constructors and
public contract methods.

- `Context::new(...)` creates the native-backed context implementation.
- `Context::create_pair_socket()`, `create_dealer_socket()`, `create_router_socket()`, `create_pub_socket()`, `create_sub_socket()`, `create_xpub_socket()`, `create_xsub_socket()`, `create_stream_socket()` create the native-backed socket implementations.
- `Context::create_spot_node(...)` creates the service-layer implementation.
- A `Spot` handle is obtained via `SpotNode::create_spot(...)`, `entry_spot()`, `get_or_create_spot(...)`, or `spot_lookup(...)`. Direct `Spot` construction is not public.
- An Actor handle is created via `SpotNode::create_actor(...)`. Direct Actor construction is not public.
- `Poller::new(...)`, `Timer::new(...)`, and the timer-on-SPOT helpers create eventing resources.
- `Poller` accepts a socket monitor as a source through
  `add_monitor(&self, monitor: &SocketMonitor, events: i16, slot: usize) -> Result<(), ConfigError>`,
  `modify_monitor(&self, monitor: &SocketMonitor, events: i16) -> Result<(), ConfigError>` and
  `remove_monitor(&self, monitor: &SocketMonitor) -> Result<bool, ConfigError>` (common spec "Monitor sources in
  `Poller`"). `SocketMonitor` does not implement the socket-only sealed `Pollable` (this protects the `proxy`
  argument contract). Only `POLLIN` is valid for a monitor; any other mask is rejected with
  `ConfigError(ConfigResult::InvalidArgument)`. Drain with `recv_with_flags(RecvFlags::DONT_WAIT)` after readiness;
  `PollEvent` reports it as `PollSourceKind::Socket`.
- `AtomicCounter::new()`, `Stopwatch::start()`, `Thread::start(...)` create caller-owned utility resources.
- Version and capability lookup, strerror, proxy, sleep, and the multipart cleanup helper are public crate functions. The FFI calls behind these functions stay private.

## Contract category map

These categories map to lowercase modules under
`bindings/rust/src/contracts/`, and are the review-ownership map for
public crate items and re-exports.

- `core/`: context, context option, routing id, version/capability lookup helpers, utility contracts.
- `messaging/`: message, received metadata, topic message, subscription event, stream packet data, builder payload helpers.
- `sockets/`: socket operations, socket family, typed options, request/reply, publish/subscribe surfaces.
- `eventing/`: monitor, monitor snapshot/event, poller, poll event, timer, public poll helpers.
- `service/`: SPOT node, SPOT handle, topology model, actor ref, actor lifecycle, operation builders.
- `errors/`: the typed error/result domain.
- Enum, flag, and result types live in the category that defines their meaning. No `enums` module is created just to group declarations syntactically.

## Standard interface rules

- Data-plane `recv`, routed recv, `subscribe`, and subscription-event receive fill a caller-provided `&mut Received`, `&mut TopicMessage`, or `&mut SubscriptionEvent` value and return `Result<bool, RecvError>`.
- Send, routed send, publish, request, reply, SPOT operations, and Actor location/session operations return a typestate builder.
- A builder's start method takes only a target identity, topic, channel, routing ID, or `ReplyToken`.
  Payload, request timeout, and terminal choice are the builder's state or stages.
- SPOT's channel-targeted operations use `send_to_channel(...)` and `request_to_channel(...)`. SPOT's topic publish keeps `publish(topic)` as-is.
- No single-payload shortcut method is added under the same name as an operation's start method. `send(message)`, `send(routing_id, message)`, `publish(topic, message)`, `send_to_channel(channel, message)`, `send_to_spot(..., message)` are not public contract members. A caller uses `send(...).message(message).submit()`.
- A multipart payload accumulates via repeated `message(...)` calls. A `messages(...)` convenience method is allowed only when it delegates to the same builder contract and is declared on the public crate surface.
- A Dealer socket does not expose protocol envelope helpers such as `request_frame(...)` or `reply(request_token, parts)`. Dealer can start a request with `request()`, but has no API-level peer routing id, so it cannot reply to an arbitrary token.
- A message payload factory uses a fallible from-source contract: `Message::try_from(...)` and a `TryFrom` implementation. A copy-only name such as `copy_from` is not part of the public contract.
- Routing id construction uses the standard `From` implementations. Public helpers such as `from_bytes`, `from_string`, `from_u32`, `from_uuid_bytes` are not part of the public contract. Hex decoding may keep `from_hex` / `try_from_hex`.
- No operation-start method family such as `send_no_wait`, `publish_with_flags`, `request_async` is added. Keep one operation name, and let the builder absorb variants. The async surface also does not grow the operation start-point name — it is expressed as a builder terminator or a `Future`-returning surface.
- The async builder terminator is `submit()` because `async` is a Rust keyword.

  ```rust
  dealer.send().message(message).submit().await?;
  let reply = dealer.request().message(request).submit().await?;
  ```

  HWM-managed **send** (PAIR `send()`, STREAM `send(target)`, `Received::send()`, and
  DEALER/ROUTER send) provides asynchronous `submit()` and synchronous `submit_sync()`. Request also
  provides `submit()` and `submit_sync()` and keeps reply timeout on the builder. **publish** is not
  in this classification: its terminal is synchronous
  `submit() -> Result<(), SubmitError>` (lossy PUB semantics; under `ZLINK_PUB_OPT_NODROP`, a full
  subscriber immediately returns `Backpressured`).
- A send Future progresses when the socket-local owner drains a Core `DONTWAIT` completion.
  `submit_sync()` uses Core `NONE` admission. Future drop detaches only the waiter; a late completion
  releases provisional-registry state and the native payload.
- PUB/XPUB `publish` and ROUTER reply retain separate synchronous operation contracts. The raw
  ROUTER/`Received` reply terminal is the one-shot
  `ReplyOp<Ready>::submit() -> Result<(), SubmitError>`. It submits a terminal reply or error reply
  with one native call. A DEALER peer is subject to Application HWM, `PAUSED`, and `SNDTIMEO`, so the
  result can be `BACKPRESSURED`; a ROUTER peer uses the HWM-free Completion connection.
  `NOT_CONNECTED`, `TERMINATED`, `INVALID_ARGUMENT`, and other submit failures return immediately as
  `Err(SubmitError)`.

## Crate layout

The crate must expose clear public modules or re-exports.

- Core: context, options, version/capability lookup helpers, utility.
- Messaging: message, routing id, received metadata, topic message, subscription event, stream packet data.
- Sockets: pair, dealer, router, pub, sub, xpub, xsub, stream, typed options, request/reply, publish/subscribe, stream packet API.
- Eventing: monitor, monitor snapshot/event, poller, poll event, timer.
- Service: SPOT node, SPOT handle, topology snapshot, actor ref, actor lifecycle, operation builders.
- Error: the typed error/result domain that preserves core semantics.

The public crate may re-export frequently used types at the crate root,
but keeps private FFI modules private.

## Byte HWM and Auto-HWM

Core owns HWM calculation and queue admission.
`CommonSocketOptions::set_send_high_water_mark(u64)` and
`set_receive_high_water_mark(u64)` pass byte values losslessly to Core's exact
8-byte options. Their getters return Core's full `uint64_t` range as `u64`.
`0` means unlimited.

Context options pass the byte-valued Core memory limit and budget, plus the
profile, unchanged to Core. Core applies the profile ratio and distributes the
result across physical directional queues exactly once. Setting a directional
HWM makes that direction a manual override and excludes it from automatic HWM
recalculation.
Context provides `core_hwm_budget_snapshot()` and
`reset_core_hwm_budget_metrics()`. The Rust binding supplies no runtime memory
hint. Input precedence is manual Core budget, explicit memory limit, then Core
fallback. If an explicit input exceeds a finite hard limit Core detected, the
binding preserves the existing configuration error corresponding to `EINVAL`
and does not clamp the value.

Core decides backpressure when the accounted bytes retained by a pipe reach
the applied HWM. The Rust binding does not recount messages and maps Core's
result through its `Result`-based operation contract.
`SocketMonitorOpenOptions::monitor_hwm_bytes`, passed through
`SocketMonitor::open_with_options`, is an exact `u64` byte value. Zero selects
the Core monitor default; a positive value is forwarded unchanged, with no
message-count alias or conversion. Planned, applied, and
deferred HWM and in-flight usage in `MonitorStatus` are `u64` bytes.
Pending-message counts remain display diagnostics; no slot, message-unit,
size-cap, or connection-bucket property is exposed. `snd_pending_bytes` and
`rcv_pending_bytes` are separate `u64` byte values.

The Core budget snapshot projects ABI version/size, configured/runtime/resolved
memory limits, configured/effective budgets, planned/applied/manual-reserved
HWM, Core-queue/application/current/peak/provisional accounted bytes,
completion current/peak/pending and total-messaging values, monitor/instance
aggregates, application/completion queue counts,
`outstanding_application_lease_count`, `retired_queue_count`,
`deferred_origin_credit_bytes`, oversize/blocked/aggregate flags,
`budget_generation`, and `measurement_epoch` as `u64`/boolean values. Reset
preserves current, pending, and queue-count gauges, rebases both peaks to
current, clears epoch counters, and increments `measurement_epoch`.
`application_accounted_bytes` and the three owner-lifecycle fields are
ABI-reserved and always zero. An ABI version/size mismatch is an unsupported
error.

## Receive flow state

The binding exposes the Core receive-flow state as the `ReceiveFlowState`
enum with `Running = 0` and `Paused = 1`. The setter is
`CommonSocketOptions::set_receive_flow_state(&self, value: ReceiveFlowState)
-> Result<(), ConfigError>`, reached through the socket's common-options
facade. It follows the Rust error policy: success is `Ok(())`, and a failure is
`Err(ConfigError)` whose `ConfigResult` this binding derives from the native
errno rather than from the returned result code. `ENOTSUP` therefore becomes
`ConfigResult::NotSupported` for a socket that doesn't support receive flow, and
`EINVAL` becomes `ConfigResult::InvalidArgument`. Setting the state the socket
already holds returns `Ok(())`.

The observation surface follows the C contract, so the constant and metric
names are fixed by the C layer: the monitor events `SEND_FLOW_PAUSED`,
`SEND_FLOW_RESUMED`, and `FLOW_STATE_STALE` (`1 << 16`, `1 << 17`, `1 << 18`,
with the full mask `0x7FFFF`), the event flags `SEND_FLOW_WRITABLE` (`1 << 1`),
and `FLOW_STATE_STALE_EPOCH` (`1 << 3`), the status detail bit `FLOW_STATE`
(`1 << 5`), and the five status
fields `flow_paused_connections`, `flow_pause_applied_total`,
`flow_resume_applied_total`, `flow_state_stale_total`, and
`flow_pause_duration_ms`, projected with this language's naming convention.

Flow-state frames stay inside Core. The binding calls the setter, reads the
monitor events and the snapshot fields, and never encodes, decodes, sends, or
receives a flow-state frame itself.

## Required capability coverage

Once the binding is aligned with the common .NET-baseline policy, the
public crate must cover the following stable, user-facing capabilities.

- Context lifecycle, options, shutdown, auto-HWM recalculation, version, capability lookup, strerror.
- Message ownership, multipart payload, routing id, received metadata, topic message, subscription event, and stream packet values.
- Every socket family and its typed options. `SubSocket::subscription_at(index)` and `XSubSocket::subscription_at(index)` return that index's subscription filter and whether it is a pattern. If that index doesn't exist, they return `None`.
- Monitor, poller, timer, readiness semantics.
- SPOT node, SPOT handle, topology snapshot, actor, stream actor binding.

Rust names and the ownership model may differ from C, but behavior must
match the meaning of the core capabilities.

## Spot Get-Or-Create

Rust exposes `SpotNode::get_or_create_spot(&RoutingId) -> Result<(Spot,
bool), ConfigError>`. This maps directly to
`zlink_spot_node_spot_get_or_new(...)`, and is not implemented by combining
`spot_lookup` and `create_spot`.

The returned `Spot` is owned by the caller and follows ordinary `Drop`
lifetime rules. The boolean is `true` only for the call that created the
logical spot.

## Receive and Subscribe shape

- Data-plane receive and subscribe APIs use a reusable, caller-owned result storage.
- Core byte-HWM charge ends when ordinary `recv` or `subscribe` dequeues the
  payload. `Received` and `TopicMessage` own only the Rust lifetime of parts
  and metadata; reuse, consuming accessors, and `Drop` do not participate in
  Core HWM accounting. No separate retained receive, raw lease handle, or
  application byte capacity exists in a public or internal API. Ordinary typed receive preserves
  ROUTER routing ID and `ReplyToken`, and SUB topic/routing ID metadata.
- Non-blocking no-data is distinguished from a hard receive failure.
- A SPOT readable dispatch event is a readiness notification. The caller drains the matching receive API until it returns no-data.
- Returned message data has clear ownership and lifetime. Borrowed data never outlives its native owner.
- A service control/admission receive path, such as Actor join request receive, may use `Option`, a nullable equivalent, or a typed result-return form when that's clearer than the reusable data-plane storage. Even then, no-data and a hard receive failure are kept distinct.

## Error and validation policy

- A native fixed-size id or string is validated before crossing the FFI boundary.
- Routing id, actor id, endpoint, channel name, and topic are never silently truncated.
- The submit, request, recv, handler, close, bind, connect, and config error domains are preserved.
- A public error must be checkable via a Rust type, not by parsing a string.

## Performance policy

- The hot path does not use avoidable dynamic dispatch, avoidable allocation, avoidable byte copies, hidden sleeps, busy waits, broad locks, or thread joins.
- FFI bridge code must materialize public Rust values directly from the core part substrate.
- It does not create a thread or timer per request when progress can be shared per handle.
- Perf, samples, and tests use only the public crate API.

## Implementation checklist

- Public exports are intentional and documented.
- Raw FFI and unsafe state never leak through the public API.
- Resource ownership is enforced by Rust types and `Drop`.
- A trait is used only at a genuine abstraction point.
- A public free function and a builder convenience method are declared in a public crate module, not a runtime helper.
- Receive/subscription semantics match the common binding policy.
- Exceptions where service control/admission receive differs from the data plane's caller-provided storage are documented.
- Perf semantics match `bindings/c/perf`.
- `src/contracts` has no import or export dependency on `src/runtime`.
- `lib.rs` imports a runtime module only to wire up constructors, and does not export a runtime module or a runtime implementation type name.
- Tests, samples, and perf do not use private runtime imports.
- A native-backed resource is created via a crate root constructor or a contract method, and typed as a public contract type.
- The public surface has no compatibility-only alias, duplicate operation-start name, or deprecated
  wrapper.

Run the required verification commands from `bindings/rust/`.

- Run `cargo fmt --all --check`.
- Run `cargo test --workspace --all-targets`.
- Where clippy is available, run `cargo clippy --workspace --all-targets -- -D warnings`.
- Run `./tests/run_tests.sh`.
- Run `./samples/run_samples.sh` when a public example or a generation path changed.
- Run `./perf/run_benchmarks.sh` and `./perf/run_benchmarks_multi.sh` as a smoke gate when hot-path, receive, send, request, poller, timer, or service behavior changed.
- Inspect rustdoc/public re-exports to confirm the crate export surfaces contract types, not runtime implementation modules.
- Search `src/contracts`, `tests`, `samples`, `perf` for imports from `crate::runtime`, `runtime::native`, a raw FFI module, or a generated private file. Check `src/lib.rs` separately to confirm a runtime import is used only to wire up constructors and never appears in a public signature.

## Actor and Spot Route results

Rust exposes Actor and Spot route lookup results as public value types.

- `ActorRoute` preserves the resolved Actor ref, the Actor node RID, the current Spot RID, and the current Spot kind.
- `SpotRoute` preserves the Spot RID, the owner node RID, and the Spot kind.
- `SpotKind` distinguishes an Entry Spot from a user Spot. An invalid kind is not a successful route result.
- A SpotNode snapshot entry exposes the same Spot kind/current Spot fields as the core snapshot.

- Rust exposes `SpotNode::send_to_actor(&ActorRef)` and `SpotNode::request_to_actor(&ActorRef)`, which take a resolved Actor ref.
- The send operation, once submit succeeds, transfers ownership of one or more message parts, and completes once the Actor owner's mailbox takes them over.
- The request operation, once submit succeeds, transfers ownership of the request part and delivers the reply part the Actor handler produced.
- Rust must not resurrect the removed Discovery route table or resolver API as a compatibility helper.

## Pull completion public contract

The Rust crate uses Core 0.16.0 as an exact dependency.

The Rust runtime drains native completions and converts them into blocking `Result` values or
runtime-independent Futures. `submit_sync()` uses Core `NONE`; `submit()` uses Core `DONTWAIT`.
Completion-backed state is registered in a provisional registry before native `FINAL` and completes
exactly once after submit publication and completion capture join. Future drop or executor task abort
ends only the waiter and does not cancel the Core operation; a late completion releases the payload and
state.

`POLLCOMPLETION` is a progress event indicating that the public poller's wait thread drained the native
queue and fully processed at least one live Future or detached state. Under public poller ownership,
using a blocking request requires another thread to continue executing the wait loop.

A `ReplyToken` carries both an `Arc<RouterOwnerTag>` created by the ROUTER wrapper and an opaque value.
Only `pub(crate) fn from_native(owner, value)` creates a token; equality, hashing, and reply-owner
validation use owner-tag identity and the value. `StreamPacket` is a reusable output, but `close(self)`
is a consuming terminal. A token provides no `Default`, raw numeric conversion, ordering,
serialization, or close operation. Concurrent recv into the same output is invalid-state. Message
references remain valid only until the next recv entry or output drop. Before the first bind/connect,
`set_recv_mode` accepts only `Raw` and `Packet` and rejects `Unspecified`.

### Public interface

```rust
#[derive(Clone)]
pub struct ReplyToken {
    owner: Arc<RouterOwnerTag>,
    value: u64,
}

impl PartialEq for ReplyToken { /* Arc::ptr_eq(owner) + value */ }
impl Eq for ReplyToken {}
impl std::hash::Hash for ReplyToken { /* owner tag address + value */ }

impl std::fmt::Debug for ReplyToken {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.write_str("ReplyToken")
    }
}

impl SendOp<Ready> {
    pub fn submit(
        self,
    ) -> impl Future<Output = Result<(), SubmitError>> + Send;
    pub fn submit_sync(self) -> Result<(), SubmitError>;
}

impl RequestOp<Ready> {
    pub fn timeout(self, timeout: Duration) -> Self;
    pub fn submit(
        self,
    ) -> impl Future<Output = Result<Vec<Message>, ZlinkError>> + Send;
    pub fn submit_sync(self) -> Result<Vec<Message>, ZlinkError>;
}

impl Received {
    pub fn reply_token(&self) -> Option<ReplyToken>;
}

impl ReplyOp<Ready> {
    pub fn submit(self) -> Result<(), SubmitError>;
}

#[repr(i32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum StreamRecvMode {
    Unspecified = 0,
    Raw = 1,
    Packet = 2,
}

pub struct StreamPacket {
    routing_id: Option<RoutingId>,
    header: Option<Message>,
    body: Option<Message>,
}

impl Default for StreamPacket {
    fn default() -> Self;
}

impl StreamPacket {
    pub fn empty() -> Self;
    pub fn is_empty(&self) -> bool;
    pub fn routing_id(&self) -> Option<&RoutingId>;
    pub fn header(&self) -> Option<&Message>;
    pub fn body(&self) -> Option<&Message>;
    pub fn close(self) -> Result<(), CloseError>;
}

impl StreamSocket {
    pub fn recv_packet(
        &self, out: &mut StreamPacket, flags: RecvFlags,
    ) -> Result<bool, RecvError>;
}

impl StreamSocketOptions<'_> {
    pub fn recv_mode(&self) -> Result<StreamRecvMode, ConfigError>;
    pub fn set_recv_mode(
        &self, mode: StreamRecvMode,
    ) -> Result<(), ConfigError>;
}
```

The operation-start signatures are PAIR `send(&self) -> SendOp<Empty>`, DEALER
`send(&self) -> SendOp<Empty>` and `request(&self) -> RequestOp<Empty>`, ROUTER
`send(&self, &RoutingId) -> SendOp<Empty>`,
`request(&self, &RoutingId) -> RequestOp<Empty>`, and
`reply(&self, &RoutingId, ReplyToken) -> ReplyOp<Empty>`, and STREAM
`send(&self, &RoutingId) -> SendOp<Empty>`. A send factory captures the target in the builder.
`Received::send()` returns a `SendOp<Empty>` that captures the source target, and
`Received::reply()` returns a `ReplyOp<Empty>` that captures the source RID and token.

The public Rust surface contains no `RoutedSendOp`, send `.timeout(Duration)`, `RequestCallbackOp` or
`on_reply()`, `ReplyOp::flags()`, STREAM callback, `SocketMonitor::on_event()`, `ignore_handler()`, or
`snapshot()`, `Timer::on_fire()`, or pair/generation member.

Monitor provides `recv()`, `recv_with_flags(RecvFlags)`, `status()`, and `close(&mut self)`. Monitor
DONTWAIT no-data is `Ok(None)`. Timer provides `start(&self, u64, u64)`, `stop(&self)`, and
`recv() -> Result<Option<u64>, RecvError>`; `Drop` performs lifecycle cleanup. The native-header mirror
contains only `ZLINK_OPT_PENDING_MAX_MSGS` and `ZLINK_OPT_PENDING_MAX_BYTES` as pending options.
Monitor-event `connection_id` is used only for diagnostics and correlation, not as a send/reply target
or reconnect fence. Pending native options add no public high-level option method.

## Implementation and contract-test verification requirements

Verify the following using only the public Rust interface, `Result`, Futures, and poller events. Each
item maps to one contract test.

**Operations and completion**

- Every socket's send factory returns `SendOp<Empty>`, and send/request expose only the flag-free Future
  and synchronous terminals in the Public interface section.
- Even when completion drains before submit returns, the Future completes exactly once after joining
  submit publication.
- After Future drop or task abort, a late completion does not complete the Future again and releases the
  native aggregate.
- A non-OK request completion exposes only typed `ZlinkError` and does not expose the error payload.
- `POLLCOMPLETION` returns only after Future settlement or detached cleanup finishes.
- When HWM/`PAUSED` waiting expires for a raw reply submitted to a DEALER peer,
  `Err(SubmitError)` carries the `BACKPRESSURED` code; a reply submitted to a
  ROUTER peer retains the HWM-free result of the Completion connection.

**ReplyToken and STREAM**

- A token is `Clone` but not `Copy` or `Default`, provides no raw numeric conversion, and its `Debug`
  output does not expose the raw value.
- Moving a ROUTER wrapper preserves owner-tag identity. After close and recreation, reply with a stale
  token fails before the native call.
- `recv_packet()` fills the output after success and leaves it empty on no-data or error; consuming
  `close(self)` and `Drop` do not release the payload twice.

**Pull eventing**

- Monitor and timer recv distinguish no-data as `Ok(None)` and expose events and fire counts without
  callbacks.
