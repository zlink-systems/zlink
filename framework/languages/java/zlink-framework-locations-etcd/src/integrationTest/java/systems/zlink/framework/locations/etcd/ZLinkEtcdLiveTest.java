package systems.zlink.framework.locations.etcd;

import static org.junit.jupiter.api.Assertions.*;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.*;

import java.time.Duration;
import java.util.*;
import java.util.concurrent.*;

/** Requires the repository Docker cluster and the Node Store clock process. */
class ZLinkEtcdLiveTest {
    static final ZLinkStoreCancellation NEVER = () -> false;

    static <T> T await(CompletionStage<T> stage) throws Exception {
        return stage.toCompletableFuture().get(10, TimeUnit.SECONDS);
    }

    static ZLinkStoreWriteRequest put(
            ZLinkStoreKey key,
            byte[] bytes,
            Duration retention,
            ZLinkStoreCondition... conditions) {
        return new ZLinkStoreWriteRequest(
                List.of(conditions), List.of(new ZLinkStorePut(key, bytes, retention)));
    }

    @Test
    void realEtcdConditionsExpirationMaximumBatchAndSnapshotPage() throws Exception {
        String endpoints = System.getenv("ZLINK_ETCD_ENDPOINTS");
        String namespace = System.getenv("ZLINK_ETCD_NAMESPACE");
        assumeTrue(
                endpoints != null && namespace != null,
                "Set ZLINK_ETCD_ENDPOINTS and initialize ZLINK_ETCD_NAMESPACE with the Node admin clock");
        var options =
                new ZLinkEtcdLocationOptions()
                        .setEndpoints(List.of(endpoints.split(",")))
                        .setNamespace(namespace);
        String prefix = "java-live/" + UUID.randomUUID() + "/";
        var key = new ZLinkStoreKey(prefix + "condition");
        var cleanup = new ArrayList<ZLinkStoreMutation>();
        cleanup.add(new ZLinkStoreDelete(key));
        try (var store = new ZLinkEtcdLocationStore(options)) {
            try {
                var applied =
                        assertInstanceOf(
                                ZLinkStoreWriteApplied.class,
                                await(
                                        store.write(
                                                put(
                                                        key,
                                                        new byte[] {1, 0, -1},
                                                        Duration.ofMillis(1),
                                                        new ZLinkStoreMissingCondition(key)),
                                                NEVER)));
                var conflict =
                        await(
                                store.write(
                                        put(
                                                key,
                                                new byte[] {2},
                                                null,
                                                new ZLinkStoreValueCondition(key, new byte[] {2})),
                                        NEVER));
                assertInstanceOf(ZLinkStoreWriteConflict.class, conflict);
                long waitUntil = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
                ZLinkStoreReadResult result;
                do {
                    result = await(store.read(key, NEVER));
                    if (result instanceof ZLinkStoreReadMissing) break;
                    Thread.sleep(50);
                } while (System.nanoTime() < waitUntil);
                assertInstanceOf(
                        ZLinkStoreReadMissing.class,
                        result,
                        "Node Store clock must advance through expiry");
                assertInstanceOf(
                        ZLinkStoreWriteConflict.class,
                        await(
                                store.write(
                                        put(
                                                key,
                                                new byte[0],
                                                null,
                                                new ZLinkStoreVersionCondition(
                                                        key, applied.putVersions().get(key))),
                                        NEVER)));

                var mutations = new ArrayList<ZLinkStoreMutation>();
                var keys = new ArrayList<ZLinkStoreKey>();
                String batchPrefix = prefix + "batch/";
                int keyBytes = 0;
                for (int i = 0; i < 2048; i++) {
                    var batchKey =
                            new ZLinkStoreKey(batchPrefix + String.format(Locale.ROOT, "%04d", i));
                    keys.add(batchKey);
                    keyBytes +=
                            batchKey.value()
                                    .getBytes(java.nio.charset.StandardCharsets.UTF_8)
                                    .length;
                }
                int remaining = 4 * 1024 * 1024 - keyBytes;
                for (int i = 0; i < keys.size(); i++) {
                    int length = remaining / (keys.size() - i);
                    remaining -= length;
                    byte[] payload = new byte[length];
                    Arrays.fill(payload, (byte) i);
                    mutations.add(new ZLinkStorePut(keys.get(i), payload, null));
                    cleanup.add(new ZLinkStoreDelete(keys.get(i)));
                }
                assertEquals(0, remaining);
                var batch =
                        assertInstanceOf(
                                ZLinkStoreWriteApplied.class,
                                await(
                                        store.write(
                                                new ZLinkStoreWriteRequest(List.of(), mutations),
                                                NEVER)));
                assertEquals(2048, batch.putVersions().size());
                var first =
                        assertInstanceOf(
                                        ZLinkStoreScanPageResult.class,
                                        await(
                                                store.scan(
                                                        new ZLinkStoreScanRequest(
                                                                batchPrefix, null, 1000),
                                                        NEVER)))
                                .value();
                assertEquals(1000, first.items().size());
                assertNotNull(first.nextCursor());
                var replaced = keys.get(1500);
                assertInstanceOf(
                        ZLinkStoreWriteApplied.class,
                        await(store.write(put(replaced, new byte[] {9}, null), NEVER)));
                var second =
                        assertInstanceOf(
                                        ZLinkStoreScanPageResult.class,
                                        await(
                                                store.scan(
                                                        new ZLinkStoreScanRequest(
                                                                batchPrefix,
                                                                first.nextCursor(),
                                                                1000),
                                                        NEVER)))
                                .value();
                assertEquals(1000, second.items().size());
                assertEquals(first.storeNow(), second.storeNow());
                var historical =
                        second.items().stream()
                                .filter(item -> item.key().equals(replaced))
                                .findFirst()
                                .orElseThrow();
                assertEquals(batch.putVersions().get(replaced), historical.value().version());
                assertTrue(historical.value().bytes().length > 1);
                var last =
                        assertInstanceOf(
                                        ZLinkStoreScanPageResult.class,
                                        await(
                                                store.scan(
                                                        new ZLinkStoreScanRequest(
                                                                batchPrefix,
                                                                second.nextCursor(),
                                                                1000),
                                                        NEVER)))
                                .value();
                assertEquals(48, last.items().size());
                assertNull(last.nextCursor());
                assertEquals(first.storeNow(), last.storeNow());
            } finally {
                for (int start = 0; start < cleanup.size(); start += 2048)
                    await(
                            store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(),
                                            cleanup.subList(
                                                    start, Math.min(start + 2048, cleanup.size()))),
                                    NEVER));
            }
        }
    }
}
