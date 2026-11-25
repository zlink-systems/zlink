package systems.zlink.framework.locations.etcd;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertInstanceOf;

import static systems.zlink.framework.locations.etcd.ZLinkEtcdProviderTest.*;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.*;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

class ZLinkEtcdCounterTest {
    @Test
    void counterRevisionRaceReevaluatesInsideProvider() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            backend.record(KEY.value(), decimal("1"), null);
            var attempts = new AtomicInteger();
            backend.txnFault =
                    (request, out) -> {
                        if (applying(request) && attempts.getAndIncrement() == 0)
                            backend.record(KEY.value(), decimal("2"), null);
                        backend.transact(request, out);
                    };

            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    await(
                            store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(
                                                    new ZLinkStoreSumAtMostCondition(
                                                            List.of(KEY), 1, 3)),
                                            List.of(new ZLinkStoreAdd(KEY, 1))),
                                    NEVER)));
            assertEquals(2, attempts.get());
            assertArrayEquals(decimal("3"), found(store, KEY.value()).value().bytes());

            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    await(
                            store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(
                                                    new ZLinkStoreSumAtMostCondition(
                                                            List.of(KEY), 1, 3)),
                                            List.of(new ZLinkStoreAdd(KEY, 1))),
                                    NEVER)));
            assertEquals(2, attempts.get());
        }
    }

    @Test
    void corruptCounterBatchLeavesOpaqueRowsUnchanged() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            var opaque = new ZLinkStoreKey("opaque");

            assertInstanceOf(
                    ZLinkStoreWriteCorrupt.class,
                    await(
                            store.write(
                                    new ZLinkStoreWriteRequest(
                                            List.of(),
                                            List.of(
                                                    new ZLinkStorePut(opaque, new byte[] {1}, null),
                                                    new ZLinkStoreAdd(KEY, -1))),
                                    NEVER)));
            assertInstanceOf(ZLinkStoreReadMissing.class, await(store.read(opaque, NEVER)));
            assertInstanceOf(ZLinkStoreReadMissing.class, await(store.read(KEY, NEVER)));
        }
    }

    private static byte[] decimal(String value) {
        return value.getBytes(StandardCharsets.US_ASCII);
    }
}
