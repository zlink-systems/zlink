# zlink Node Binding API Reference

This reference is generated from the TypeScript source in `bindings/node/src/`.

## Prerequisites

```bash
npm install --save-dev typedoc
```

## Generate

```bash
cd bindings/node
npx typedoc
```

Generated HTML entrypoint:

```text
bindings/node/typedoc/html/index.html
```

## Scope

- Public exports from `src/index.ts`
- Socket types and domain objects
- Message and result types (`Message`, `Received`, `SubmitError`)
- Constants and enums (`SocketType`, `PollEventFlag`, `CompletionKind`)
- Internal symbols marked `@internal` are excluded

## Completion and Backpressure

The binding settles managed SEND and REQUEST Promises by pulling Core
completion records. Managed send uses `submit()`/`submit_sync()`, request uses
the matching terminals returning reply parts, and STREAM, socket-monitor, and
timer input uses caller-driven pull APIs.

Successful SEND admission returns completion ID `0` and publishes no SEND
completion. For async `submit()`, a DONTWAIT `SubmitResult.Backpressured` result
with `EAGAIN` contains a nonzero wait token while Core retains no payload. The
Node runtime keeps the submit-time packet snapshot. When no public poller owns
completion draining, it watches the socket notification descriptor and pulls
the completion queue. A
`CompletionKind.Writable` record must match the token, user context, and target
routing ID before the runtime resubmits that packet. Caller mutation after
`submit()` does not alter the retry. A `Message` input is consumed as soon as
the binding takes the back-pressure snapshot; the retry does not retain that
wrapper until the Promise resolves. REQUEST uses the same WRITABLE-token retry
before admission, then settles through its matching `CompletionKind.Request`
reply or timeout. A public poller registered for
`PollEventFlag.PollCompletion` owns completion draining until its registration
drops that flag, the socket is removed, or the poller is closed. The application
must keep calling `poller.wait(...)` until its pending SEND and REQUEST Promises
settle. The raw Core ABI-only `ZLINK_OPT_PENDING_MAX_*` values and storage are
retained but ignored. The typed Node socket options do not expose them.
