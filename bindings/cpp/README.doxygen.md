# zlink C++ Binding API Reference

This reference is generated from the C++20 public contract headers in
`bindings/cpp/include/zlink`.

## Generate

```bash
cd bindings/cpp
doxygen Doxyfile
```

Generated HTML entrypoint:

```text
bindings/cpp/doxygen/html/index.html
```

## Scope

- Public C++20 contract headers in `include/zlink/`
- Contract projections for core, messaging, sockets, eventing, and errors
- Runtime-backed public types (`context_t`, `socket_t`, `message_t`, `poller_t`, etc.)
- `context_t::options()` exposes the typed `context_options_t` facade
- `message_t` diagnostics expose `ref_count()`
- `socket_monitor_t` is the public monitoring wrapper for socket-level events and snapshots

## DONTWAIT send and completions

At the raw API boundary, a DONTWAIT send that cannot be admitted returns
`ZLINK_SUBMIT_BACKPRESSURED`, sets `errno` to `EAGAIN`, and returns a nonzero
wait token. Core retains the token, target routing id, and `user_context`, but
does not retain the packet payload. After write credit returns, `POLLOUT` wakes
the event loop. The completion queue must then be pulled through
`ZLINK_RECV_NO_DATA`; only a `ZLINK_COMPLETION_WRITABLE` carrying the same token,
context, and routing id identifies the operation whose retained packet may be
submitted again. A normally admitted SEND returns completion id 0 and produces
no SEND completion.

The high-level C++ `send().message(...).async()` terminal performs that
bookkeeping and retains the packet needed for retry. Register the socket with a
public `poller_t` for `pollcompletion`, the exact level-triggered signal for the
socket-local completion drain. Aggregate `pollout` may also be requested for
application readiness, but it does not identify a retry token and is not needed
for async progress. Keep `pollcompletion` registered until the async result
becomes terminal. During `poller_t::wait()`, the binding drains completions,
matches the WRITABLE token and context to the waiting send, and retries the same
packet. Once an attempt is admitted, the async result completes without waiting
for a SEND completion. This event-loop path creates no binding-owned OS thread,
sleep, or timer.

REQUEST processing is unchanged and continues to use `pollcompletion` for its
reply completion. `ZLINK_OPT_PENDING_MAX_MSGS` and
`ZLINK_OPT_PENDING_MAX_BYTES` limit REQUEST pending admission only; their
numeric values remain ABI-stable and they are no-ops for SEND.
