# zlink Go Binding API Reference

The Go binding is implemented in the `zlink.systems/zlink/v11` module under
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

The exported Go package reflects the Core 11 raw-socket contract.

- multipart-only public send/receive APIs
- blocking methods use direct names such as `Send`, `Recv`, `Publish`,
  `Subscribe`
- non-blocking methods use `Try*`
- non-blocking submit returns `(false, nil)` only for temporary backpressure
- non-blocking receive returns `(value, ok, error)`
- message diagnostics expose only the properties provided by the Core 11 raw API
- context options are exposed via `Context.Options()` and `ContextOptions`
- typed domain objects are used for `Message`, `RoutingID`, `Received`,
  `TopicMessage`, `SubscriptionEvent`, and `MonitorEvent`
- raw option bags and raw flags are not exposed publicly
- socket-specific capabilities are exposed only on concrete socket types
- monitor open APIs take typed masks and default to `ALL` when omitted
- all Core 11 monitor mask and delivered event values have typed constants;
  use `MonitorEventMask` for opening and `MonitorEventType` for event values
- poller registrations borrow socket and timer handles; remove a source before
  closing it and serialize one poller's add/modify/remove/wait operations
- callback delivery hops off native callback threads onto Go-managed
  dispatcher goroutines before user handlers run
- ROUTER completion-control records use `OnCompletionControl` and
  `CompletionControl`; native part callbacks remain internal

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
