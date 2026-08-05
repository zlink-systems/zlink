[한국어](core-source-layout.ko.md)

# Core Source Layout

> **What this chapter answers** — how the raw runtime source that remains in
> Core 11 is split across directories and include direction.

This document explains the responsibility and include direction of the raw
runtime source that remains in Core 11.

## Layer Model

```text
core/
|-- include/zlink/          public raw C ABI
|-- src/api/                validation and C ABI facade
|-- src/runtime/sockets/    socket-type semantics
|-- src/runtime/core/       context, session, pipe and message runtime
|-- src/runtime/engine/     ZMP and RAW engines
|-- src/runtime/transports/ transport integration
|-- src/runtime/eventing/   poller, timer and socket monitor
`-- tests/                  raw contract and integration tests
```

## Dependency Direction

The public API facade calls the socket-semantics layer, and the
socket-semantics layer uses the runtime core's shared connection/pipe
mechanism. The engine and transport don't know a socket type's application
meaning. A lower layer does not include a higher layer's header.

Core source does not hold MeshName, ChannelName, an application mailbox,
Spot, Actor, Location Store, or service-lifecycle state. It also does not
add a separate native seam for Framework's convenience.

## Public Include Rule

The root `zlink.h` includes only the context, message, raw socket,
eventing, and utility domain headers. A public header does not include a
private `src/` header. The installed header and the export manifest must
match the [Core runtime boundary](../spec/core/09-runtime-boundary.en.md).
