# Python Binding

Python wrapper for the Core 0.16.0 raw messaging contract. The normative
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
awaitable and uses Core `DONTWAIT`; a socket-local drain owner converts pull
completion records into the terminal result. `RequestOp` provides the same two
terminals and preserves `timeout()`. Cancelling an awaitable detaches only the
caller wait: the Core operation continues and late completion payload is still
closed.

ROUTER request receive supplies an opaque, owner-bound `ReplyToken`. Only the
originating router accepts it, and `ReplyOp.submit()` has no flags. PUB/XPUB
publication remains separate as synchronous `PublishOp`; only that operation
retains `SendFlags`. Receive no-data is selected with `RecvFlags.DONT_WAIT`.

`PollEventFlag.POLLCOMPLETION` transfers completion draining to the public
poller while registered. A reported completion event means at least one live
or detached operation was fully processed, not merely that native bytes were
ready.

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
