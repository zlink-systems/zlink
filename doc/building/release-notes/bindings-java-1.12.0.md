[English](./bindings-java-1.12.0.md) | [한국어](./bindings-java-1.12.0.ko.md)

# ZLink Java binding 1.12.0 release notes

Uses Core 1.12.0.

## Changes

- STREAM sockets do not provide connect or disconnect and provide `disconnectRid`. A RID that does not exist returns connect error NotFound (605).

## Behavior changes

- When Core returns `BUSY` during a concurrent receive, Java reports `ZlinkRecvException` instead of treating it as an empty receive (#1200).
- PUB and XPUB publish operations honor the supplied send flags. `NONE` can wait until the send timeout; `DONT_WAIT` returns immediately (#1200).
- The subscription topic receive buffer grows to the length reported by Core (#1218).
- Aligns representative errnos for REQUEST results with Core: `CONFLICT` returns `EEXIST`, and `INTERNAL_ERROR` returns `EIO` (#1218, #1248).
