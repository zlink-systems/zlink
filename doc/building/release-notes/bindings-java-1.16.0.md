[English](./bindings-java-1.16.0.md) | [한국어](./bindings-java-1.16.0.ko.md)

# ZLink Java binding 1.16.0 release notes

Uses Core 1.16.0.

## Changes

- Core 1.16.0 applies `ZLINK_OPT_BINDTODEVICE` to `tcp`, `tls`, `ws` and `wss` sockets before bind or connect; unsupported platforms reject a non-empty value with `ZLINK_CONFIG_NOT_SUPPORTED` (#1469).
- Core 1.16.0 returns `NULL` and preserves the system errno from `zlink_socket()` when socket mailbox signaler creation fails, including at the process fd limit (#1481).
