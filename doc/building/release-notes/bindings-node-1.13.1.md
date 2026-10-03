[English](./bindings-node-1.13.1.md) | [한국어](./bindings-node-1.13.1.ko.md)

# ZLink Node.js binding 1.13.1 release notes

Uses Core 1.13.0.

## Changes

- A blocking request waits for the Core request terminal even past the socket RCVTIMEO. It previously stopped waiting at RCVTIMEO and could report a result different from Core (#1164).
