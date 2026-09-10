[English](./bindings-0.18.0.md) | [한국어](./bindings-0.18.0.ko.md)

# ZLink bindings 0.18.0 release notes

Runs on Core 0.18.0 and is a **breaking public-API release**. The asynchronous submit terminals of
all seven language bindings now return the Core submission result to the caller. Compatibility is not
maintained (pre-1.0).

## Highlights (breaking)

- **Async submit terminals return a result object.** Previously `submit()`/`async()`/`Async()` returned a
  single completion (the reply, or void). They now return a result object that separates the submit-time
  result from completion.
  - `SendSubmission`: `result` (`OK`|`BACKPRESSURED`, a submit-time snapshot) and `admitted` (the admission
    completion stage).
  - `RequestSubmission`: adds `reply` (the reply stage) — what `submit()` used to return.
  - When `result == OK`, `admitted` is already complete; when `BACKPRESSURED`, the binding retains and
    resubmits the input and completes `admitted` on WRITABLE. An admission failure propagates the same cause
    to both `admitted` and `reply` (exactly once). Submit failures other than `OK`|`BACKPRESSURED` remain
    exceptions/errors.
- **Synchronous terminals (`submit_sync()`, .NET/C++ `Submit()`/`submit()`), publish, and reply are unchanged.**
- Per-language return types: Java `SendSubmission`/`RequestSubmission` (`result()`/`admitted()`/`reply()`),
  .NET `readonly struct` (`Result`/`Admitted`/`Reply`) — **`TrySubmit()` removed** (replaced by
  `Result == BACKPRESSURED`), C++ `send_submission_t`/`request_submission_t`, Node `SendSubmission`/
  `RequestSubmission` (synchronous `result` field + `admitted`/`reply` Promises), Go `Submit(ctx)` returns a
  result object immediately plus `Result()`/`Admitted(ctx)`/`Reply(ctx)`, Rust `Result<SendSubmission>`/
  `Result<RequestSubmission>` (boxed futures), Python `SendSubmission`/`RequestSubmission`
  (`result()`/`admitted()`/`reply()`).
- **Framework** (F1/F2/F2-a): the public terminal does not expose backpressure (only the internal
  implementation consumes the result object); a synchronous blocking terminator is added (calling it from a
  runtime execution context fails with `InvalidOperation`); the messaging call contract is otherwise preserved.

## Why

Request `submit()` merged admission and reply into one stage, so a producer could not observe backpressure.
A single socket was pinned to depth 1, leaving multi-perf `ROUTER_ROUTER_REQREP` clients=1 at ~8.5k/s.
Splitting admission from reply lets a client submit continuously on `OK`, pipelining to the HWM depth —
clients=1 improved ~38× to ~300k/s (on par with the C reference), with no regression at clients=100
(design and measurement: `doc/draft/bindings-submit-result-terminal.ko.md`,
`doc/plan/fw-bench-worklog/decisions.ko.md` FB-071).

## Migration

- send: `await op.submit()` → `await op.submit().admitted` (or Go `sub, _ := op.Submit(ctx); sub.Admitted(ctx)`).
  A simple fire-and-forget send just awaits `admitted`.
- request: `reply = await op.submit()` → `sub = op.submit(); reply = await sub.reply` (Go `sub.Reply(ctx)`).
- Replace .NET `TrySubmit()` call sites with the `Result == BACKPRESSURED` check on `Async()`.
- Synchronous `submit_sync()`/`Submit()`, publish, and reply need no change.

Spec: `bindings/doc/spec/async-coroutine-policy.*` §6, `bindings/doc/spec/README.*` (submit result projection),
per-language `bindings/doc/spec/<lang>/README.*`.

## Verification

- Each of the seven bindings has two contract-test scenarios (immediate admission → `result==OK` with a
  completed `admitted`; HWM → `BACKPRESSURED`, `admitted` after WRITABLE, and `reply` for a request) and they pass.
- Zero old terminal signatures remain in the binding spec/guide (grep-verified).
- Perf (G6, Core fixed at 0.18.0): criterion 2 (clients=1 escapes depth 1) PASS, criterion 4 (no clients=100
  regression) PASS — cross-checked against the saved 0.17.5 results by size and version (FB-071).

Release tags are `<language>/v0.18.0` (four languages); go/rust/python are manifest syncs.
