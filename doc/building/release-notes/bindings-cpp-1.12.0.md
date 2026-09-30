[English](./bindings-cpp-1.12.0.md) | [한국어](./bindings-cpp-1.12.0.ko.md)

# ZLink C++ binding 1.12.0 release notes

Uses Core 1.12.0.

## Changes

- STREAM sockets do not provide connect or disconnect and provide `disconnectRid`. A RID that does not exist returns connect error NotFound (605).
- Exposes `stream_socket_t::disconnect_rid` (PR #1194).
