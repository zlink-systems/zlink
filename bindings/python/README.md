# Python Binding

Python wrapper for the Core 0.17.0 raw messaging contract. The normative
public surface is `bindings/doc/spec/python/README.ko.md`; repository policy
and the socket capability matrix live in `bindings/README.md`, and the native
ABI comes from `core/include/zlink.h`.

The binding is multipart-only and exposes typed `ContextOptions`, socket option
facades, `Message.ref_count()`, `RoutingId`, `Received`, `ReplyToken`, and
`StreamPacket`. Socket capabilities remain split across `PairSocket`,
`DealerSocket`, `RouterSocket`, `StreamSocket`, `PubSocket`, `SubSocket`,
`XPubSocket`, and `XSubSocket`.

## Completion contract

`SendOp.submit_sync()` uses Core's blocking `NONE` path. `SendOp.submit()` is
awaitable and makes one Core `DONTWAIT` admission attempt. Immediate admission
returns completion ID `0` and produces no SEND completion. On
`BACKPRESSURED`/`EAGAIN`, Core retains only a nonzero wait token, the user
context, and the target; the binding retains the exact packet and target needed
for retry because Core does not retain the payload.

The socket-local owner blocks a daemon worker on Core `POLLCOMPLETION`, drains
the completion queue through `NO_DATA`, and resubmits the same packet only for a
`CompletionKind.WRITABLE` record with the same token, context, and routing ID.
WRITABLE grants permission to retry; it is not successful SEND notification.
The awaitable completes only when a retry is admitted. A wake FD transfers
ownership to a public poller and stops lifecycle waits; the worker uses no
fixed sleep, timer, or zero-time polling.

`RequestOp.submit_sync()` keeps Core's blocking `NONE` path. The awaitable
terminal uses the same DONTWAIT wait-token machine as SEND: it snapshots the
request only after `BACKPRESSURED`/`EAGAIN`, retries only for its matching
WRITABLE token, and starts reply-timeout handling only after admission. It then
completes from its `CompletionKind.REQUEST` record and preserves `timeout()`.
Cancelling an awaitable detaches only the caller wait: the socket-local owner
continues the required retry or completion cleanup and closes late completion
payloads.

ROUTER request receive supplies an opaque, owner-bound `ReplyToken`. Only the
originating router accepts it, and `ReplyOp.submit()` has no flags. PUB/XPUB
publication remains separate as synchronous `PublishOp`; only that operation
retains `SendFlags`. Receive no-data is selected with `RecvFlags.DONT_WAIT`.

A public poller driving pending operations registers both
`PollEventFlag.POLLOUT` and `PollEventFlag.POLLCOMPLETION`. The latter transfers
completion draining to that poller while registered; callers keep polling until
their SEND and REQUEST awaitables settle. A REQUEST record may settle an
admitted request, while a WRITABLE record only advances the matching SEND or
pre-admission REQUEST retry.

The raw ABI options `ZLINK_OPT_PENDING_MAX_MSGS` and
`ZLINK_OPT_PENDING_MAX_BYTES` keep their values and storage for ABI
compatibility but are ignored. The typed Python socket options do not expose
them.

## Pull receive and eventing

`StreamSocketOptions.recv_mode` selects `StreamRecvMode.RAW` or
`StreamRecvMode.PACKET` before bind/connect. Packet mode fills a reusable
`StreamPacket` through `recv_packet_into()`. The output is empty on no-data or
failure and can be reused after `close()`.

Monitor and timer APIs are pull-only. A monitor exposes
`monitor_open(events=..., monitor_hwm_bytes=...)`, `recv()`, `status()`, and
`close()` with `MonitorEventMask`; a `Timer` exposes `start()`, `stop()`,
`recv()`, and `close()`. Pending control values return `None`. XPUB subscription
events use `receive_subscription_event_into()`.

Resource owners support synchronous and asynchronous context manager cleanup.
Utility facades include `Stopwatch` and `AtomicCounter`.

## Poller

`Poller.add_monitor(monitor: MonitorSocket, events: PollEventFlag, slot: int)`, `modify_monitor(monitor, events)`, and `remove_monitor(monitor)` alias the socket methods; monitors accept only `POLLIN`, followed by `recv(flags=RecvFlags.DONT_WAIT)` until `None`.

## Native loading

The wheel loads its packaged native runtime. Source builds require an explicit
Core prefix or the local Core environment exported by
`bindings/tools/local_core_runtime.sh`; the loader does not search arbitrary
system or repository build paths. Supported packaged targets are Linux x86_64
and Windows x86_64.

## Verification

Run the complete contract, unit, integration, and sample gate:

```bash
tests/run_tests.sh
```

Run samples alone with:

```bash
samples/run_samples.sh
```

Perf sources under `perf/` follow `doc/perf/PERF_POLICY.md`,
`doc/perf/PERF_SINGLE_TEST_POLICY.md`, and `doc/perf/PERF_MULTI_TEST_POLICY.md`.
Phase 6 keeps those sources syntactically valid; behavioral benchmark migration
is a Phase 7 task.
