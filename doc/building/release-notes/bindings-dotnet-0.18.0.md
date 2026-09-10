[English](./bindings-dotnet-0.18.0.md) | [한국어](./bindings-dotnet-0.18.0.ko.md)

# ZLink .NET binding 0.18.0 release notes

A **breaking release** on Core 0.18.0. See the [bindings 0.18.0 release notes](./bindings-0.18.0.md) for the full overview and migration.

## Highlights (breaking)

- The async terminal `Async()` returns a result object.
  - `readonly struct SendSubmission { SubmitResult Result; Task Admitted; }`
  - `readonly struct RequestSubmission { Result; Admitted; Task<IReadOnlyList<Message>> Reply; }`
- **`TrySubmit()` removed** — `Result == BACKPRESSURED` replaces it. `ct` applies to both `Admitted` and `Reply`.
- `Result` is a submit-time `OK`|`BACKPRESSURED` snapshot. An admission failure propagates the same cause to `Admitted` and `Reply` (exactly once).
- Synchronous `Submit()`, publish, and reply are unchanged.

## Migration

- send: `await op.Async()` → `await op.Async().Admitted`
- request: `await op.Async()` → `await op.Async().Reply`
- Replace `TrySubmit()` call sites with the `Result == BACKPRESSURED` check on `Async()`.

## Verification

- Both contract-test scenarios pass ×5, the full .NET binding suite is 249/249 green, and perf (G6) criterion 2·4 PASS (FB-071).

The release tag is `dotnet/v0.18.0` (NuGet `Zlink`).
