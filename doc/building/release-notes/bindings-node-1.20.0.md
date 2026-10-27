[English](./bindings-node-1.20.0.md) | [한국어](./bindings-node-1.20.0.ko.md)

# ZLink Node.js binding 1.20.0 release notes

Uses Core 1.20.0.

## Changes

- Core 1.20.0: Fixed completion poller registration during handshake stopping the temporary transport owner's executor, which could prevent a pair from reaching READY within 30 seconds (#2504, PR #2505). The Core fix affects C++, .NET, Java, and Node.js bindings.
