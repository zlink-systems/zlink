[English](./bindings-rust-1.26.0.md) | [한국어](./bindings-rust-1.26.0.ko.md)

# ZLink Rust binding 1.26.0 release notes

Uses Core 1.26.0.

## Changes

- No binding-only changes; this release follows Core 1.26.0.

## Core 1.26.0 included

- `zlink_disconnect(endpoint)` ends only connect registrations and auto-reconnect intents; `zlink_unbind(endpoint)` ends only the bind registration and its accepted connections. A disconnect no longer closes a listener bound on the same address (#4846, #4864).
- A connecter uses the I/O thread of its own session (#3541, #4303).
