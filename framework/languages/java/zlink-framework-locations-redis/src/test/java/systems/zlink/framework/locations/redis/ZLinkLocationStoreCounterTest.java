package systems.zlink.framework.locations.redis;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.ValueSource;

import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryProviderLocationStore;
import systems.zlink.framework.testsupport.ZLinkTestRedis;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import java.util.concurrent.TimeUnit;

class ZLinkLocationStoreCounterTest {
    private static final ZLinkStoreKey ACTIVE = new ZLinkStoreKey("active");
    private static final ZLinkStoreKey PENDING = new ZLinkStoreKey("pending");
    private static final ZLinkStoreKey MARKER = new ZLinkStoreKey("marker");

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void updatesForDistinctKeysShareOneCounterWithoutCasConflicts(boolean redis) throws Exception {
        try (var provider = newProvider(redis);
                var executor = Executors.newVirtualThreadPerTaskExecutor()) {
            var start = new CompletableFuture<Void>();
            var writes = new ArrayList<Future<ZLinkStoreWriteResult>>();
            for (int id = 0; id < 200; id++) {
                var object = new ZLinkStoreKey("object/" + id);
                writes.add(
                        executor.submit(
                                () -> {
                                    start.join();
                                    return write(
                                            provider.store(),
                                            List.of(
                                                    new ZLinkStoreMissingCondition(object),
                                                    new ZLinkStoreSumAtMostCondition(
                                                            List.of(ACTIVE, PENDING), 1, 200)),
                                            new ZLinkStorePut(object, new byte[] {1}, null),
                                            new ZLinkStoreAdd(PENDING, 1));
                                }));
            }
            start.complete(null);
            for (var write : writes) {
                var applied =
                        assertInstanceOf(
                                ZLinkStoreWriteApplied.class, write.get(10, TimeUnit.SECONDS));
                assertEquals(2, applied.putVersions().size());
            }
            assertEquals("200", read(provider.store(), PENDING));
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    write(
                            provider.store(),
                            List.of(
                                    new ZLinkStoreSumAtMostCondition(
                                            List.of(ACTIVE, PENDING), 1, 200)),
                            new ZLinkStoreAdd(PENDING, 1)));
            assertEquals("200", read(provider.store(), PENDING));

            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    write(
                            provider.store(),
                            List.of(),
                            new ZLinkStoreAdd(PENDING, -1),
                            new ZLinkStoreAdd(ACTIVE, 1)));
            assertEquals("199", read(provider.store(), PENDING));
            assertEquals("1", read(provider.store(), ACTIVE));
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void underflowLeavesEveryMutationUnapplied(boolean redis) throws Exception {
        try (var provider = newProvider(redis)) {
            var store = provider.store();
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    write(store, List.of(), new ZLinkStoreAdd(ACTIVE, 1)));
            var before = found(store, ACTIVE).value();
            assertInstanceOf(
                    ZLinkStoreWriteCorrupt.class,
                    write(
                            store,
                            List.of(),
                            new ZLinkStoreAdd(ACTIVE, 1),
                            new ZLinkStorePut(MARKER, new byte[] {1}, null),
                            new ZLinkStoreAdd(PENDING, -1)));
            assertEquals(before.version(), found(store, ACTIVE).value().version());
            assertEquals("1", read(store, ACTIVE));
            assertInstanceOf(
                    ZLinkStoreReadMissing.class,
                    store.read(MARKER, () -> false).toCompletableFuture().get());
            assertInstanceOf(
                    ZLinkStoreReadMissing.class,
                    store.read(PENDING, () -> false).toCompletableFuture().get());
        }
    }

    @ParameterizedTest
    @ValueSource(booleans = {false, true})
    void usesExactDecimalArithmeticAtLongBoundary(boolean redis) throws Exception {
        try (var provider = newProvider(redis)) {
            var store = provider.store();
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    write(
                            store,
                            List.of(),
                            new ZLinkStoreAdd(ACTIVE, Long.MAX_VALUE),
                            new ZLinkStoreAdd(PENDING, 1)));
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    write(
                            store,
                            List.of(
                                    new ZLinkStoreSumAtMostCondition(
                                            List.of(ACTIVE, PENDING), -1, Long.MAX_VALUE)),
                            new ZLinkStoreAdd(PENDING, -1)));
            assertInstanceOf(
                    ZLinkStoreWriteCorrupt.class,
                    write(store, List.of(), new ZLinkStoreAdd(ACTIVE, 1)));
            assertEquals(Long.toString(Long.MAX_VALUE), read(store, ACTIVE));
            assertEquals("0", read(store, PENDING));
        }
    }

    private static ZLinkStoreWriteResult write(
            ZLinkLocationStore store,
            List<ZLinkStoreCondition> conditions,
            ZLinkStoreMutation... mutations)
            throws Exception {
        return store.write(new ZLinkStoreWriteRequest(conditions, List.of(mutations)), () -> false)
                .toCompletableFuture()
                .get(10, TimeUnit.SECONDS);
    }

    private static ZLinkStoreReadFound found(ZLinkLocationStore store, ZLinkStoreKey key)
            throws Exception {
        return assertInstanceOf(
                ZLinkStoreReadFound.class,
                store.read(key, () -> false).toCompletableFuture().get(10, TimeUnit.SECONDS));
    }

    private static String read(ZLinkLocationStore store, ZLinkStoreKey key) throws Exception {
        return new String(found(store, key).value().bytes(), StandardCharsets.US_ASCII);
    }

    private static Provider newProvider(boolean redis) {
        if (!redis) return new Provider(new ZLinkInMemoryProviderLocationStore(), () -> {});
        var store =
                new ZLinkRedisLocationStore(
                        new ZLinkRedisLocationOptions()
                                .setConnectionString(ZLinkTestRedis.endpoint())
                                .setKeyPrefix("counter-test:" + UUID.randomUUID()));
        return new Provider(store, store);
    }

    private record Provider(ZLinkLocationStore store, AutoCloseable resource)
            implements AutoCloseable {
        @Override
        public void close() throws Exception {
            resource.close();
        }
    }
}
