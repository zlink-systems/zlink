[English](./bindings-rust-1.12.0.md) | [한국어](./bindings-rust-1.12.0.ko.md)

# ZLink Rust binding 1.12.0 release notes

Uses Core 1.12.0. The binding version moves from 0.18.0 to 1.12.0 to match its Core version.

## Changes

- The selected ROUTER route is available in received results (#1087).
- STREAM sockets expose disconnect by RID and do not provide general connect or disconnect. An unknown RID returns NotFound (605) (#1164).
- SEND WRITABLE completions report Core results directly. A wait that times out reports BACKPRESSURED and `EAGAIN` (#1154).

## Behavior changes

- Socket close reports Core's result directly. REQUEST submission runs outside the socket lock (#1164).
- A SEND wait without a public completion owner is rejected with `InvalidState` (#1197).
- The subscription topic receive buffer grows to the length reported by Core for long topics (#1218).
- Representative errnos for REQUEST results match Core: `CONFLICT` is `EEXIST`, and `INTERNAL_ERROR` is now `EIO` instead of 0 (#1218, #1248).
