[English](./bindings-node-1.14.0.md) | [한국어](./bindings-node-1.14.0.ko.md)

# ZLink Node.js binding 1.14.0 release notes

Uses Core 1.14.0.

## Changes

- Includes the Core 1.14.0 connection termination, terminal ERROR write ordering, handshake timeout, and session lifetime fixes (#1434).
- A blocking REQUEST continues waiting for the Core terminal completion when `RCVTIMEO` causes a completion receive to return `NO_DATA`. This fix was included in 1.13.1.
