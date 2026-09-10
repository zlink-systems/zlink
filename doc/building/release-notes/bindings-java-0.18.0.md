[English](./bindings-java-0.18.0.md) | [한국어](./bindings-java-0.18.0.ko.md)

# ZLink Java binding 0.18.0 release notes

A **breaking release** on Core 0.18.0. See the [bindings 0.18.0 release notes](./bindings-0.18.0.md) for the full overview and migration.

## Highlights (breaking)

- The async terminal `submit()` returns a result object.
  - `SendSubmission { SubmitResult result(); CompletionStage<Void> admitted(); }`
  - `RequestSubmission { result(); admitted(); CompletionStage<List<Message>> reply(); }`
- `result()` is a submit-time `OK`|`BACKPRESSURED` snapshot. When `OK`, `admitted()` is complete; when `BACKPRESSURED`, it completes via WRITABLE resubmission. An admission failure propagates the same cause to `admitted()` and `reply()` (exactly once). Kotlin `await()`s `admitted()`/`reply()`.
- Synchronous `submit_sync()`, publish, and reply are unchanged.

## Migration

- send: `op.submit()` → `op.submit().admitted()`
- request: `op.submit()` (reply stage) → `op.submit().reply()`

## Verification

- Both contract-test scenarios pass ×5, Java unit/integration/Netty/Kotlin and samples are green, and perf (G6) criterion 2·4 PASS (FB-071).

The release tag is `java/v0.18.0` (Maven Central `systems.zlink:zlink`).
