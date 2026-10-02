[English](./bindings-cpp-1.13.0.md) | [한국어](./bindings-cpp-1.13.0.ko.md)

# ZLink C++ binding 1.13.0 release notes

Uses Core 1.13.0.

## Changes

- Includes Core 1.13.0 receive admission, long subscription trie, and physical-disconnect monitoring fixes (#1292, #1334).
- Uses the first errno in the Core REQUEST table as the representative errno for REQUEST completions (#1248).
- Removed single-part receive overloads that consumed REQUEST records and then lost them with `EPROTO` (#1231).

## Retained contracts

These behaviors were included in 1.12.0 and remain unchanged in 1.13.0.

- Provides `stream_socket_t::disconnect_rid` (#1194).
- Provides `request_result_t::backpressured` (113) (#1198).
- WRITABLE completions report Core results; expired waits report `BACKPRESSURED` and `EAGAIN` (#1168).
