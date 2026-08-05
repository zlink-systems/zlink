package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.locationprovider.*;

final class ZLinkInMemoryProviderLocationStoreTest {
    private static final ZLinkStoreCancellation ACTIVE = () -> false;

    @Test
    void conditionalBatchIsAtomicAndScanKeepsItsSnapshot() throws Exception {
        var store = new ZLinkInMemoryProviderLocationStore();
        var first = new ZLinkStoreKey("a/1");
        var second = new ZLinkStoreKey("a/2");
        var applied = assertInstanceOf(
            ZLinkStoreWriteApplied.class,
            store.write(
                new ZLinkStoreWriteRequest(
                    List.of(new ZLinkStoreMissingCondition(first)),
                    List.of(
                        new ZLinkStorePut(first, new byte[] {1}, null),
                        new ZLinkStorePut(second, new byte[] {2}, null))),
                ACTIVE).toCompletableFuture().get());

        var page = ((ZLinkStoreScanPageResult) store.scan(
            new ZLinkStoreScanRequest("a/", null, 1),
            ACTIVE).toCompletableFuture().get()).value();
        assertEquals(List.of(first), page.items().stream()
            .map(ZLinkStoreScanItem::key).toList());

        assertInstanceOf(
            ZLinkStoreWriteConflict.class,
            store.write(
                new ZLinkStoreWriteRequest(
                    List.of(new ZLinkStoreVersionCondition(
                        first, new ZLinkStoreVersion("wrong"))),
                    List.of(new ZLinkStoreDelete(second))),
                ACTIVE).toCompletableFuture().get());
        assertInstanceOf(
            ZLinkStoreReadFound.class,
            store.read(second, ACTIVE).toCompletableFuture().get());

        store.write(
            new ZLinkStoreWriteRequest(
                List.of(),
                List.of(new ZLinkStoreDelete(second))),
            ACTIVE).toCompletableFuture().get();
        var snapshotTail = ((ZLinkStoreScanPageResult) store.scan(
            new ZLinkStoreScanRequest("a/", page.nextCursor(), 1),
            ACTIVE).toCompletableFuture().get()).value();
        assertEquals(List.of(second), snapshotTail.items().stream()
            .map(ZLinkStoreScanItem::key).toList());
        assertEquals(2, applied.putVersions().size());
    }

    @Test
    void scanSnapshotExpiresAndReleasesItsActiveSlot() throws Exception {
        var clock = new MutableClock(Instant.parse("2026-07-29T00:00:00Z"));
        var store = new ZLinkInMemoryProviderLocationStore(clock);
        store.write(
            new ZLinkStoreWriteRequest(
                List.of(),
                List.of(
                    new ZLinkStorePut(
                        new ZLinkStoreKey("scan/1"),
                        new byte[] {1},
                        null),
                    new ZLinkStorePut(
                        new ZLinkStoreKey("scan/2"),
                        new byte[] {2},
                        null))),
            ACTIVE).toCompletableFuture().get();

        var first = ((ZLinkStoreScanPageResult) store.scan(
            new ZLinkStoreScanRequest("scan/", null, 1),
            ACTIVE).toCompletableFuture().get()).value();
        clock.advance(Duration.ofMinutes(1));

        assertInstanceOf(
            ZLinkStoreScanExpired.class,
            store.scan(
                new ZLinkStoreScanRequest(
                    "scan/",
                    first.nextCursor(),
                    1),
                ACTIVE).toCompletableFuture().get());

        // Starting a new scan also performs bounded cleanup.
        assertInstanceOf(
            ZLinkStoreScanPageResult.class,
            store.scan(
                new ZLinkStoreScanRequest("scan/", null, 1),
                ACTIVE).toCompletableFuture().get());
    }

    @Test
    void activeScanCountIsBounded() throws Exception {
        var store = new ZLinkInMemoryProviderLocationStore();
        store.write(
            new ZLinkStoreWriteRequest(
                List.of(),
                List.of(
                    new ZLinkStorePut(
                        new ZLinkStoreKey("bound/1"),
                        new byte[] {1},
                        null),
                    new ZLinkStorePut(
                        new ZLinkStoreKey("bound/2"),
                        new byte[] {2},
                        null))),
            ACTIVE).toCompletableFuture().get();

        for (int index = 0; index < 4096; index++) {
            var page = assertInstanceOf(
                ZLinkStoreScanPageResult.class,
                store.scan(
                    new ZLinkStoreScanRequest("bound/", null, 1),
                    ACTIVE).toCompletableFuture().get());
            assertInstanceOf(
                ZLinkStoreScanCursor.class,
                page.value().nextCursor());
        }

        assertThrows(
            IllegalStateException.class,
            () -> store.scan(
                new ZLinkStoreScanRequest("bound/", null, 1),
                ACTIVE));
    }

    private static final class MutableClock extends Clock {
        private Instant now;

        private MutableClock(Instant now) {
            this.now = now;
        }

        private void advance(Duration duration) {
            now = now.plus(duration);
        }

        @Override
        public ZoneId getZone() {
            return ZoneOffset.UTC;
        }

        @Override
        public Clock withZone(ZoneId zone) {
            return this;
        }

        @Override
        public Instant instant() {
            return now;
        }
    }
}
