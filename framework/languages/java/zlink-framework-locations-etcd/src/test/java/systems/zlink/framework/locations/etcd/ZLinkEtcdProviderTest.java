package systems.zlink.framework.locations.etcd;

import static org.junit.jupiter.api.Assertions.*;

import com.google.protobuf.ByteString;

import io.grpc.Status;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.locationprovider.*;
import systems.zlink.framework.locations.etcd.internal.proto.etcdserverpb.Rpc.*;

import java.time.Duration;
import java.time.Instant;
import java.util.*;
import java.util.concurrent.*;
import java.util.concurrent.atomic.*;

class ZLinkEtcdProviderTest {
    static final ZLinkStoreCancellation NEVER = () -> false;
    static final ZLinkStoreKey KEY = new ZLinkStoreKey("k");

    static <T> T await(CompletionStage<T> value) throws Exception {
        return value.toCompletableFuture().get(8, TimeUnit.SECONDS);
    }

    static <T extends Throwable> T assertFailure(
            Class<T> expected, org.junit.jupiter.api.function.Executable work) {
        Throwable cause = assertThrows(Exception.class, work);
        while (cause.getCause() != null) cause = cause.getCause();
        return assertInstanceOf(expected, cause);
    }

    static ZLinkStoreWriteRequest put(
            String key, byte[] bytes, Duration retention, ZLinkStoreCondition... conditions) {
        return new ZLinkStoreWriteRequest(
                List.of(conditions),
                List.of(new ZLinkStorePut(new ZLinkStoreKey(key), bytes, retention)));
    }

    static ZLinkStoreReadFound found(ZLinkEtcdLocationStore store, String key) throws Exception {
        return assertInstanceOf(
                ZLinkStoreReadFound.class, await(store.read(new ZLinkStoreKey(key), NEVER)));
    }

    static boolean applying(TxnRequest request) {
        return request.getSuccessList().stream().anyMatch(RequestOp::hasRequestPut);
    }

