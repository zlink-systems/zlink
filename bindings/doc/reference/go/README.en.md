[한국어](README.ko.md) | English

[Go binding spec](../../spec/go/README.en.md) · [Go binding guide](../../guide/go/index.en.md)

# Go bindings reference

Import `zlink.systems/zlink` to use the Go binding. Create sockets through `Context` methods such as
`ctx.DealerSocket()`. Select a socket by its role, then use its typed options and operation builders.

The `zlink.systems/zlink/contracts` package defines values, enums, and errors. The root package
re-exports those types and exposes the native-backed handles.

- Access common options with `CommonOptions()` and PUB/XPUB-specific options with
  `PubOptions()`. DEALER, ROUTER, and STREAM expose their own options as socket methods.
- Request `Submit(ctx)` returns a `RequestSubmission`. Call `Reply(ctx)` for reply parts
  or a terminal error. When a public poller handles `PollCompletion`, drive the poller
  in a separate goroutine while waiting for the result.
- The `ZlinkError` interface exposes `Error()`, `Code()`, and `InternalErrno()`.
  Each typed error also implements `Unwrap()` for use with `errors.Is` and `errors.As`.

This reference follows the [reference-writing guide](../../../../doc/principal/documentation/reference-writing-guide.ko.md) (Korean).

## Locale convention

Every `bindings/doc/spec/<lang>/` document is English-original, Korean-translation (unlike the
framework's interface-catalog convention). This reference tree follows the same direction: write
`.en.md` first, `.ko.md` second, and every spec citation links to the same-locale spec file.

## Categories

- [Core](01-core.en.md)
- [Messaging](02-messaging.en.md)
- [Sockets](03-sockets.en.md)
- [Eventing](04-eventing.en.md)
- [Errors](05-errors.en.md)
