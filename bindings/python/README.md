# Python Binding

Python binding for the Core raw messaging API. The [Core release
metadata](../../VERSION) records the Core version. The public surface is `bindings/doc/spec/python/README.ko.md`; repository policy
and the socket capability matrix live in `bindings/README.md`, and the native
ABI comes from `core/include/zlink.h`.

The binding is multipart-only and exposes typed `ContextOptions`, socket option
facades, `Message.ref_count()`, `RoutingId`, `Received`, `ReplyToken`, and
`StreamPacket`. Socket capabilities remain split across `PairSocket`,
`DealerSocket`, `RouterSocket`, `StreamSocket`, `PubSocket`, `SubSocket`,
`XPubSocket`, and `XSubSocket`.

## Socket endpoints and TLS

Use `bind(endpoint)` to open a local endpoint and `unbind(endpoint)` to close
it. Configure TLS with `set_tls_server(cert, key, require_client_cert=False)`
before binding or `set_tls_client(ca_cert, hostname, trust_system=False)` before
connecting.

## Completion contract

`SendOp.submit()` and `RequestOp.submit()` make a nonblocking submission and
return a `SendSubmission` or `RequestSubmission`. Read the initial result
from `submission.result` and use `await submission.admitted` to wait for
admission. An immediately admitted send already has a completed admission
stage. For a request, use `await submission.reply` to get the caller-owned
reply parts. The reply timeout starts after admission.

Register the socket with a public poller for `PollEventFlag.POLLCOMPLETION`
before submitting an asynchronous request or a send that may need to wait for
admission. Keep calling `Poller.wait()` while those stages are pending. A matching
`WRITABLE` record allows the binding to retry a backpressured submission. A
`REQUEST` record resolves the admitted request. Cancelling an awaitable detaches
the caller's wait. The binding continues to process completions and closes
late reply parts. See the [async request sample](samples/request_reply_async_sample.py).

Use `SendOp.submit_sync()` for blocking send admission or
`RequestOp.submit_sync()` to block until the request resolves. Without a
registered completion poller, the blocking request drains its own completion
on the calling thread.

ROUTER request receive supplies an opaque, owner-bound `ReplyToken`. Only the
originating router accepts it, and `ReplyOp.submit()` has no flags. PUB/XPUB
publication remains separate as synchronous `PublishOp`; only that operation
retains `SendFlags`. Receive no-data is selected with `RecvFlags.DONT_WAIT`.

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
system or repository build paths. [The package build](setup.py) supports Linux
x86_64 and Windows x86_64.

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