    @Test
    void operationsWithoutCancellationDoNotScheduleWatchers() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            assertInstanceOf(ZLinkStoreReadMissing.class, await(store.read(KEY, null)));
            assertEquals(0, store.scheduledCancellationTaskCount());
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    await(store.write(put("k", new byte[] {1}, null), null)));
            assertEquals(0, store.scheduledCancellationTaskCount());
            assertInstanceOf(
                    ZLinkStoreScanPageResult.class,
                    await(store.scan(new ZLinkStoreScanRequest("", null, 10), null)));
            assertEquals(0, store.scheduledCancellationTaskCount());
            await(store.read(KEY, NEVER));
            assertTrue(store.scheduledCancellationTaskCount() > 0);
        }
    }

    @Test
    void readWriteConditionsAndRetentionUseStoreClock() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            assertEquals(
                    new ZLinkStoreReadMissing(Instant.ofEpochMilli(1000)),
                    await(store.read(KEY, NEVER)));
            var written =
                    assertInstanceOf(
                            ZLinkStoreWriteApplied.class,
                            await(
                                    store.write(
                                            put(
                                                    "k",
                                                    new byte[] {0, -1},
                                                    Duration.ofNanos(1),
                                                    new ZLinkStoreMissingCondition(KEY)),
                                            NEVER)));
            var value = found(store, "k").value();
            assertArrayEquals(new byte[] {0, -1}, value.bytes());
            assertEquals(Instant.ofEpochMilli(1001), value.expiresAt());
            assertEquals(Instant.ofEpochMilli(1000), value.storeNow());
            assertEquals(written.putVersions().get(KEY), value.version());
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    await(
                            store.write(
                                    put(
                                            "other",
                                            new byte[0],
                                            null,
                                            new ZLinkStoreValueCondition(KEY, new byte[] {1, -1})),
                                    NEVER)));
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    await(
                            store.write(
                                    put(
                                            "other",
                                            new byte[0],
                                            null,
                                            new ZLinkStoreValueCondition(KEY, new byte[] {1})),
                                    NEVER)));
            assertInstanceOf(
                    ZLinkStoreReadMissing.class,
                    await(store.read(new ZLinkStoreKey("other"), NEVER)));
            backend.clock(1001);
            assertInstanceOf(ZLinkStoreReadMissing.class, await(store.read(KEY, NEVER)));
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    await(
                            store.write(
                                    put(
                                            "other",
                                            new byte[0],
                                            null,
                                            new ZLinkStoreVersionCondition(KEY, value.version())),
                                    NEVER)));
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    await(
                            store.write(
                                    put(
                                            "k",
                                            new byte[0],
                                            null,
                                            new ZLinkStoreMissingCondition(KEY)),
                                    NEVER)));
        }
    }

    @Test
    void authenticationErrorReauthenticatesAndResendsOnlyOnce() throws Exception {
        try (var backend = new EtcdTestServer();
                var store =
                        new ZLinkEtcdLocationStore(
                                backend.options()
                                        .setCredentials(
                                                new ZLinkEtcdCredentials("user", "password")))) {
            backend.txnFault =
                    (request, out) -> {
                        if (backend.txnCalls.get() == 1)
                            out.onError(
                                    Status.UNAUTHENTICATED
                                            .withDescription("etcdserver: invalid auth token")
                                            .asRuntimeException());
                        else backend.transact(request, out);
                    };
            assertInstanceOf(ZLinkStoreReadMissing.class, await(store.read(KEY, NEVER)));
            assertEquals(2, backend.authCalls.get());
            assertEquals(2, backend.txnCalls.get());
            backend.txnFault =
                    (request, out) ->
                            out.onError(
                                    Status.UNAUTHENTICATED
                                            .withDescription("invalid auth token")
                                            .asRuntimeException());
            int before = backend.txnCalls.get();
            assertThrows(Exception.class, () -> await(store.read(KEY, NEVER)));
            assertEquals(before + 2, backend.txnCalls.get());
            assertEquals(3, backend.authCalls.get());
        }
    }

    @Test
    void statusDescriptionControlsAuthenticationClassification() throws Exception {
        try (var backend = new EtcdTestServer();
                var store =
                        new ZLinkEtcdLocationStore(
                                backend.options()
                                        .setCredentials(new ZLinkEtcdCredentials("u", "p")))) {
            backend.txnFault =
                    (request, out) ->
                            out.onError(
                                    Status.UNAUTHENTICATED
                                            .withDescription("permission denied")
                                            .asRuntimeException());
            assertThrows(Exception.class, () -> await(store.read(KEY, NEVER)));
            assertEquals(1, backend.txnCalls.get());
            assertEquals(1, backend.authCalls.get());
        }
    }

    @Test
    void lostWriteOutcomeIsNotResentAndNextOperationMovesEndpoint() throws Exception {
        try (var first = new EtcdTestServer();
                var second = new EtcdTestServer();
                var store =
                        new ZLinkEtcdLocationStore(
                                first.options()
                                        .setEndpoints(
                                                List.of(first.endpoint(), second.endpoint())))) {
            var applies = new AtomicInteger();
            first.txnFault =
                    (request, out) -> {
                        if (applying(request)) {
                            applies.incrementAndGet();
                            out.onError(
                                    Status.UNAVAILABLE
                                            .withDescription("connection lost after commit")
                                            .asRuntimeException());
                        } else first.transact(request, out);
                    };
            assertThrows(
                    Exception.class, () -> await(store.write(put("k", new byte[0], null), NEVER)));
            assertEquals(1, applies.get());
            assertInstanceOf(ZLinkStoreReadMissing.class, await(store.read(KEY, NEVER)));
            assertEquals(1, second.txnCalls.get());
        }
    }

    @Test
    void noResponseUsesOperationDeadlineAndMovesNextOperation() throws Exception {
        try (var first = new EtcdTestServer();
                var second = new EtcdTestServer();
                var store =
                        new ZLinkEtcdLocationStore(
                                first.options()
                                        .setOperationTimeout(Duration.ofMillis(250))
                                        .setEndpoints(
                                                List.of(first.endpoint(), second.endpoint())))) {
            first.txnFault = (request, out) -> {};
            assertThrows(Exception.class, () -> await(store.read(KEY, NEVER)));
            assertInstanceOf(ZLinkStoreReadMissing.class, await(store.read(KEY, NEVER)));
            assertEquals(1, first.txnCalls.get());
            assertEquals(1, second.txnCalls.get());
        }
    }

    @Test
    void noSpaceAndUnrecognizedFailuresNeverBecomeConflictOrMissing() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            var attempts = new AtomicInteger();
            backend.txnFault =
                    (request, out) -> {
                        if (applying(request)) {
                            attempts.incrementAndGet();
                            out.onError(
                                    Status.RESOURCE_EXHAUSTED
                                            .withDescription(
                                                    "etcdserver: mvcc: database space exceeded")
                                            .asRuntimeException());
                        } else backend.transact(request, out);
                    };
            assertThrows(
                    Exception.class, () -> await(store.write(put("k", new byte[0], null), NEVER)));
            assertEquals(1, attempts.get());
            backend.txnFault =
                    (request, out) ->
                            out.onError(
                                    Status.OUT_OF_RANGE
                                            .withDescription("some other range error")
                                            .asRuntimeException());
            assertThrows(Exception.class, () -> await(store.read(KEY, NEVER)));
            assertThrows(
                    Exception.class,
                    () -> await(store.scan(new ZLinkStoreScanRequest("", null, 1), NEVER)));
        }
    }

    @Test
    void authenticationAndAllWriteRpcsShareOneAbsoluteDeadline() throws Exception {
        try (var backend = new EtcdTestServer();
                var store =
                        new ZLinkEtcdLocationStore(
                                backend.options()
                                        .setOperationTimeout(Duration.ofSeconds(2))
                                        .setCredentials(new ZLinkEtcdCredentials("u", "p")))) {
            backend.txnFault =
                    (request, out) -> {
                        if (backend.txnCalls.get() == 1) {
                            try {
                                // A fresh per-RPC deadline would shift by this delay.
                                Thread.sleep(500);
                            } catch (InterruptedException error) {
                                Thread.currentThread().interrupt();
                            }
                            out.onError(
                                    Status.INVALID_ARGUMENT
                                            .withDescription("revision of auth store is old")
                                            .asRuntimeException());
                        } else backend.transact(request, out);
                    };
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    await(store.write(put("k", new byte[0], null), NEVER)));
            assertEquals(5, backend.deadlines.size());
            long earliest = Collections.min(backend.deadlines);
            long latest = Collections.max(backend.deadlines);
            assertTrue(
                    latest - earliest < TimeUnit.MILLISECONDS.toNanos(250),
                    "RPCs must retain the operation deadline");
        }
    }

    @Test
    void conditionPayloadCompactionStartsDecisionAgain() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            backend.record("k", new byte[] {1}, null);
            var compacted = new AtomicBoolean();
            backend.txnFault =
                    (request, out) -> {
                        boolean historical =
                                request.getSuccessList().stream()
                                        .anyMatch(
                                                op ->
                                                        op.hasRequestRange()
                                                                && op.getRequestRange()
                                                                                .getRevision()
                                                                        > 0);
                        if (historical && compacted.compareAndSet(false, true))
                            out.onError(
                                    Status.OUT_OF_RANGE
                                            .withDescription("required revision has been compacted")
                                            .asRuntimeException());
                        else backend.transact(request, out);
                    };
            assertInstanceOf(
                    ZLinkStoreWriteApplied.class,
                    await(
                            store.write(
                                    put(
                                            "x",
                                            new byte[0],
                                            null,
                                            new ZLinkStoreValueCondition(KEY, new byte[] {1})),
                                    NEVER)));
            assertEquals(5, backend.txnCalls.get());
        }
    }

    @Test
    void failedApplyComparisonReevaluatesChangedCondition() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            var changed = new AtomicBoolean();
            backend.txnFault =
                    (request, out) -> {
                        if (applying(request) && changed.compareAndSet(false, true))
                            backend.record("k", new byte[] {1}, null);
                        backend.transact(request, out);
                    };
            assertInstanceOf(
                    ZLinkStoreWriteConflict.class,
                    await(
                            store.write(
                                    put(
                                            "x",
                                            new byte[0],
                                            null,
                                            new ZLinkStoreMissingCondition(KEY)),
                                    NEVER)));
            assertInstanceOf(
                    ZLinkStoreReadMissing.class, await(store.read(new ZLinkStoreKey("x"), NEVER)));
            assertEquals(4, backend.txnCalls.get());
        }
    }

    @Test
    void concurrentMissingConditionsHaveExactlyOneAppliedWinner() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            var operations = new ArrayList<CompletionStage<ZLinkStoreWriteResult>>();
            for (int i = 0; i < 16; i++)
                operations.add(
                        store.write(
                                put(
                                        "k",
                                        new byte[] {(byte) i},
                                        null,
                                        new ZLinkStoreMissingCondition(KEY)),
                                NEVER));
            int applied = 0;
            int conflicts = 0;
            for (var operation : operations) {
                var result = await(operation);
                if (result instanceof ZLinkStoreWriteApplied) applied++;
                else if (result instanceof ZLinkStoreWriteConflict) conflicts++;
                else fail("Unexpected Store result");
            }
            assertEquals(1, applied);
            assertEquals(15, conflicts);
            assertInstanceOf(ZLinkStoreReadFound.class, await(store.read(KEY, NEVER)));
        }
    }

    @Test
    void expiryBetweenDecisionAndApplyIsReevaluated() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            backend.record("k", new byte[] {1}, 1001L);
            var version = found(store, "k").value().version();
            var changed = new AtomicBoolean();
            backend.txnFault =
                    (request, out) -> {
                        if (applying(request) && changed.compareAndSet(false, true))
                            backend.clock(1001);
                        backend.transact(request, out);
                    };
            var conflict =
                    assertInstanceOf(
                            ZLinkStoreWriteConflict.class,
                            await(
                                    store.write(
                                            put(
                                                    "x",
                                                    new byte[0],
                                                    null,
                                                    new ZLinkStoreVersionCondition(KEY, version)),
                                            NEVER)));
            assertEquals(Instant.ofEpochMilli(1001), conflict.storeNow());
            assertInstanceOf(
                    ZLinkStoreReadMissing.class, await(store.read(new ZLinkStoreKey("x"), NEVER)));
        }
    }

    @Test
    void snapshotRetainsValuesClockAndPageBoundAndCompactionExpires() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            backend.record("p/a", new byte[] {1}, null);
            backend.record("p/b", new byte[] {2}, 1001L);
            var first =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    await(
                                            store.scan(
                                                    new ZLinkStoreScanRequest("p/", null, 1),
                                                    NEVER)))
                            .value();
            assertEquals(1, first.items().size());
            assertNotNull(first.nextCursor());
            backend.record("p/b", new byte[] {9}, null);
            backend.clock(2000);
            var second =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    await(
                                            store.scan(
                                                    new ZLinkStoreScanRequest(
                                                            "p/", first.nextCursor(), 1),
                                                    NEVER)))
                            .value();
            assertEquals(first.storeNow(), second.storeNow());
            assertArrayEquals(new byte[] {2}, second.items().get(0).value().bytes());
            assertNull(second.nextCursor());
            backend.compacted = backend.revision;
            assertInstanceOf(
                    ZLinkStoreScanExpired.class,
                    await(
                            store.scan(
                                    new ZLinkStoreScanRequest("p/", first.nextCursor(), 1),
                                    NEVER)));
        }
    }

    @Test
    void scanSkipsMoreThanOneMetadataBatchAndChecksPayloadCorruption() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            for (int i = 0; i < 1001; i++)
                backend.record(String.format("p/%04d", i), new byte[0], 1000L);
            backend.record("p/z", new byte[] {1}, null);
            var page =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    await(
                                            store.scan(
                                                    new ZLinkStoreScanRequest("p/", null, 1000),
                                                    NEVER)))
                            .value();
            assertEquals(1, page.items().size());
            assertEquals("p/z", page.items().get(0).key().value());
            assertTrue(backend.rangeCalls.get() >= 2);
            backend.put(EtcdTestServer.bytes("test\0p\0p/z"), ByteString.EMPTY);
            assertThrows(
                    Exception.class,
                    () -> await(store.scan(new ZLinkStoreScanRequest("p/", null, 1000), NEVER)));
        }
    }

    @Test
    void scanCompactionBetweenMetadataAndPayloadReturnsExpired() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            backend.record("k", new byte[] {1}, null);
            backend.txnFault =
                    (request, out) -> {
                        if (request.getSuccessList().stream()
                                .anyMatch(
                                        op ->
                                                op.hasRequestRange()
                                                        && op.getRequestRange().getRevision() > 0))
                            backend.compacted = backend.revision;
                        backend.transact(request, out);
                    };
            assertInstanceOf(
                    ZLinkStoreScanExpired.class,
                    await(store.scan(new ZLinkStoreScanRequest("", null, 1000), NEVER)));
        }
    }

    @Test
    void scanHonorsEncodedPageByteBoundAndCursorScope() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            byte[] payload = new byte[1024 * 1024];
            for (int i = 0; i < 5; i++) backend.record("p/" + i, payload, null);
            var first =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    await(
                                            store.scan(
                                                    new ZLinkStoreScanRequest("p/", null, 1000),
                                                    NEVER)))
                            .value();
            assertEquals(3, first.items().size());
            assertNotNull(first.nextCursor());
            assertTrue(
                    first.nextCursor()
                                    .value()
                                    .getBytes(java.nio.charset.StandardCharsets.UTF_8)
                                    .length
                            <= 4096);
            assertInstanceOf(
                    ZLinkStoreScanExpired.class,
                    await(
                            store.scan(
                                    new ZLinkStoreScanRequest("other/", first.nextCursor(), 1000),
                                    NEVER)));
            var second =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    await(
                                            store.scan(
                                                    new ZLinkStoreScanRequest(
                                                            "p/", first.nextCursor(), 1000),
                                                    NEVER)))
                            .value();
            assertEquals(2, second.items().size());
            assertNull(second.nextCursor());
            backend.cluster++;
            assertInstanceOf(
                    ZLinkStoreScanExpired.class,
                    await(
                            store.scan(
                                    new ZLinkStoreScanRequest("p/", first.nextCursor(), 1000),
                                    NEVER)));
        }
    }

    @Test
    void cancellationBeforeIoAndInputBoundsSendNoRpc() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            assertFailure(CancellationException.class, () -> await(store.read(KEY, () -> true)));
            assertFailure(
                    IllegalArgumentException.class,
                    () -> await(store.read(new ZLinkStoreKey("a".repeat(1025)), NEVER)));
            assertFailure(
                    IllegalArgumentException.class,
                    () -> await(store.write(put("k", new byte[1048577], null), NEVER)));
            assertFailure(
                    IllegalArgumentException.class,
                    () -> await(store.write(put("k", new byte[0], Duration.ZERO), NEVER)));
            assertFailure(
                    IllegalArgumentException.class,
                    () ->
                            await(
                                    store.write(
                                            new ZLinkStoreWriteRequest(
                                                    List.of(
                                                            new ZLinkStoreMissingCondition(KEY),
                                                            new ZLinkStoreMissingCondition(KEY)),
                                                    List.of()),
                                            NEVER)));
            assertFailure(
                    IllegalArgumentException.class,
                    () -> await(store.scan(new ZLinkStoreScanRequest("", null, 1001), NEVER)));
            assertFailure(
                    IllegalArgumentException.class,
                    () ->
                            await(
                                    store.scan(
                                            new ZLinkStoreScanRequest(
                                                    "",
                                                    new ZLinkStoreScanCursor("a".repeat(4097)),
                                                    1),
                                            NEVER)));
            var tooMany = new ArrayList<ZLinkStoreMutation>();
            for (int i = 0; i < 2049; i++)
                tooMany.add(new ZLinkStoreDelete(new ZLinkStoreKey("k" + i)));
            assertFailure(
                    IllegalArgumentException.class,
                    () ->
                            await(
                                    store.write(
                                            new ZLinkStoreWriteRequest(List.of(), tooMany),
                                            NEVER)));
            var tooLarge = new ArrayList<ZLinkStoreMutation>();
            for (int i = 0; i < 4; i++)
                tooLarge.add(
                        new ZLinkStorePut(new ZLinkStoreKey("k" + i), new byte[1048576], null));
            assertFailure(
                    IllegalArgumentException.class,
                    () ->
                            await(
                                    store.write(
                                            new ZLinkStoreWriteRequest(List.of(), tooLarge),
                                            NEVER)));
            assertEquals(0, backend.txnCalls.get());
            assertEquals(0, backend.rangeCalls.get());
        }
    }

    @Test
    void cancellationDuringIoCancelsCallAndCloseDrainsBeforeRejectingNewWork() throws Exception {
        try (var backend = new EtcdTestServer()) {
            var store = new ZLinkEtcdLocationStore(backend.options());
            var entered = new CountDownLatch(1);
            var rpcCancelled = new CountDownLatch(1);
            backend.txnFault =
                    (request, out) -> {
                        io.grpc.Context.current()
                                .addListener(context -> rpcCancelled.countDown(), Runnable::run);
                        entered.countDown();
                    };
            var cancelled = new AtomicBoolean();
            var read = store.read(KEY, cancelled::get);
            assertTrue(entered.await(2, TimeUnit.SECONDS));
            var closer = Executors.newFixedThreadPool(2);
            var closersReady = new CountDownLatch(2);
            var closeTogether = new CountDownLatch(1);
            Callable<Void> close =
                    () -> {
                        closersReady.countDown();
                        closeTogether.await();
                        store.close();
                        return null;
                    };
            try {
                var closing = closer.submit(close);
                var alsoClosing = closer.submit(close);
                assertTrue(closersReady.await(2, TimeUnit.SECONDS));
                closeTogether.countDown();
                Thread.sleep(30);
                assertFalse(closing.isDone());
                assertFalse(alsoClosing.isDone());
                assertThrows(Exception.class, () -> await(store.read(KEY, NEVER)));
                cancelled.set(true);
                assertThrows(Exception.class, () -> await(read));
                assertTrue(rpcCancelled.await(2, TimeUnit.SECONDS));
                closing.get(2, TimeUnit.SECONDS);
                alsoClosing.get(2, TimeUnit.SECONDS);
                store.close();
            } finally {
                closeTogether.countDown();
                cancelled.set(true);
                closer.shutdownNow();
                store.close();
            }
        }
    }

    @Test
    void publicOperationsCanChainFromRpcCompletionCallbacks() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            var entered = new CountDownLatch(1);
            var firstRequest = new AtomicReference<TxnRequest>();
            var firstReply = new AtomicReference<io.grpc.stub.StreamObserver<TxnResponse>>();
            backend.txnFault =
                    (request, out) -> {
                        if (backend.txnCalls.get() == 1) {
                            firstRequest.set(request);
                            firstReply.set(out);
                            entered.countDown();
                        } else backend.transact(request, out);
                    };
            var chain =
                    store.read(KEY, NEVER)
                            .thenCompose(
                                    missing -> {
                                        assertInstanceOf(ZLinkStoreReadMissing.class, missing);
                                        return store.write(put("k", new byte[] {3}, null), NEVER);
                                    })
                            .thenCompose(
                                    applied -> {
                                        assertInstanceOf(ZLinkStoreWriteApplied.class, applied);
                                        return store.read(KEY, NEVER);
                                    })
                            .thenCompose(
                                    found -> {
                                        assertArrayEquals(
                                                new byte[] {3},
                                                assertInstanceOf(ZLinkStoreReadFound.class, found)
                                                        .value()
                                                        .bytes());
                                        return store.scan(
                                                new ZLinkStoreScanRequest("", null, 1), NEVER);
                                    });
            assertTrue(entered.await(2, TimeUnit.SECONDS));
            backend.transact(firstRequest.get(), firstReply.get());
            var page = assertInstanceOf(ZLinkStoreScanPageResult.class, await(chain)).value();
            assertEquals(1, page.items().size());
            assertEquals(KEY, page.items().get(0).key());
            assertArrayEquals(new byte[] {3}, page.items().get(0).value().bytes());
            assertNull(page.nextCursor());
        }
    }

    @Test
    void closeFromRpcCompletionCallbackFinishesWithoutDeadlock() throws Exception {
        try (var backend = new EtcdTestServer()) {
            var store = new ZLinkEtcdLocationStore(backend.options());
            var entered = new CountDownLatch(1);
            var request = new AtomicReference<TxnRequest>();
            var reply = new AtomicReference<io.grpc.stub.StreamObserver<TxnResponse>>();
            backend.txnFault =
                    (incoming, out) -> {
                        request.set(incoming);
                        reply.set(out);
                        entered.countDown();
                    };
            var closedFromCallback =
                    store.read(KEY, NEVER)
                            .thenApply(
                                    result -> {
                                        store.close();
                                        return result;
                                    });
            assertTrue(entered.await(2, TimeUnit.SECONDS));
            backend.transact(request.get(), reply.get());
            assertInstanceOf(ZLinkStoreReadMissing.class, await(closedFromCallback));
            assertFailure(IllegalStateException.class, () -> await(store.read(KEY, NEVER)));
            store.close();
        }
    }

    @Test
    void cancellationAtAppliedResponseBoundaryReportsUnknownWriteOutcome() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            var cancelled = new AtomicBoolean();
            backend.txnFault =
                    (request, out) -> {
                        if (!applying(request)) {
                            backend.transact(request, out);
                            return;
                        }
                        backend.transact(
                                request,
                                new io.grpc.stub.StreamObserver<>() {
                                    public void onNext(TxnResponse response) {
                                        cancelled.set(true);
                                        out.onNext(response);
                                    }

                                    public void onError(Throwable error) {
                                        out.onError(error);
                                    }

                                    public void onCompleted() {
                                        out.onCompleted();
                                    }
                                });
                    };
            var failure =
                    assertThrows(
                            Exception.class,
                            () ->
                                    await(
                                            store.write(
                                                    put("k", new byte[] {7}, null),
                                                    cancelled::get)));
            Throwable cause = failure;
            while (cause.getCause() != null) cause = cause.getCause();
            assertInstanceOf(java.io.IOException.class, cause);
            assertTrue(cause.getMessage().contains("write outcome may be unknown"));
            assertArrayEquals(
                    new byte[] {7},
                    backend.records
                            .get(EtcdTestServer.bytes("test\0p\0k"))
                            .getValue()
                            .toByteArray());
            assertEquals(2, backend.txnCalls.get(), "Applied writes must not be submitted again");
        }
    }

    @Test
    void cancellationAtReadResponseBoundaryRejectsTheReceivedResult() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            var cancelled = new AtomicBoolean();
            backend.txnFault =
                    (request, out) -> {
                        backend.transact(
                                request,
                                new io.grpc.stub.StreamObserver<>() {
                                    public void onNext(TxnResponse response) {
                                        cancelled.set(true);
                                        out.onNext(response);
                                    }

                                    public void onError(Throwable error) {
                                        out.onError(error);
                                    }

                                    public void onCompleted() {
                                        out.onCompleted();
                                    }
                                });
                    };
            var failure =
                    assertThrows(Exception.class, () -> await(store.read(KEY, cancelled::get)));
            Throwable cause = failure;
            while (cause.getCause() != null) cause = cause.getCause();
            assertInstanceOf(java.io.IOException.class, cause);
            assertEquals(1, backend.txnCalls.get());
        }
    }

    @Test
    void optionsAreValidatedBeforeChannelIo() throws Exception {
        try (var backend = new EtcdTestServer()) {
            for (String endpoint :
                    List.of(
                            "localhost:2379",
                            "ftp://localhost",
                            "http://localhost/path",
                            "http://u:p@localhost",
                            "http://localhost?x=1"))
                assertThrows(
                        IllegalArgumentException.class,
                        () ->
                                new ZLinkEtcdLocationStore(
                                        backend.options().setEndpoints(List.of(endpoint))));
            assertThrows(
                    IllegalArgumentException.class,
                    () -> new ZLinkEtcdLocationStore(backend.options().setNamespace("")));
            assertThrows(
                    IllegalArgumentException.class,
                    () -> new ZLinkEtcdLocationStore(backend.options().setNamespace("a\0b")));
            assertThrows(
                    IllegalArgumentException.class,
                    () ->
                            new ZLinkEtcdLocationStore(
                                    backend.options().setOperationTimeout(Duration.ZERO)));
            assertThrows(
                    IllegalArgumentException.class,
                    () ->
                            new ZLinkEtcdLocationStore(
                                    backend.options()
                                            .setCredentials(new ZLinkEtcdCredentials("", "p"))));
            assertThrows(
                    IllegalArgumentException.class,
                    () ->
                            new ZLinkEtcdLocationStore(
                                    backend.options().setTls(new ZLinkEtcdTlsOptions())));
            assertEquals(0, backend.txnCalls.get());
        }
    }

    @Test
    void longestPrefixAndKeyKeepCursorWithinBoundAndResumeSnapshot() throws Exception {
        String namespace = "한".repeat(21) + "a";
        String prefix = "x".repeat(1023);
        assertEquals(64, namespace.getBytes(java.nio.charset.StandardCharsets.UTF_8).length);
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options().setNamespace(namespace))) {
            backend.put(EtcdTestServer.bytes(namespace + "\0c"), EtcdTestServer.clockBytes(1000));
            backend.record(namespace, prefix + "a", new byte[] {1}, null);
            backend.record(namespace, prefix + "b", new byte[] {2}, null);
            var first =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    await(
                                            store.scan(
                                                    new ZLinkStoreScanRequest(prefix, null, 1),
                                                    NEVER)))
                            .value();
            assertEquals(prefix + "a", first.items().get(0).key().value());
            assertNotNull(first.nextCursor());
            assertTrue(
                    first.nextCursor()
                                    .value()
                                    .getBytes(java.nio.charset.StandardCharsets.UTF_8)
                                    .length
                            <= 4096);
            var second =
                    assertInstanceOf(
                                    ZLinkStoreScanPageResult.class,
                                    await(
                                            store.scan(
                                                    new ZLinkStoreScanRequest(
                                                            prefix, first.nextCursor(), 1),
                                                    NEVER)))
                            .value();
            assertEquals(prefix + "b", second.items().get(0).key().value());
            assertEquals(first.storeNow(), second.storeNow());
            assertNull(second.nextCursor());
        }
    }

    @Test
    void overflowingExpiryIsCallerErrorAndDoesNotSendAnApplyTransaction() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            backend.clock(9_223_372_036_854L);
            var failure =
                    assertThrows(
                            Exception.class,
                            () ->
                                    await(
                                            store.write(
                                                    put("k", new byte[] {1}, Duration.ofMillis(1)),
                                                    NEVER)));
            Throwable cause = failure;
            while (cause.getCause() != null) cause = cause.getCause();
            assertInstanceOf(IllegalArgumentException.class, cause);
            assertEquals(1, backend.txnCalls.get());
            assertFalse(backend.records.containsKey(EtcdTestServer.bytes("test\0m\0k")));
            assertFalse(backend.records.containsKey(EtcdTestServer.bytes("test\0p\0k")));
        }
    }

    @Test
    void snapshotCursorExpiresAfterFiveMinutesAndRejectsMutation() {
        var cursors = new EtcdCursor();
        var prefix = ByteString.copyFromUtf8("p/");
        var current = new EtcdCursor.Snapshot(42, 2, 1000, System.nanoTime(), prefix);
        String encoded = cursors.encode(current, "test", prefix);
        assertNotNull(cursors.decode(encoded, "test", prefix));
        var expired =
                new EtcdCursor.Snapshot(
                        42, 2, 1000, System.nanoTime() - TimeUnit.MINUTES.toNanos(5), prefix);
        assertNull(cursors.decode(cursors.encode(expired, "test", prefix), "test", prefix));
        char changed = encoded.charAt(0) == 'A' ? 'B' : 'A';
        assertNull(cursors.decode(changed + encoded.substring(1), "test", prefix));
        assertNull(new EtcdCursor().decode(encoded, "test", prefix));
    }

    @Test
    void missingStoreClockFailsEveryOperationWithoutMutation() throws Exception {
        try (var backend = new EtcdTestServer();
                var store = new ZLinkEtcdLocationStore(backend.options())) {
            synchronized (backend) {
                backend.records.clear();
            }
            assertThrows(Exception.class, () -> await(store.read(KEY, NEVER)));
            assertThrows(
                    Exception.class, () -> await(store.write(put("k", new byte[0], null), NEVER)));
            assertThrows(
                    Exception.class,
                    () -> await(store.scan(new ZLinkStoreScanRequest("", null, 1), NEVER)));
            assertTrue(backend.records.isEmpty());
        }
    }
}
