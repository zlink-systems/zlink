[한국어](connection-memory.ko.md)

# Per-connection memory

Each transport connection allocates a session, engine state, pipe endpoints,
handshake buffers, and kernel socket buffers. Queued message storage grows with
effective message size and HWM rather than with a single fixed connection cost.

## Stable components

- session and engine objects;
- pipe metadata and queue chunks;
- routing-id and endpoint metadata;
- protocol handshake state;
- operating-system socket structures.

## Variable components

Directional pipes charge each complete message once while writing it. The
charge includes payload and routing-frame bytes and is never smaller than one
`msg_t`. The peer returns that exact charge as byte credit when it releases the
message. This uses fixed integer work in the existing pipe synchronization
path; it adds no allocator query, heap allocation, system call, or new hot-path
lock.

The directional HWM limits this accounted storage. An empty pipe may admit one
complete message larger than the HWM, subject to the socket's maximum message
size, and then blocks later writes. This exception does not apply to an
unfinished multipart. A hidden Completion connection in a paired transport
caps each directional HWM at 262144 bytes and caps each network send and
receive socket buffer at 65536 bytes. The monitor reports bytes in flight and this
oversize-admission history. These values explain Core accounting but are not an
exact process resident-memory measurement.

Kernel buffers may grow according to platform autotuning. TLS adds record and
handshake storage. Monitor snapshots report the applied HWM plan but do not
measure all allocator and kernel overhead.

Capacity planning must measure idle, post-traffic residual, and burst peak
memory with the production transport and message-size distribution.
