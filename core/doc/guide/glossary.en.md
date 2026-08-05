[한국어](glossary.ko.md)

# Core Glossary

> **What this chapter answers** — it gathers the core terms used throughout
> this guide in one place. The exact contract for each term is owned by the
> matching spec chapter.

- **Context** — the top-level handle that manages the lifetime of I/O threads and socket resources.
- **Socket pattern** — defines peer selection, the send/receive direction, and message-distribution rules.
- **Routing ID** — the byte sequence that identifies a connected peer on ROUTER and STREAM.
- **Multipart message** — one or more parts that make up a single logical message.
- **HWM** — a value that applies backpressure by limiting the bytes a queue may hold.
- **Backpressure** — the behavior of limiting a sender's further submits when a downstream can't keep up.
- **Poller** — a handle that waits together on the readiness of sockets, file descriptors, and generic timers.
- **Socket monitor** — a separate handle that reports a raw socket's transport and protocol events.
- **ZMP** — the wire protocol that carries the handshake and message frames between zlink sockets.
- **STREAM** — the raw socket pattern for communicating with an external byte-stream peer.
