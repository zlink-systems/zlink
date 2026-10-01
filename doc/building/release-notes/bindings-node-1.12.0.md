[English](./bindings-node-1.12.0.md) | [한국어](./bindings-node-1.12.0.ko.md)

# ZLink Node.js binding 1.12.0 release notes

Uses Core 1.12.0.

## Changes

- STREAM sockets do not provide connect or disconnect and provide `disconnectRid`. A RID that does not exist returns connect error NotFound (605).
- Includes the 1.11.1 fix that classifies `disconnectRid` results correctly (#1188).

## Behavior changes

- The subscription topic receive buffer grows to the length reported by Core, so long topics are no longer truncated or rejected (#1218).
- Aligns representative errnos for REQUEST results with Core: `CONFLICT` returns `EEXIST`, and `INTERNAL_ERROR` returns `EIO` (#1248).
