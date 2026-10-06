[English](./bindings-node-1.16.0.md) | [한국어](./bindings-node-1.16.0.ko.md)

# ZLink Node.js binding 1.16.0 release notes

Uses Core 1.16.0.

## Added

- `sharedContext()` provides a process-wide shared Core context. The main thread and `worker_threads` Workers use the same context, allowing Workers to communicate with each other over `inproc://`. The binding manages its lifetime through thread references; `SharedContext` has no `close()` or `shutdown()` method (#1478).

## Changes

- Follows Core 1.16.0: `ZLINK_OPT_BINDTODEVICE` is applied to `tcp`, `tls`, `ws` and `wss` sockets before bind or connect; unsupported platforms reject a non-empty value with `ZLINK_CONFIG_NOT_SUPPORTED` (#1469).
- Follows Core 1.16.0: `zlink_socket()` returns `NULL` with the preserved system errno if socket mailbox signaler creation fails, including at the process fd limit (#1481).
