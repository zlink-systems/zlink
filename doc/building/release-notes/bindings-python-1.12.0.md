[English](./bindings-python-1.12.0.md) | [한국어](./bindings-python-1.12.0.ko.md)

# ZLink Python binding 1.12.0 release notes

Uses Core 1.12.0. The binding version moves from 0.18.0 to 1.12.0 to match its Core version.

## Changes

- The selected ROUTER route is available in received results (#1087).
- STREAM sockets expose disconnect by RID and do not provide general connect or disconnect. An unknown RID returns NotFound (605) (#1164).
- SEND WRITABLE completions report Core results directly. A wait that times out reports BACKPRESSURED and `EAGAIN` (#1154).

## Behavior changes

- Socket close reports Core's `EBUSY` instead of retrying it. REQUEST submission runs outside the socket lock (#1164).
- The subscription topic receive buffer grows to the length reported by Core for long topics (#1199).
- Representative errnos for REQUEST results match Core: `CONFLICT` is `EEXIST`, and `INTERNAL_ERROR` is `EIO` (#1218, #1248).
