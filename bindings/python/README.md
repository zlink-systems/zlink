# Python Binding

Policy-aligned Python binding for `zlink`.

This package must stay aligned with:

- `bindings/README.md`
- `core/include/zlink.h`
- `doc/perf/PERF_POLICY.md`
- `doc/perf/PERF_SINGLE_TEST_POLICY.md`
- `doc/perf/PERF_MULTI_TEST_POLICY.md`

The Python surface is the Core 0.13.0 raw messaging contract with typed Python
ownership and error handling. The public contract is:

- multipart-only send and subscribe/receive
- the capability matrix from `bindings/README.md` is enforced by concrete
  socket types instead of one generic bag
- blocking APIs use direct names like `send`, `recv`, `publish`, `subscribe`
- non-blocking behavior is expressed through `SendFlags` and `RecvFlags`
- `Context` exposes typed `ContextOptions` instead of raw `set/get`
- `Message` exposes `data`, `to_bytes()`, and `ref_count()`
- receive and subscribe return domain objects such as `Received`,
  `TopicMessage`, `RoutingId`, and `SubscriptionEvent`
- request/reply surfaces use `request`, `reply`, and raw routing ids
- raw public option bags like `setsockopt` and `getsockopt` are not exposed
- typed option families are exposed through properties and capability objects
- monitor sockets use canonical
  `monitor_open(events=..., monitor_hwm_bytes=...)`, `recv()`, and `snapshot()`
- monitor masks use `MonitorEventMask`; decoded monitor payload uses
  `MonitorEvent`
- resource-owning types support sync and async context manager cleanup
- `*_READY_CHANGED` monitor events do not expose aggregate ready counts
- monitor snapshots are state/queue inspection surfaces, not ready-count gates
- callback registration uses canonical names `on_packet` and monitor
  `on_event`; topic subscription uses `subscribe_into()` and
  `receive_subscription_event_into()`.
- callback removal by passing `None` is not part of the public contract;
  callback lifecycle ends with socket close
- `send_ready` readiness-hint semantics is abolished
  (`bindings/doc/spec/async-coroutine-policy.ko.md`, 2nd revision). There is
  no `on_send_ready` handler; HWM-managed send completion is Core's
  `zlink_send_complete_handler` notification, delivered through the
  awaitable `submit()` already returns.

The raw FFI declaration `zlink_recv_handler()` is a private implementation
primitive. The public Python surface uses `on_packet` for STREAM packets,
`on_event` for monitor events, and the `request(...)` asynchronous terminal
for routed request completion. Python does not add a separate direct
raw-receive registration method.

## Send, Publish, Request, Raw Reply Completion

Per `bindings/doc/spec/async-coroutine-policy.ko.md` (3rd revision) and
`doc/plan/core-send-completion-design.ko.md`, bindings are pure Core
wrappers: **zero binding-owned threads, queues, or retry** anywhere in the
send/request completion surface.

- **HWM-managed send** — PAIR `send()` and DEALER/ROUTER routed `send()` —
  is ASYNC because it can pass through Core's HWM admission queue.
  `submit()` returns an awaitable coroutine object completed by Core's
  `zlink_send_complete_handler` notification (`zlink_send_async` /
  `zlink_send_async_cancel`, `core/include/zlink/socket/api.h`). Cancelling
  the awaitable maps to `zlink_send_async_cancel`. There is no per-op
  Python timer; the deadline is the Core-side
  `zlink_send_async_options_t.timeout_ms` field (the same pattern
  `_runtime/eventing/timer.py` already uses for Core-owned timing).
- **`publish`** (PUB/XPUB) is synchronous-only: `submit()` returns `None` or
  raises `SubmitError` immediately. PUB semantics are lossy by default — a
  full subscriber queue silently drops that subscriber's copy and the
  publisher never waits; `ZLINK_PUB_OPT_NODROP` surfaces an immediate
  `SubmitError` instead. `zlink_send_async` returns `ENOTSUP` for PUB/XPUB,
  so there is no publish awaitable.
- **`request`** is ASYNC; `submit()` returns an awaitable coroutine object
  completed purely by Core's reply callback (`ZLINK_REQUEST_TIMED_OUT` on
  expiry). There is no admission ticket and no polling thread driving
  completion.
