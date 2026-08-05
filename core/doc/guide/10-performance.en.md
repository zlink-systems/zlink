[한국어](10-performance.ko.md)

# Core performance

Measure the complete application path before changing socket options. Message
size, connection count, queue depth, transport, TLS, and callback work all
affect throughput and latency.

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

## Benchmark procedure

Rebuild `core/build` after changing Core sources. The benchmark runner must
print the exact `libzlink` path and reject a runtime older than the sources.
Record message size, connection count, duration, transport, TLS setting, HWM
profile, CPU allocation, and the reported percentile statistics.
