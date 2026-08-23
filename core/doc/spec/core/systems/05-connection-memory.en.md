[한국어](05-connection-memory.ko.md) | English

<!-- zlink-nav:start -->
[Systems Index](README.en.md) | [Previous: Thread Safety](04-thread-safety.en.md) | [Next: Auto HWM Internals](06-auto-hwm.en.md)
<!-- zlink-nav:end -->

# Per-connection memory

Each transport connection allocates a session, engine state, pipe endpoints,
handshake buffers, and kernel socket buffers. Queued message storage grows with
actual accounted frame bytes and HWM rather than with a single fixed connection
cost.

## Stable components

- session and engine objects;
- pipe metadata and queue chunks;
- routing-id and endpoint metadata;
- protocol handshake state;
- operating-system socket structures.

## Variable components

For every frame, a directional pipe charges the payload plus `sizeof(msg_t)`.
As soon as the decoder knows the frame length, it acquires provisional credit
from the origin queue before allocating the payload buffer. The final multipart
frame converts the same provisional sum into a committed message without
incrementing the counter again. Write failure, rollback, close, and detach
return the charge of each provisional or committed frame actually removed,
exactly once.

An application directional HWM limits physical-queue bytes together with bytes
held by retained-credit leases originating from that queue. Retained receive
changes only the owner from queue to application lease. Releasing the lease
returns read credit to the exact origin generation. If the origin detaches
first, its retired registry entry remains until the final lease returns.

An empty application pipe may admit one complete message larger than the HWM,
subject to the socket's maximum message size, and then blocks later writes.
This exception does not apply to an unfinished multipart. The DEALER/ROUTER
completion progress lane carries only terminal replies and error replies and
applies no byte HWM, LWM, manual HWM, or Core budget reservation. A valid
completion record is admitted while the connection remains available and its
allocation succeeds, even when an application pipe is full.

Monitoring distinguishes current queue bytes, application-lease bytes,
completion bytes, and oversize-admission history. The Core budget is a
normal-state basis for distributing per-pipe HWMs, not a hard cap on actual
context usage. These values explain Core accounting but are not an exact
process resident-memory measurement.

Kernel buffers may grow according to platform autotuning. TLS adds record and
handshake storage. Monitor snapshots report the applied HWM plan but do not
measure all allocator and kernel overhead.

Capacity planning must measure idle, post-traffic residual, and burst peak
memory with the production transport and message-size distribution.
