package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.ZLinkLocationStore;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreMissingCondition;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreReadMissing;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreValueCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CancellationException;

class ZLinkRedisOpaqueLocationStoreTest {
    @Test
    void valueConditionChecksBytesAndExpiryAtomically() throws Exception {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
                endpoint != null && !endpoint.isBlank(),
                "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(endpoint)
                                .setKeyPrefix("value:" + UUID.randomUUID()))) {
            var key = new ZLinkStoreKey("lease");
            var marker = new ZLinkStoreKey("marker");
            byte[] bytes = {0, (byte) 0xff};
            var put = new ZLinkStorePut(key, bytes, Duration.ofMinutes(1));
            var first =
                    assertInstanceOf(
                            ZLinkStoreWriteApplied.class,
                            store.write(
                                            new ZLinkStoreWriteRequest(List.of(), List.of(put)),
                                            () -> false)
                                    .toCompletableFuture()
                                    .get());
            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(
                                            new ZLinkStoreVersionCondition(
                                                    key, first.putVersions().get(key))),
                                    List.of(put)),
                            () -> false)
                    .toCompletableFuture()
                    .get();
            var condition = new ZLinkStoreValueCondition(key, bytes);
            var markerWrite =
                    assertInstanceOf(
                            ZLinkStoreWriteApplied.class,
                            store.write(
                                            new ZLinkStoreWriteRequest(
                                                    List.of(condition),
                                                    List.of(
                                                            new ZLinkStorePut(
                                                                    marker, new byte[] {1}, null))),
                                            () -> false)
                                    .toCompletableFuture()
                                    .get());
            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(new ZLinkStorePut(key, new byte[] {2}, null))),
                            () -> false)
                    .toCompletableFuture()
                    .get();
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(condition),
                                            List.of(new ZLinkStoreDelete(marker))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(), List.of(new ZLinkStoreDelete(key))),
                            () -> false)
                    .toCompletableFuture()
                    .get();
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(condition),
                                            List.of(new ZLinkStoreDelete(marker))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            assertInstanceOf(
                    ZLinkStoreReadFound.class,
                    store.read(marker, () -> false).toCompletableFuture().get());
            assertEquals(
                    markerWrite.putVersions().get(marker),
                    ((ZLinkStoreReadFound)
                                    store.read(marker, () -> false).toCompletableFuture().get())
                            .value()
                            .version());

            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(),
                                    List.of(new ZLinkStorePut(key, bytes, Duration.ofMillis(1)))),
                            () -> false)
                    .toCompletableFuture()
                    .get();
            boolean expired = false;
            for (int attempt = 0; attempt < 1000; attempt++) {
                if (store.read(key, () -> false).toCompletableFuture().get()
                        instanceof ZLinkStoreReadMissing) {
                    expired = true;
                    break;
                }
            }
            assertTrue(expired);
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(condition),
                                            List.of(new ZLinkStoreDelete(marker))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            assertInstanceOf(
                    ZLinkStoreReadFound.class,
                    store.read(marker, () -> false).toCompletableFuture().get());
        }
    }

    @Test
    void publicStoreImplementsOpaqueProviderContract() {
        assertTrue(ZLinkLocationStore.class.isAssignableFrom(ZLinkRedisLocationStore.class));
    }

    @Test
    void validatesProviderBoundsBeforeConnecting() {
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString("redis://127.0.0.1:1")
                                .setKeyPrefix("validation:" + UUID.randomUUID()))) {
            assertThrows(
                    IllegalArgumentException.class,
                    () -> store.read(new ZLinkStoreKey(""), () -> false));
            assertThrows(
                    IllegalArgumentException.class,
                    () ->
                            store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(),
                                            List.of(
                                                    new ZLinkStorePut(
                                                            new ZLinkStoreKey("large"),
                                                            new byte[1024 * 1024 + 1],
                                                            null))),
                                    () -> false));
            assertThrows(
                    CancellationException.class,
                    () ->
                            store.read(new ZLinkStoreKey("cancelled"), () -> true)
                                    .toCompletableFuture()
                                    .join());
        }
    }

    @Test
    void conditionalBatchAndScanUseStableOpaqueSnapshot() throws Exception {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
                endpoint != null && !endpoint.isBlank(),
                "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(endpoint)
                                .setKeyPrefix("opaque:" + UUID.randomUUID()))) {
            var keyA = new ZLinkStoreKey("descriptor/a");
            var keyB = new ZLinkStoreKey("descriptor/b");
            var initial =
                    assertInstanceOf(
                            ZLinkStoreWriteApplied.class,
                            store.write(
                                            new ZLinkStoreWriteRequest(
                                                    List.of(
                                                            new ZLinkStoreMissingCondition(keyA),
                                                            new ZLinkStoreMissingCondition(keyB)),
                                                    List.of(
                                                            new ZLinkStorePut(
                                                                    keyA,
                                                                    new byte[] {1},
                                                                    Duration.ofMinutes(5)),
                                                            new ZLinkStorePut(
                                                                    keyB, new byte[] {2}, null))),
                                            () -> false)
                                    .toCompletableFuture()
                                    .get());

            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(new ZLinkStoreMissingCondition(keyA)),
                                            List.of(new ZLinkStoreDelete(keyB))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());
            assertArrayEquals(
                    new byte[] {2},
                    assertInstanceOf(
                                    ZLinkStoreReadFound.class,
                                    store.read(keyB, () -> false).toCompletableFuture().get())
                            .value()
                            .bytes());

            var first =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(
                                                    new ZLinkStoreScanRequest(
                                                            "descriptor/", null, 1),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();
            assertArrayEquals(new byte[] {1}, first.items().getFirst().value().bytes());

            store.write(
                            new ZLinkStoreWriteRequest(
                                    List.of(
                                            new ZLinkStoreVersionCondition(
                                                    keyB, initial.putVersions().get(keyB))),
                                    List.of(new ZLinkStorePut(keyB, new byte[] {9}, null))),
                            () -> false)
                    .toCompletableFuture()
                    .get();

            var second =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    store.scan(
                                                    new ZLinkStoreScanRequest(
                                                            "descriptor/", first.nextCursor(), 1),
                                                    () -> false)
                                            .toCompletableFuture()
                                            .get())
                            .value();
            assertArrayEquals(new byte[] {2}, second.items().getFirst().value().bytes());
            assertArrayEquals(
                    new byte[] {9},
                    assertInstanceOf(
                                    ZLinkStoreReadFound.class,
                                    store.read(keyB, () -> false).toCompletableFuture().get())
                            .value()
                            .bytes());
        }
    }

    @Test
    void multiKeyCasKeepsEachConditionBoundToItsKey() throws Exception {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
                endpoint != null && !endpoint.isBlank(),
                "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(endpoint)
                                .setKeyPrefix("condition-order:" + UUID.randomUUID()))) {
            // These names intentionally iterate in the opposite order in a
            // HashSet. The provider must preserve request order because the
            // Lua script binds condition i to KEYS[i].
            var existingKey = new ZLinkStoreKey("z");
            var missingKey = new ZLinkStoreKey("a");
            var initial =
                    assertInstanceOf(
                            ZLinkStoreWriteApplied.class,
                            store.write(
                                            new ZLinkStoreWriteRequest(
                                                    List.of(
                                                            new ZLinkStoreMissingCondition(
                                                                    existingKey)),
                                                    List.of(
                                                            new ZLinkStorePut(
                                                                    existingKey,
                                                                    new byte[] {1},
                                                                    null))),
                                            () -> false)
                                    .toCompletableFuture()
                                    .get());

            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(
                                                    new ZLinkStoreVersionCondition(
                                                            existingKey,
                                                            initial.putVersions().get(existingKey)),
                                                    new ZLinkStoreMissingCondition(missingKey)),
                                            List.of(
                                                    new ZLinkStorePut(
                                                            missingKey, new byte[] {2}, null))),
                                    () -> false)
                            .toCompletableFuture()
                            .get());

            assertArrayEquals(
                    new byte[] {2},
                    assertInstanceOf(
                                    ZLinkStoreReadFound.class,
                                    store.read(missingKey, () -> false).toCompletableFuture().get())
                            .value()
                            .bytes());
        }
    }
}
