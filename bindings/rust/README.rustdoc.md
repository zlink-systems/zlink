# zlink Rust Binding API Reference

This reference is generated from the Rust source in `bindings/rust/src/`.

## Generate

```bash
cd bindings/rust
cargo doc --no-deps
```

Generated HTML entrypoint:

```text
bindings/rust/target/doc/zlink/index.html
```

## Scope

- Public API of the `zlink` crate
- Public socket and monitor types re-exported at the crate root
- Domain objects (`Message`, error types, enums)
- FFI internals (`zlink::ffi`) are private and excluded

The current approved crate payload contains the Core 0.17.0 ABI with the 0.17.0
Linux x86_64 runtime. Other target triples fail during build until a matching native
runtime has passed the Core package provenance and clean-consumer checks.

## DONTWAIT SEND/REQUEST and WRITABLE

- A DONTWAIT SEND makes one admission attempt. Immediate admission succeeds
  with completion ID `0`; Core does not enqueue a SEND completion.
- HWM, missing credit, flow pause, or an existing-but-unready target returns
  `SubmitResult::Backpressured` with `EAGAIN` and a nonzero wait token. Core
  retains the token, user context, and target, but never the payload.
- Once the target can accept another attempt, Core enqueues
  `CompletionKind::Writable` and wakes pollers with `POLLOUT`. WRITABLE grants
  one retry; it is not notification that the original SEND succeeded.
- ROUTER and STREAM sends to a routing ID that has no route fail as
  `SubmitResult::NotConnected` without a wait token.

`SendOp::submit()` and `RequestOp::submit()` implement the managed asynchronous
form. Each retains the exact multipart packet, pulls completion records through
NO_DATA after `POLLOUT`, matches the token, user context, and routed target,
then resubmits that packet. A repeated backpressure result arms the new token
and repeats the same state transition. After REQUEST admission, its future
waits for the existing REQUEST reply or terminal completion. Without a public
poller the socket's completion queue is drained by one binding reactor thread
per socket that blocks in a native poller on `POLLCOMPLETION`, starts with the
first wait token or REQUEST, and retires when no operation is outstanding;
parked futures are woken by that thread, never by executor re-polling.

Registering a socket with a public `Poller` for `POLLCOMPLETION` transfers
completion-queue ownership to that poller. Include `POLLOUT` in the mask and
keep calling `Poller::wait()` while it drives backpressured SEND or REQUEST
futures.
REQUEST admission can use the same WRITABLE path. A successful REQUEST FINAL
reserves a different nonzero completion ID and then completes with its reply or
terminal result through `CompletionKind::Request` and `POLLCOMPLETION`.

`CompletionKind::Send` remains public with ABI value `1`, but it is reserved:
ordinary SEND success never produces that record. `CompletionKind::Request`
is `2`, and `CompletionKind::Writable` is `3`. PUB/XPUB publish remains a
synchronous one-shot and produces neither SEND nor WRITABLE completions;
`SendFlags::DONT_WAIT` backpressure is returned as `SubmitError` with
`SubmitResult::Backpressured` and native `EAGAIN`.

`ZLINK_OPT_PENDING_MAX_MSGS` and `ZLINK_OPT_PENDING_MAX_BYTES` retain their ABI
values and set/get storage but are ignored. Core owns no pre-admission SEND or
REQUEST payload pool, and the typed Rust socket option surface does not expose
them as flow-control settings.

## Context Thread Safety

`Context` is `Send` and `Sync`. Applications may share it across threads, for
example with `std::sync::Arc`, and those threads may create sockets from the
same context. The context is terminated when the last owning `Context` value is
dropped. Keep an owning reference alive while another thread is creating or
using sockets.

## Monitor Contract Note

- `*_READY_CHANGED` monitor events are readiness edge/state notifications.
- Rust bindings must not interpret monitor `value` as an aggregate ready count.
- Monitor snapshots are for state/queue inspection, not ready-count gates.
- Perf or readiness verification in Rust bindings must follow the shared perf
  policy.
- raw sockets: `CONNECTION_READY` event counting
- SPOT: explicit benchmark barrier protocol; no separate service-event gate
