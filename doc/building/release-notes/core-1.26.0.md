[English](./core-1.26.0.md) | [한국어](./core-1.26.0.ko.md)

# libzlink 1.26.0 release notes

## Changes

- `zlink_disconnect(endpoint)` now ends only the connect registrations and auto-reconnect intents of that endpoint, and `zlink_unbind(endpoint)` only its bind registration and the connections accepted through it. Before, both closed every registration of the endpoint, so a disconnect could also close a listener bound on the same address and drop its inbound connections. `zlink_disconnect` on an endpoint that has only a bind now returns `ZLINK_CONNECT_NOT_FOUND` (#4846, #4864).
- A connecter uses the I/O thread of its own session (#3541, #4303).
- C++ and Java bindings: a receive that copies a borrowed routing id holds the existing call reservation so it does not overlap close (#4568; C++ #4599, Java #4597). Java message copy reuses the message slot pool header (#4570, #4588).

## Compatibility

There are no public API or ABI changes; linker SOVERSION remains 0. The disconnect change follows the existing socket specification; code that called `zlink_disconnect` on its own bound endpoint to close a listener must call `zlink_unbind`. Core and all seven binding versions are 1.26.0. Component compatibility follows the [version policy](../versioning.md).
