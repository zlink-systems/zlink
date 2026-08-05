package systems.zlink.framework.runtime.streams;

import java.util.concurrent.atomic.AtomicLong;

// Process-global monotonic correlation id for framework-originated outbound stream
// packets (hex of a per-process counter). Mirrors the connector generator and the
// C++/.NET versions: unique within a process, propagated to correlate nodes (client
// request -> server echo). Server replies echo the request id instead.
final class ZLinkStreamCorrelation {
    private static final AtomicLong COUNTER = new AtomicLong();

    private ZLinkStreamCorrelation() {
    }

    static String next() {
        return Long.toHexString(COUNTER.incrementAndGet());
    }
}
