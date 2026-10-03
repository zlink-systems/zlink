[English](./bindings-rust-1.13.1.md) | [한국어](./bindings-rust-1.13.1.ko.md)

# ZLink Rust binding 1.13.1 release notes

Uses Core 1.13.0.

## Changes

- `set_linger`, `set_send_timeout`, `set_receive_timeout` and the matching getters take and return Core's `i32` milliseconds instead of `Duration`. `-1` means unlimited waiting, which a `Duration` could not express (#1164).
  - Callers pass `n` instead of `Duration::from_millis(n)`.
