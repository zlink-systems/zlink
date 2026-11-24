[English](./framework-cpp-0.34.1.md) | [한국어](./framework-cpp-0.34.1.ko.md)

# ZLink C++ Framework 0.34.1 Release Notes

Framework 0.34.1 is a hotfix for two defects in 0.34.0. It keeps the C++ binding 1.26.0 and Core 1.26.0.

## Bug fixes

- Registering a Spot timer again under the same name was rejected as a duplicate. Now the previous timer with that name retires once the new timer has started, and its remaining ticks don't run (Spot timer §1). (#5465, #5476)
- A WebSocket (`ws://`) or TLS (`tls://`) STREAM listener reported a `tcp://` address in its public status. It now reports the configured scheme with the actual address and port. (#5460, #5471)

## Install

Choose the Framework archive for your platform from the [GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.34.1).

The release tag is [`framework-cpp/v0.34.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.34.1).
