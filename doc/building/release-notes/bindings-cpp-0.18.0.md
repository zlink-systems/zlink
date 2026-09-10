[English](./bindings-cpp-0.18.0.md) | [한국어](./bindings-cpp-0.18.0.ko.md)

# ZLink C++ binding 0.18.0 release notes

A **breaking release** on Core 0.18.0. See the [bindings 0.18.0 release notes](./bindings-0.18.0.md) for the full overview and migration.

## Highlights (breaking)

- The async terminal `async()` returns a result object.
  - `send_submission_t { zlink_submit_result_t result; async_result_t<void> admitted; }`
  - `request_submission_t { result; admitted; async_result_t<std::vector<message_t>> reply; }`
- `result` is a submit-time `OK`|`BACKPRESSURED` snapshot. When `OK`, `admitted` is already complete; when `BACKPRESSURED`, it completes via WRITABLE resubmission. An admission failure propagates the same cause to `admitted` and `reply` (exactly once).
- Synchronous `submit()`, publish, and reply are unchanged.

## Migration

- send: `co_await op.async()` → `co_await op.async().admitted`
- request: `auto reply = co_await op.async()` → `auto sub = op.async(); auto reply = co_await sub.reply`

## Verification

- Both contract-test scenarios pass, the full CTest is green, and perf (G6) criterion 2·4 PASS (FB-071).

The release tag is `cpp/v0.18.0`.
