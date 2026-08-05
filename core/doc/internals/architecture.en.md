[한국어](architecture.ko.md)

# Core runtime architecture

Core is a raw socket runtime. The public C API enters socket implementations,
which exchange commands and messages with I/O-thread objects through mailboxes
and pipes.

```text
+------------------------------------------------------+
| Public C API                                         |
+------------------------------------------------------+
| Socket patterns and eventing                         |
+------------------------------------------------------+
| Context, socket base, sessions, pipes, mailboxes      |
+------------------------------------------------------+
| ZMP or raw engines                                   |
+------------------------------------------------------+
| TCP, IPC, inproc, WebSocket, TLS transports          |
+------------------------------------------------------+
```

## Context and threads

`ctx_t` owns socket slots, I/O threads, the reaper, endpoint registries, and the
generic control runtime used by monitor and timer callbacks. Socket close sends
termination commands and waits for owned objects to release their resources.

## Socket and session path

`socket_base_t` owns public socket state and pattern-specific behavior. A
session represents one transport connection. Pipes carry messages between the
socket thread and session/engine objects while preserving multipart order.

## Engines and transports

Asio engines submit asynchronous reads and writes and receive completion
callbacks. ZMP engines encode zlink message frames; raw engines pass byte-stream
payloads. Transport classes own endpoint parsing, connection establishment, and
operating-system I/O.

## Eventing

Pollers combine socket, file-descriptor, and generic-timer readiness. Socket
monitors observe raw transport and protocol transitions. The control runtime
executes monitor and timer callbacks outside socket I/O threads.

The exact raw-only responsibility boundary is documented in
[Runtime Boundary](runtime-boundary.en.md).
