[English](./bindings-rust-1.19.0.md) | [한국어](./bindings-rust-1.19.0.ko.md)

# ZLink Rust binding 1.19.0 release notes

Uses Core 1.19.0.

## Changes

- Core 1.19.0: Fixed a defect in `zlink_socket_set_receive_flow_state` that wrote controls to pipe send ends after releasing the socket turn. A send on another thread could overlap with the control writer, causing assertions at `pipe.cpp:795/798` or a SIGSEGV. The socket turn now remains held through the last control write and deferred control flush (#1622, PR #1623).
