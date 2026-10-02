package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseFound;

import java.lang.reflect.Proxy;
import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

class ZLinkOwnerLeaseTrackerTest {
    private static final Instant NOW = Instant.parse("2026-07-03T00:00:00Z");

    @Test
    void ownerIdentityIsCheckedForFreshCachedAndWildcardGenerationReads() throws Exception {
        AtomicInteger reads = new AtomicInteger();
        ZLinkLocationRepository store =
                (ZLinkLocationRepository)
                        Proxy.newProxyInstance(
                                ZLinkOwnerLeaseTrackerTest.class.getClassLoader(),
                                new Class<?>[] {ZLinkLocationRepository.class},
                                (proxy, method, arguments) -> {
                                    if (!method.getName().equals("readOwnerLease"))
                                        throw new UnsupportedOperationException(method.getName());
                                    reads.incrementAndGet();
                                    return CompletableFuture.completedFuture(
                                            new ZLinkOwnerLeaseFound(
                                                    new ZLinkLocationOwnerToken("actual-owner", 5),
                                                    NOW.plusSeconds(10),
                                                    NOW));
                                });
        ManualTicker ticker = new ManualTicker();
        ZLinkOwnerLeaseTracker tracker =
                new ZLinkOwnerLeaseTracker(store, Duration.ofSeconds(30), ticker::nanos);
        assertNull(
                tracker.remainingAdmissionLifetime("requested-owner", 5)
                        .toCompletableFuture()
                        .get());
        assertNull(
                tracker.remainingAdmissionLifetime("requested-owner", 5)
                        .toCompletableFuture()
                        .get());
        assertFalse(tracker.isOwnerLive("requested-owner").toCompletableFuture().get());
        assertEquals(1, reads.get());
        assertTrue(tracker.isOwnerLive("actual-owner").toCompletableFuture().get());
        assertTrue(
                tracker.remainingAdmissionLifetime("actual-owner", 5)
                                .toCompletableFuture()
                                .get()
                                .compareTo(Duration.ZERO)
                        > 0);
        assertEquals(2, reads.get());
    }

    @Test
    void ownerLivenessUsesStoreTimePlusMonotonicElapsedTime() throws Exception {
        ZLinkInMemoryLocationStore store =
                new ZLinkInMemoryLocationStore(Clock.fixed(NOW, ZoneOffset.UTC));
        ManualTicker ticker = new ManualTicker();
        ZLinkOwnerLeaseTracker tracker =
                new ZLinkOwnerLeaseTracker(store, Duration.ofSeconds(30), ticker::nanos);
        store.claimOwnerLease("owner-a", Duration.ofSeconds(10)).toCompletableFuture().get();

        assertTrue(tracker.isOwnerLive("owner-a").toCompletableFuture().get());

        ticker.advance(Duration.ofSeconds(11));

        assertFalse(tracker.isOwnerLive("owner-a").toCompletableFuture().get());
    }

    @Test
    void newlyStartedOwnerIsVisibleBeforePollingIntervalExpires() throws Exception {
        ZLinkInMemoryLocationStore store =
                new ZLinkInMemoryLocationStore(Clock.fixed(NOW, ZoneOffset.UTC));
        ManualTicker ticker = new ManualTicker();
        ZLinkOwnerLeaseTracker tracker =
                new ZLinkOwnerLeaseTracker(store, Duration.ofSeconds(30), ticker::nanos);

        assertFalse(tracker.isOwnerLive("owner-new").toCompletableFuture().get());

        store.claimOwnerLease("owner-new", Duration.ofSeconds(30)).toCompletableFuture().get();

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
