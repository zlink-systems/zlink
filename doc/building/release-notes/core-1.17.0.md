[English](./core-1.17.0.md) | [한국어](./core-1.17.0.ko.md)

# libzlink 1.17.0 release notes

## Changes

- On `ws` and `wss`, Core sends each STREAM message as a separate WebSocket message. Previously, Core could combine multiple length-prefixed STREAM frames in one WebSocket message, and the receiver closed the connection with `frame length does not match prefix` (#1545, #1553).
- Renamed local buffers from `small` to `short_buffer` in `unittest_recv_admission.cpp` to avoid a Windows SDK macro collision that prevented the test from compiling on Windows (#1553).
