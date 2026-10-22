[English](./core-1.20.0.md) | [한국어](./core-1.20.0.ko.md)

# libzlink 1.20.0 release notes

## Changes

- Fixed completion poller registration during handshake stopping the temporary transport owner's executor, which could prevent a pair from reaching READY within 30 seconds (#2504, PR #2505). The Core fix affects C++, .NET, Java, and Node.js bindings.
