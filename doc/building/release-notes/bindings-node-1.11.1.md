[English](./bindings-node-1.11.1.md) | [한국어](./bindings-node-1.11.1.ko.md)

# ZLink Node.js binding 1.11.1 release notes

Uses Core 1.11.0.

## Changes

- `StreamSocket.disconnectRid()` projects Core's connect result into the connect error family. A routing ID that is not present now ends with `ConnectError` (`ConnectResult.NotFound`, 605) instead of `ConfigError`, the same result as the other socket types (#1082).
