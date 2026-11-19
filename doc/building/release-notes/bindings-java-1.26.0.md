[English](./bindings-java-1.26.0.md) | [한국어](./bindings-java-1.26.0.ko.md)

# ZLink Java binding 1.26.0 release notes

Uses Core 1.26.0.

## Changes

- A receive that copies a borrowed routing id holds the existing native-call reservation so it does not overlap close (#4568, PR #4597).
- Message copy reuses the message slot pool header (#4570, PR #4588).

## Core 1.26.0 included

- `zlink_disconnect(endpoint)` ends only connect registrations and auto-reconnect intents; `zlink_unbind(endpoint)` ends only the bind registration and its accepted connections. A disconnect no longer closes a listener bound on the same address (#4846, #4864).
- A connecter uses the I/O thread of its own session (#3541, #4303).