- **Raw `reply()`** (on `Received`, ROUTER only) is HWM-free and
  synchronous: `submit()` returns `None` or raises `SubmitError`
  immediately.

## Surface Summary

Socket capabilities are split by type instead of one generic bag of unrelated
methods:

- `PairSocket`
- `DealerSocket`
- `RouterSocket`
- `StreamSocket`
- `PubSocket`
- `SubSocket`
- `XPubSocket`
- `XSubSocket`

Examples of policy-enforced capability boundaries:

- `SubSocket` exposes `subscribe`, `set_subscription`, and
  `unset_subscription`, but not direct `recv` or a direct subscription
  callback
- `XPubSocket` is the only Python socket surface that exposes
  `receive_subscription_event`
- `StreamSocket` keeps routed send/receive but does not expose generic
  `connect` / `disconnect`
Common hot-path helpers are value-typed:

- `Message`
- `Received` with `send(...)` for normal routed send-back over the original
  receive context and `reply(...)` for request-reply messages
- `TopicMessage`
- `RoutingId`
- `SubscriptionEvent`
- `SendFlags`
- `RecvFlags`
- `SubmitResult`
- `RequestResult`
- `RecvResult`
- `HandlerResult`
- `CloseResult`
- `BindResult`
- `ConnectResult`
- `ConfigResult`
- `Timer`
- `Stopwatch`
- `Thread`
- `AtomicCounter`

`TopicMessage` carries the raw subscription topic. `SubscriptionEvent` carries
the topic and subscription state returned by the Core socket.

## Native Library Loading

The Python binding loads the native zlink runtime through `ctypes`. A wheel
loads the native payload bundled in that wheel. The current Core 0.13.0 package
target is Linux x86_64 only. Source builds use the same target policy and
require an explicit `ZLINK_LIBRARY_PATH` or `ZLINK_CORE_PREFIX`; the loader does
not search the repository build directory or an arbitrary system library.
Other operating systems and CPU architectures fail fast until a separate Core
11 candidate supplies their native payload and clean-consumer evidence.

## Receive Buffer Lifetime

`Message.data`, received message part `data`, and related receive objects may
return a `memoryview` over native-owned storage. The view is valid only while
the owning `Message`, `Received`, `ReceivedMultipart`, `TopicMessage`, or other
receive owner remains open. Use `to_bytes()` or `to_bytes_list()` when the
payload must outlive the receive object or cross an async/task boundary.

## Boundary Rules

The Python binding fail-fast validates values before the native call when the
policy requires it:

- endpoint, topic, and subscription strings/bytes reject embedded NUL
- fixed-size endpoint inputs fail fast above the Core limit
- `RoutingId` enforces the native 255-byte maximum
- typed integer options fail on signed/unsigned overflow instead of truncating
- send/receive convenience does not change the multipart-only contract
- blocking send/publish inside receive callbacks raises an explicit error
  instead of silently degrading to non-blocking behavior; use
  `SendFlags.DONT_WAIT` or `RecvFlags.DONT_WAIT` for explicit
  non-blocking behavior

## Typed Options

The canonical Python option facades are:

- `ContextOptions`
- `CommonSocketOptions`
- `RouterSocketOptions`
- `DealerSocketOptions`
- `StreamSocketOptions`
- `PubSocketOptions`
- `SubSocketOptions`

## Verification

Run tests from `bindings/python`:

```bash
tests/run_tests.sh
```

The suite covers:

- canonical public surface and legacy API removal
- blocking/non-blocking behavioral contract
- ownership and multipart receive semantics
- raw monitor, poller, timer, and callback flows
- perf runner smoke execution

## Samples

Run the canonical sample suite from `bindings/python`:

```bash
samples/run_samples.sh
```

## Perf

The official Python perf surface lives under `perf/`.

- [perf/README.md](/home/hep7/project/kairos/zlink/bindings/python/perf/README.md)
- [perf/multi/README.md](/home/hep7/project/kairos/zlink/bindings/python/perf/multi/README.md)

Perf code is a verification surface, not a workaround layer for binding or core
bugs. Hot-path measurements should stay close to the canonical Python receive
and publish/send paths and must not hide extra copies, conversions, or helper
layers behind the benchmark wrapper.

Readiness gates in Python perf and samples must use low-cost event counting
rather than monitor payload counts or monitor snapshot ready counts.
- raw sockets: `CONNECTION_READY` event counting
