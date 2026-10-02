[English](./bindings-node-1.13.0.md) | [한국어](./bindings-node-1.13.0.ko.md)

# ZLink Node.js binding 1.13.0 release notes

Uses Core 1.13.0.

## Changes

- Includes Core 1.13.0 receive admission, long subscription trie, and physical-disconnect monitoring fixes (#1292, #1334).
- Uses the first errno in the Core REQUEST table as the representative errno for REQUEST completions (#1248).

## Retained contracts

These behaviors were included in 1.12.0 and remain unchanged in 1.13.0.

- STREAM remains bind-only and provides RID-based disconnect (#1192, #1194).
- WRITABLE completions report Core results; expired waits report `BACKPRESSURED` and `EAGAIN` (#1168).
