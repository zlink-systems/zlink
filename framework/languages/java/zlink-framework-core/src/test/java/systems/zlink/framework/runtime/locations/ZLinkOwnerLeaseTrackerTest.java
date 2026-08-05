package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.concurrent.atomic.AtomicLong;
import org.junit.jupiter.api.Test;

class ZLinkOwnerLeaseTrackerTest {
    private static final Instant NOW = Instant.parse("2026-07-03T00:00:00Z");

    @Test
    void ownerLivenessUsesStoreTimePlusMonotonicElapsedTime() throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore(Clock.fixed(NOW, ZoneOffset.UTC));
        ManualTicker ticker = new ManualTicker();
        ZLinkOwnerLeaseTracker tracker = new ZLinkOwnerLeaseTracker(store, Duration.ofSeconds(30), ticker::nanos);
        store.claimOwnerLease("owner-a", Duration.ofSeconds(10))
            .toCompletableFuture().get();

        assertTrue(tracker.isOwnerLive("owner-a").toCompletableFuture().get());

        ticker.advance(Duration.ofSeconds(11));

        assertFalse(tracker.isOwnerLive("owner-a").toCompletableFuture().get());
    }

    @Test
    void newlyStartedOwnerIsVisibleBeforePollingIntervalExpires() throws Exception {
        ZLinkInMemoryLocationStore store = new ZLinkInMemoryLocationStore(Clock.fixed(NOW, ZoneOffset.UTC));
        ManualTicker ticker = new ManualTicker();
        ZLinkOwnerLeaseTracker tracker = new ZLinkOwnerLeaseTracker(
            store,
            Duration.ofSeconds(30),
            ticker::nanos);

        assertFalse(tracker.isOwnerLive("owner-new").toCompletableFuture().get());

        store.claimOwnerLease("owner-new", Duration.ofSeconds(30))
            .toCompletableFuture().get();

        assertTrue(tracker.isOwnerLive("owner-new").toCompletableFuture().get());
    }

    private static final class ManualTicker {
        private final AtomicLong nanos = new AtomicLong();

        long nanos() {
            return nanos.get();
        }

        void advance(Duration duration) {
            nanos.addAndGet(duration.toNanos());
        }
    }
}
