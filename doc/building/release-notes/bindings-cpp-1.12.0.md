[English](./bindings-cpp-1.12.0.md) | [한국어](./bindings-cpp-1.12.0.ko.md)

# ZLink C++ binding 1.12.0 release notes

Uses Core 1.12.0.

## Changes

- STREAM sockets do not provide connect or disconnect and provide `disconnectRid`. A RID that does not exist returns connect error NotFound (605).
- Exposes `stream_socket_t::disconnect_rid` (PR #1194).

## Behavior changes

- Exposes the REQUEST result `backpressured` (113). Its errno is `EAGAIN`, matching Core (#1198).
- Aligns representative errnos for REQUEST results with Core: `CONFLICT` returns `EEXIST`, and `INTERNAL_ERROR` returns `EIO` (#1248).
- Removes the single-part receive overloads from DEALER and ROUTER. Code receiving multipart messages must use the API that returns the parts list (#1231).
