[English](./bindings-node-1.17.0.md) | [한국어](./bindings-node-1.17.0.ko.md)

# ZLink Node.js binding 1.17.0 release notes

Uses Core 1.17.0.

## Changes

- Core 1.17.0 sends each STREAM message as a separate WebSocket message on `ws` and `wss`. Previously, Core could combine multiple length-prefixed STREAM frames in one WebSocket message, and the receiver closed the connection with `frame length does not match prefix` (#1545, #1553).
