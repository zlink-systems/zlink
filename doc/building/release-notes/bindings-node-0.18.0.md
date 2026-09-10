[English](./bindings-node-0.18.0.md) | [한국어](./bindings-node-0.18.0.ko.md)

# ZLink Node.js binding 0.18.0 release notes

A **breaking release** on Core 0.18.0. See the [bindings 0.18.0 release notes](./bindings-0.18.0.md) for the full overview and migration.

## Highlights (breaking)

- The async terminal `submit()` returns a result object synchronously.
  - `SendSubmission { result: SubmitResult; admitted: Promise<void>; }`
  - `RequestSubmission { result; admitted; reply: Promise<Message[]>; }`
- `result` is a submit-time `OK`|`BACKPRESSURED` snapshot (a synchronous field). When `OK`, `admitted` is complete; when `BACKPRESSURED`, it completes via WRITABLE resubmission. An admission failure propagates the same cause to `admitted` and `reply` (exactly once).
- Synchronous `submit_sync()`, publish, and reply are unchanged.

## Migration

- send: `await op.submit()` → `await op.submit().admitted`
- request: `const reply = await op.submit()` → `const sub = op.submit(); const reply = await sub.reply`

## Verification

- Both contract-test scenarios pass, the Node test suite is green, and perf (G6) criterion 2·4 PASS (FB-071).

The release tag is `node/v0.18.0` (npm `@zlink-systems/zlink`).
