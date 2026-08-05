[한국어](posd-module-structure.ko.md) | English

# Core POSD Module Structure

## 1. Goal

Core hides transport, connection, pipe, and protocol complexity behind a narrow
raw-socket C ABI. It does not re-expose application service semantics as Core
helpers or options.

## 2. Layer responsibilities

| Layer | Responsibility |
|---|---|
| Public C API | Arguments, handles, ownership, and result mapping |
| Socket semantics | Per-socket routing, multipart behavior, and request correlation |
| Runtime core | Context, sessions, pipes, mailbox commands, and lifecycle |
| Engine | ZMP/RAW framing and handshake |
| Transport | TCP, WebSocket, IPC, inproc, and TLS I/O |

The public API does not branch on concrete transport or protocol parsers.
Runtime core does not know per-socket policy, and engines do not interpret
application payload meaning.

## 3. Warning signs

- Adding Framework service concepts as public Core types or options
- Turning the API facade into a service-specific pass-through
- Defining one protocol field manually in multiple engines or bindings
- Moving socket semantics into application callbacks or Framework helpers
- Having Framework use private Core headers or symbols

When one appears, re-evaluate the responsibility boundary before adding a helper.
