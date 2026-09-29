package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.*;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneId;
import java.time.ZoneOffset;
import java.util.List;

final class ZLinkInMemoryProviderLocationStoreTest {
    private static final ZLinkStoreCancellation ACTIVE = () -> false;

    @Test
    void valueConditionUsesCurrentBytesAndExpiryWithoutMutatingOnConflict() throws Exception {
        var clock = new MutableClock(Instant.parse("2026-07-29T00:00:00Z"));
        var store = new ZLinkInMemoryProviderLocationStore(clock);
        var lease = new ZLinkStoreKey("lease");
        var marker = new ZLinkStoreKey("marker");
        byte[] expected = {0, (byte) 0xff};
        var put = new ZLinkStorePut(lease, expected, Duration.ofMinutes(1));
        var first =
                assertInstanceOf(
                        ZLinkStoreWriteApplied.class,
                        store.write(new ZLinkStoreWriteRequest(List.of(), List.of(put)), ACTIVE)
                                .toCompletableFuture()
                                .get());
        var second =
                assertInstanceOf(
                        ZLinkStoreWriteApplied.class,
                        store.write(
                                        new ZLinkStoreWriteRequest(
                                                List.of(
                                                        new ZLinkStoreVersionCondition(
                                                                lease,
                                                                first.putVersions().get(lease))),
                                                List.of(put)),
                                        ACTIVE)
                                .toCompletableFuture()
                                .get());
        var condition = new ZLinkStoreValueCondition(lease, expected);
        var markerWrite =
                assertInstanceOf(
                        ZLinkStoreWriteApplied.class,
                        store.write(
                                        new ZLinkStoreWriteRequest(
                                                List.of(condition),
                                                List.of(
                                                        new ZLinkStorePut(
                                                                marker, new byte[] {1}, null))),
                                        ACTIVE)
                                .toCompletableFuture()
                                .get());
        assertEquals(
                second.putVersions().get(lease),
                ((ZLinkStoreReadFound) store.read(lease, ACTIVE).toCompletableFuture().get())
                        .value()
                        .version());
        for (byte[] replacement : List.of(new byte[] {2}, new byte[] {3})) {
            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(
                                            new ZLinkStorePut(
                                                    lease, replacement, Duration.ofMinutes(1)))),
                            ACTIVE)
                    .toCompletableFuture()
                    .get();
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(condition),
                                            List.of(new ZLinkStoreDelete(marker))),
                                    ACTIVE)
                            .toCompletableFuture()
                            .get());
            assertInstanceOf(
                    ZLinkStoreReadFound.class,
                    store.read(marker, ACTIVE).toCompletableFuture().get());
        }
        store.write(
                        new ZLinkStoreWriteRequest(List.of(), List.of(new ZLinkStoreDelete(lease))),
                        ACTIVE)
                .toCompletableFuture()
                .get();
        assertInstanceOf(
                ZLinkStoreWriteConflict.class,
                store.write(
                                new ZLinkStoreWriteRequest(
                                        List.of(condition), List.of(new ZLinkStoreDelete(marker))),
                                ACTIVE)
                        .toCompletableFuture()
                        .get());
        store.write(new ZLinkStoreWriteRequest(List.of(), List.of(put)), ACTIVE)
                .toCompletableFuture()
                .get();
        clock.advance(Duration.ofMinutes(1));
        assertInstanceOf(
                ZLinkStoreWriteConflict.class,
                store.write(
                                new ZLinkStoreWriteRequest(
                                        List.of(condition), List.of(new ZLinkStoreDelete(marker))),
                                ACTIVE)
                        .toCompletableFuture()
                        .get());
        assertInstanceOf(
                ZLinkStoreReadFound.class, store.read(marker, ACTIVE).toCompletableFuture().get());
        assertEquals(
                markerWrite.putVersions().get(marker),
                ((ZLinkStoreReadFound) store.read(marker, ACTIVE).toCompletableFuture().get())
                        .value()
                        .version());
    }

    @Test
    void roundsFractionalMillisecondRetentionUp() throws Exception {
        Instant now = Instant.parse("2026-07-29T00:00:00Z");
        var store = new ZLinkInMemoryProviderLocationStore(Clock.fixed(now, ZoneOffset.UTC));
        var key = new ZLinkStoreKey("fractional");
        store.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(
                                        new ZLinkStorePut(
                                                key, new byte[] {1}, Duration.ofNanos(1_500_000)))),
                        ACTIVE)
                .toCompletableFuture()
                .get();
        var found =
                assertInstanceOf(
                        ZLinkStoreReadFound.class,
                        store.read(key, ACTIVE).toCompletableFuture().get());
        assertEquals(now.plusMillis(2), found.value().expiresAt());
    }

    @Test
    void conditionalBatchIsAtomicAndScanKeepsItsSnapshot() throws Exception {
        var store = new ZLinkInMemoryProviderLocationStore();
        var first = new ZLinkStoreKey("a/1");
        var second = new ZLinkStoreKey("a/2");
        var applied =
                assertInstanceOf(
                        ZLinkStoreWriteApplied.class,
                        store.write(
                                        new ZLinkStoreWriteRequest(
                                                List.of(new ZLinkStoreMissingCondition(first)),
                                                List.of(
                                                        new ZLinkStorePut(
                                                                first, new byte[] {1}, null),
                                                        new ZLinkStorePut(
                                                                second, new byte[] {2}, null))),
                                        ACTIVE)
                                .toCompletableFuture()
                                .get());

        var page =
                ((ZLinkStoreScanPageResult)
                                store.scan(new ZLinkStoreScanRequest("a/", null, 1), ACTIVE)
                                        .toCompletableFuture()
                                        .get())
                        .value();
        assertEquals(List.of(first), page.items().stream().map(ZLinkStoreScanItem::key).toList());

        assertInstanceOf(
                ZLinkStoreWriteConflict.class,
                store.write(
                                new ZLinkStoreWriteRequest(
                                        List.of(
                                                new ZLinkStoreVersionCondition(
                                                        first, new ZLinkStoreVersion("wrong"))),
                                        List.of(new ZLinkStoreDelete(second))),
                                ACTIVE)
                        .toCompletableFuture()
                        .get());
        assertInstanceOf(
                ZLinkStoreReadFound.class, store.read(second, ACTIVE).toCompletableFuture().get());

        store.write(
                        new ZLinkStoreWriteRequest(
                                List.of(), List.of(new ZLinkStoreDelete(second))),
                        ACTIVE)
                .toCompletableFuture()
                .get();
        var snapshotTail =
                ((ZLinkStoreScanPageResult)
                                store.scan(
                                                new ZLinkStoreScanRequest(
                                                        "a/", page.nextCursor(), 1),
                                                ACTIVE)
                                        .toCompletableFuture()
                                        .get())
                        .value();
        assertEquals(
                List.of(second),
                snapshotTail.items().stream().map(ZLinkStoreScanItem::key).toList());
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
                                                new ZLinkStoreKey("scan/1"), new byte[] {1}, null),
                                        new ZLinkStorePut(
                                                new ZLinkStoreKey("scan/2"),
                                                new byte[] {2},
                                                null))),
                        ACTIVE)
                .toCompletableFuture()
                .get();

        var first =
                ((ZLinkStoreScanPageResult)
                                store.scan(new ZLinkStoreScanRequest("scan/", null, 1), ACTIVE)
                                        .toCompletableFuture()
                                        .get())
                        .value();
        clock.advance(Duration.ofMinutes(1));

        assertInstanceOf(
                ZLinkStoreScanExpired.class,
                store.scan(new ZLinkStoreScanRequest("scan/", first.nextCursor(), 1), ACTIVE)
                        .toCompletableFuture()
                        .get());

        // Starting a new scan also performs bounded cleanup.
        assertInstanceOf(
                ZLinkStoreScanPageResult.class,
                store.scan(new ZLinkStoreScanRequest("scan/", null, 1), ACTIVE)
                        .toCompletableFuture()
                        .get());
    }

    @Test
    void activeScanCountIsBounded() throws Exception {
        var store = new ZLinkInMemoryProviderLocationStore();
        store.write(
                        new ZLinkStoreWriteRequest(
                                List.of(),
                                List.of(
                                        new ZLinkStorePut(
                                                new ZLinkStoreKey("bound/1"), new byte[] {1}, null),
                                        new ZLinkStorePut(
                                                new ZLinkStoreKey("bound/2"),
                                                new byte[] {2},
                                                null))),
                        ACTIVE)
                .toCompletableFuture()
                .get();

        for (int index = 0; index < 4096; index++) {
            var page =
                    assertInstanceOf(
                            ZLinkStoreScanPageResult.class,
                            store.scan(new ZLinkStoreScanRequest("bound/", null, 1), ACTIVE)
                                    .toCompletableFuture()
                                    .get());
            assertInstanceOf(ZLinkStoreScanCursor.class, page.value().nextCursor());
        }

        assertThrows(
                IllegalStateException.class,
                () -> store.scan(new ZLinkStoreScanRequest("bound/", null, 1), ACTIVE));
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
