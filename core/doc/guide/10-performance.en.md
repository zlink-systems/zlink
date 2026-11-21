
<!-- zlink-nav:start -->
[← Monitoring raw sockets](06-monitoring.en.md) | [Core C API →](02-core-api.en.md)
<!-- zlink-nav:end -->

# Core performance

Measure the complete application path before changing socket options. Message
size, connection count, queue depth, transport, and TLS all affect
throughput and latency.

## Backpressure and HWM

Send and receive HWM values limit the queued bytes. Automatic HWM uses the
socket pattern, configured profile, per-message byte input, and connection
count to select bounded queue values. Inspect the applied plan through
`zlink_monitor_status()`.

Use COMPACT when memory is constrained, BALANCED for general workloads, and
THROUGHPUT only when the additional queue memory is acceptable. A send-ready
notification means retrying is worthwhile; it does not guarantee that the
next send succeeds.

## Memory and descriptors

Budget both idle connection cost and queued-message peaks. Set
`RLIMIT_NOFILE` from the expected connection count plus operational headroom.
`ZLINK_MAX_SOCKETS` limits socket handles, not transport connections.

## I/O thread count and placement

A context starts with four I/O threads (`ZLINK_IO_THREADS_DFLT`). Each
connection is served by the one I/O thread chosen when the connection is
created, and connections are spread evenly across the I/O threads.

Request-reply workloads with small messages leave each I/O thread little
work per wakeup even with many connections, so the threads sleep and wake
often. For such workloads, lowering the thread count with
`zlink_ctx_set(ctx, ZLINK_IO_THREADS, n)` or narrowing the I/O threads a
socket's connections use with the `ZLINK_OPT_AFFINITY` socket option (an I/O
thread bitmask) can increase throughput. Stream, large-message, and
bulk-transfer workloads can get slower instead, so measure your own workload
before changing either setting.

Reference measurement (Core multi bench, TCP, 100 client connections, I/O
threads 4 → 2 on both client and server, median of three rounds):

| Pattern | Message size | Throughput change |
|---|---:|---:|
| DEALER_ROUTER request-reply | 64 B | +45% to +57% |
| DEALER_ROUTER request-reply | 1024 B | +23% to +31% |
| STREAM | 64 B · 1024 B | −25% to −29% |
| PUBSUB | 1024 B | −11% to −14% |
| DEALER_ROUTER send-send (C++ multi-connection server) | 4 KB · 64 KB · 128 KB | −17% · −38% · −58% |

## Benchmark procedure

Rebuild `core/build` after changing Core sources. The benchmark runner must
print the exact `libzlink` path and reject a runtime older than the sources.
Record message size, connection count, duration, transport, TLS setting, HWM
profile, CPU allocation, and the reported percentile statistics.
