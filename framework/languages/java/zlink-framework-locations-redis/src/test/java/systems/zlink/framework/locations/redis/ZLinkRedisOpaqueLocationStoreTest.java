package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.time.Duration;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.locationprovider.ZLinkStoreDelete;
import systems.zlink.framework.locationprovider.ZLinkStoreKey;
import systems.zlink.framework.locationprovider.ZLinkStoreMissingCondition;
import systems.zlink.framework.locationprovider.ZLinkStorePut;
import systems.zlink.framework.locationprovider.ZLinkStoreReadFound;
import systems.zlink.framework.locationprovider.ZLinkStoreScanPageResult;
import systems.zlink.framework.locationprovider.ZLinkStoreScanRequest;
import systems.zlink.framework.locationprovider.ZLinkStoreVersionCondition;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteApplied;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteConflict;
import systems.zlink.framework.locationprovider.ZLinkStoreWriteRequest;

class ZLinkRedisOpaqueLocationStoreTest {
    @Test
    void publicStoreImplementsOpaqueProviderContract() {
        assertTrue(
            systems.zlink.framework.locationprovider.ZLinkLocationStore.class
                .isAssignableFrom(ZLinkRedisLocationStore.class));
    }

    @Test
    void validatesProviderBoundsBeforeConnecting() {
        try (var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions()
                .setConnectionString("redis://127.0.0.1:1")
                .setKeyPrefix("validation:" + UUID.randomUUID()))) {
            assertThrows(
                IllegalArgumentException.class,
                () -> store.read(
                    new ZLinkStoreKey(""),
                    () -> false));
            assertThrows(
                IllegalArgumentException.class,
                () -> store.write(
                    new ZLinkStoreWriteRequest(
                        List.of(),
                        List.of(new ZLinkStorePut(
                            new ZLinkStoreKey("large"),
                            new byte[1024 * 1024 + 1],
                            null))),
                    () -> false));
            assertThrows(
                CancellationException.class,
                () -> store.read(
                    new ZLinkStoreKey("cancelled"),
                    () -> true).toCompletableFuture().join());
        }
    }

    @Test
    void conditionalBatchAndScanUseStableOpaqueSnapshot() throws Exception {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
            endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions()
                .setConnectionString(endpoint)
                .setKeyPrefix("opaque:" + UUID.randomUUID()))) {
            var keyA = new ZLinkStoreKey("descriptor/a");
            var keyB = new ZLinkStoreKey("descriptor/b");
            var initial = assertInstanceOf(
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
                                keyB,
                                new byte[] {2},
                                null))),
                    () -> false).toCompletableFuture().get());

            assertInstanceOf(
                ZLinkStoreWriteConflict.class,
                store.write(
                    new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreMissingCondition(keyA)),
                        List.of(new ZLinkStoreDelete(keyB))),
                    () -> false).toCompletableFuture().get());
            assertArrayEquals(
                new byte[] {2},
                assertInstanceOf(
                    ZLinkStoreReadFound.class,
                    store.read(keyB, () -> false)
                        .toCompletableFuture().get())
                    .value().bytes());

            var first = assertInstanceOf(
                ZLinkStoreScanPageResult.class,
                store.scan(
                    new ZLinkStoreScanRequest("descriptor/", null, 1),
                    () -> false).toCompletableFuture().get()).value();
            assertArrayEquals(
                new byte[] {1},
                first.items().getFirst().value().bytes());

            store.write(
                new ZLinkStoreWriteRequest(
                    List.of(new ZLinkStoreVersionCondition(
                        keyB,
                        initial.putVersions().get(keyB))),
                    List.of(new ZLinkStorePut(
                        keyB,
                        new byte[] {9},
                        null))),
                () -> false).toCompletableFuture().get();

            var second = assertInstanceOf(
                ZLinkStoreScanPageResult.class,
                store.scan(
                    new ZLinkStoreScanRequest(
                        "descriptor/",
                        first.nextCursor(),
                        1),
                    () -> false).toCompletableFuture().get()).value();
            assertArrayEquals(
                new byte[] {2},
                second.items().getFirst().value().bytes());
            assertArrayEquals(
                new byte[] {9},
                assertInstanceOf(
                    ZLinkStoreReadFound.class,
                    store.read(keyB, () -> false)
                        .toCompletableFuture().get())
                    .value().bytes());
        }
    }

    @Test
    void multiKeyCasKeepsEachConditionBoundToItsKey() throws Exception {
        String endpoint = System.getenv("ZLINK_REDIS_LOCATION_ENDPOINT");
        assumeTrue(
            endpoint != null && !endpoint.isBlank(),
            "ZLINK_REDIS_LOCATION_ENDPOINT is not set");
        try (var store = new ZLinkRedisLocationStore(
            new ZLinkRedisLocationOptions()
                .setConnectionString(endpoint)
                .setKeyPrefix("condition-order:" + UUID.randomUUID()))) {
            // These names intentionally iterate in the opposite order in a
            // HashSet. The provider must preserve request order because the
            // Lua script binds condition i to KEYS[i].
            var existingKey = new ZLinkStoreKey("z");
            var missingKey = new ZLinkStoreKey("a");
            var initial = assertInstanceOf(
                ZLinkStoreWriteApplied.class,
                store.write(
                    new ZLinkStoreWriteRequest(
                        List.of(new ZLinkStoreMissingCondition(existingKey)),
                        List.of(new ZLinkStorePut(
                            existingKey,
                            new byte[] {1},
                            null))),
                    () -> false).toCompletableFuture().get());

            assertInstanceOf(
                ZLinkStoreWriteApplied.class,
                store.write(
                    new ZLinkStoreWriteRequest(
                        List.of(
                            new ZLinkStoreVersionCondition(
                                existingKey,
                                initial.putVersions().get(existingKey)),
                            new ZLinkStoreMissingCondition(missingKey)),
                        List.of(new ZLinkStorePut(
                            missingKey,
                            new byte[] {2},
                            null))),
                    () -> false).toCompletableFuture().get());

            assertArrayEquals(
                new byte[] {2},
                assertInstanceOf(
                    ZLinkStoreReadFound.class,
                    store.read(missingKey, () -> false)
                        .toCompletableFuture().get())
                    .value().bytes());
        }
    }
}
