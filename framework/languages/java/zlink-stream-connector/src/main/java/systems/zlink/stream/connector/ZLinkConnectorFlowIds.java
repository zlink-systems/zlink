package systems.zlink.stream.connector;

import java.security.SecureRandom;
import java.time.Instant;
import java.util.UUID;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkConnectorFlowIds {
    private static final SecureRandom RANDOM = new SecureRandom();
    private static final AtomicLong LAST_MILLIS = new AtomicLong();

    private ZLinkConnectorFlowIds() {
    }

    static String next() {
        long now = Instant.now().toEpochMilli();
        long timestamp = LAST_MILLIS.updateAndGet(previous -> Math.max(now, previous));
        long msb = ((timestamp & 0xffffffffffffL) << 16)
            | 0x7000L | (RANDOM.nextLong() & 0x0fffL);
        long lsb = (RANDOM.nextLong() & 0x3fffffffffffffffL) | 0x8000000000000000L;
        return new UUID(msb, lsb).toString();
    }
}
