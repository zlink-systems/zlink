[한국어](README.ko.md) | English

[Go binding spec](../../spec/go/README.en.md) · [Go binding guide](../../guide/go/index.en.md)

# Go bindings reference

The writing rules follow the
[Reference-writing guide](../../../../doc/principal/documentation/reference-writing-guide.ko.md)
(Korean-only). This is the bindings layer (the Core C ABI's language projection) — not the
framework layer, which has its own reference tree under `framework/doc/framework/go/reference/`.

Categories follow the [.NET binding spec](../dotnet/README.en.md)'s Contract Folder Layout as the
common architecture map. As with every wrapper binding so far, this tree has five categories, not
six — `contracts/` has no `service.go`; SPOT/Actor exists only at the framework layer.

**`contracts/` itself is a re-export shim** — every type in `contracts/*.go` is a `type X =
impl.X` alias and every function/const is a `var`/`const` pointing at
`internal/native` (an unexported package). The actual method signatures documented in this tree
are read from `internal/native/*.go`, not from `contracts/`, since the alias files carry no
members of their own.

Go-specific notes carried into every category below:

- **Socket creation is a method on `Context`** (`ctx.PairSocket()`, `ctx.DealerSocket()`, ...), not
  a free function at a package root — the only wrapper binding covered so far where this is a
  `Context` method rather than a top-level factory or a static-facade method.
- **No per-type option facade exists for Dealer/Router/Stream/Sub** — their specific options
  (`SetProbe`, `SetWeight`, `RequestTimeout`, `SetMandatory`, `SetHandover`,
  `SetConnectRoutingID`, `SetNotify`/`Notify`, `TopicsCount`) are declared as direct methods on the
  socket type itself. Only `CommonSocketOptions` (via `.CommonOptions()`) and `PubSocketOptions`
  (via `.PubOptions()`) exist as separate accessor objects — every other language covered so far
  gives every socket type its own named options facade.
- **`RequestOp` has both a Go-channel async path and a callback path** —
  `SubmitAsync(ctx) (<-chan RequestReplyCompletion, error)` alongside `Submit(ctx, callback) (bool,
  error)` — unlike rust, which is callback-only, or dotnet/java/node/cpp, which use a
  Task/CompletionStage/Promise/`async_result_t`. Every builder's terminal `Submit`/`SubmitAsync`
  takes a `context.Context` as its first argument for cancellation, which no other language's
  operation builder does.
- **`ZlinkError` is a Go `interface`**, not a struct or enum — each typed error implements
  `Error() string`, `Code() int`, `InternalErrno() int`, and `Unwrap() error` (the standard-library
  `errors.Is`/`errors.As` integration point), per its own doc comment.

## Locale convention

Every `bindings/doc/spec/<lang>/` document is English-original, Korean-translation (unlike the
framework's interface-catalog convention). This reference tree follows the same direction: write
`.en.md` first, `.ko.md` second, and every spec citation links to the same-locale spec file.

## Category

| Category | Status | Contract source (`contracts/*.go` aliases → verified against `internal/native/*.go`) |
|---|---|---|
| [Core](01-core.en.md) | Drafted | `contracts/core.go` → `internal/native/context.go`, `utility.go` |
| [Messaging](02-messaging.en.md) | Drafted | `contracts/messaging.go` → `internal/native/message.go`, `received.go`, `topic_message.go`, `subscription_event.go`, `operations.go`, `request_reply_types.go` |
| [Sockets](03-sockets.en.md) | Drafted | `contracts/sockets.go` → `internal/native/socket_core.go`, `socket_types.go`, `socket_options.go`, `connection_socket.go`, `socket_direct.go`, `socket_routed.go`, `socket_publish.go`, `socket_subscribe.go`, `socket_completion_control.go` |
| [Eventing](04-eventing.en.md) | Drafted | `contracts/eventing.go` → `internal/native/monitor.go`, `poller_timer.go` |
| [Errors](05-errors.en.md) | Drafted | `contracts/errors.go` → `internal/native/error.go`, `result_codes.go` |

This document tree is wired into `mkdocs.yml` nav.
