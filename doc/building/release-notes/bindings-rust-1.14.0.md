[English](./bindings-rust-1.14.0.md) | [한국어](./bindings-rust-1.14.0.ko.md)

# ZLink Rust binding 1.14.0 release notes

Uses Core 1.14.0.

## Changes

- Includes the Core 1.14.0 connection termination, terminal ERROR write ordering, handshake timeout, and session lifetime fixes (#1434).
- `linger`, send timeout, and receive timeout getters and setters use signed `i32` milliseconds, including Core's `-1` value for unlimited waiting (#1164). These signatures replace the `Duration` signatures from 1.13.0; callers must pass milliseconds. This change was included in 1.13.1.
