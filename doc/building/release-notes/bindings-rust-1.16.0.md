[English](./bindings-rust-1.16.0.md) | [한국어](./bindings-rust-1.16.0.ko.md)

# ZLink Rust binding 1.16.0 release notes

Uses Core 1.16.0.

## Changes

- Core 1.16.0 applies `ZLINK_OPT_BINDTODEVICE` to `tcp`, `tls`, `ws` and `wss` sockets before bind or connect; unsupported platforms reject a non-empty value with `ZLINK_CONFIG_NOT_SUPPORTED` (#1469).
- Core 1.16.0 returns `NULL` and preserves the system errno from `zlink_socket()` when socket mailbox signaler creation fails, including at the process fd limit (#1481).
- Core Auto HWM no longer accepts a runtime memory hint. The binding snapshot omits that hint; Core detects parent cgroup limits and admits connections against the resolved memory limit, independently of a manual HWM budget. Monitor ready counts now follow disconnects, and monitor close during a poller wait reports socket close (#1476, #1083).
