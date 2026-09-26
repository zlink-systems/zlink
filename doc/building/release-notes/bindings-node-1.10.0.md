[English](./bindings-node-1.10.0.md) | [한국어](./bindings-node-1.10.0.ko.md)

# ZLink Node.js binding 1.10.0 release notes

This release updates the npm package to version 1.10.0 and requires Core 1.10.0.

## Changes

- If explicit poller close returns `ZLINK_CLOSE_BUSY`/`EBUSY`, the binding reports the public `Busy` error and keeps the poller valid. The caller can close it after the active `wait()` finishes.
- Context close calls `zlink_ctx_shutdown` followed by `zlink_ctx_term`.
- Poller results follow Core results. Registering `POLLOUT` on a monitor reports `NotSupported`; registering `POLLCOMPLETION` reports `InvalidArgument`.
- The selected ROUTER route can be observed through `routesSnapshot()`, `PollRoute`, `routeGeneration`. A received message's route generation is an opaque token for equality comparison with the snapshot.

## Verification

- The release preflight checks the Node.js binding version, release notes, and npm metadata.

The release tag is `node/v1.10.0`.
