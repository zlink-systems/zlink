# zlink Go Binding API Reference

The Go binding is implemented in the `zlink.systems/zlink` module under
`bindings/go`.
Documentation is generated directly from source comments and exported symbols.

## Generate

Local package documentation:

```bash
cd bindings/go
go doc ./...
```

Browsable HTML using pkgsite:

```bash
cd bindings/go
go install golang.org/x/pkgsite/cmd/pkgsite@latest
pkgsite -http=:6060
# Open http://localhost:6060
```

## Public Surface Summary

The exported Go package reflects the Core 0.17.0 raw-socket contract.

- multipart-only public send/receive APIs
- send and request builders expose one `Submit(context.Context)` terminal;
  context cancellation ends only the caller's wait
- send and request make one native DONTWAIT admission attempt; after
  back-pressure the binding retains the logical packet, remembers Core's
  nonzero wait token, and retries the same packet only after the matching
  `CompletionWritable` record
- ordinary send admission returns completion ID zero and emits no completion;
  `CompletionSend` remains ABI-only
- admitted requests retain their nonzero request completion ID and complete
  only with the subsequent reply or timeout; replies emit no completion
- publish uses its separate flag-bearing `PublishOp`
- non-blocking receive returns `(value, ok, error)`
- ROUTER request receive exposes an opaque owner-bound `ReplyToken`
- STREAM selects RAW or PACKET before bind/connect and uses raw receive or
  reusable pull-based `StreamPacket` output respectively
- monitor and timer delivery is pull-only
- context options are exposed via `Context.Options()` and `ContextOptions`
- typed domain objects are used for `Message`, `RoutingID`, `Received`,
  `TopicMessage`, `SubscriptionEvent`, and `MonitorEvent`

When managed send or request retries may be outstanding and a public poller
owns the socket's completion drain, register both `PollOut` and
`PollCompletion`. Keep calling `Wait` from another goroutine while a managed
send or request `Submit(ctx)` is outstanding; `Wait` pulls the completion queue
to no-data. A WRITABLE-only wake advances the exact retry and remains visible
as `PollOut`, not as a successful-send `PollCompletion`.

`Publish(...).Flags(SendFlagsDontWait).Submit(ctx)` performs one attempt and
returns `(false, nil)` on back-pressure. PUB/XPUB has no wait-token completion.
The raw `ZLINK_OPT_PENDING_MAX_MSGS` and `ZLINK_OPT_PENDING_MAX_BYTES` values
remain ABI-stable but are ignored; the Go binding does not expose them as
options.

If a managed send or request context is canceled while a wait token is live,
the caller receives the context error and the retained packet is released. A
payload-free entry remains until that exact native token is pulled, and the
canceled packet is not retransmitted.

- raw option bags and raw flags are not exposed publicly
- socket-specific capabilities are exposed only on concrete socket types
- monitor open APIs take typed masks and default to `ALL` when omitted
- all Core 0.17.0 monitor mask and delivered event values have typed constants;
  use `MonitorEventMask` for opening and `MonitorEventType` for event values
- poller registrations borrow socket and timer handles; remove a source before
  closing it and serialize one poller's add/modify/remove/wait operations

## Poller

`Poller.AddMonitor(monitor *SocketMonitor, events PollEventFlag, slot uintptr) error`, `ModifyMonitor(monitor, events) error`, and `RemoveMonitor(monitor) error` alias the socket methods; monitors accept only `PollIn`, followed by `Recv(RecvFlagsDontWait)` until `RecvError.Result == RecvNoData`.

## Message Payload Lifetime

`Message.Data()` returns a zero-copy view over native message storage. The
returned slice is valid only while the `Message` remains open. Use
`Message.Bytes()` when payload data must outlive the message or cross a
goroutine boundary.

## Verification Entry Points

- `go test ./...`
- `./tests/run_tests.sh`
- `./samples/run_samples.sh`

## Scope

- exported symbols in package `zlink`
- package-level documentation in `doc.go`
- source comments on exported types and methods
