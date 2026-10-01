package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkStreamR3RegressionTest {
    @Test
    void transportCloseFailureBecomesDisconnectedEvent() {
        List<ZLinkStreamError> errors = new ArrayList<>();
        var lifecycle =
                new ZLinkStreamConnectionLifecycle(
                        null, null, null, null, null, errors::add, null, null, null, null, null);
        IllegalStateException failure = new IllegalStateException("transport close failure");
        lifecycle.publishCloseFailure(
                lifecycle.closeQuietly(
                        new ZLinkStreamTransportConnection() {
                            public CompletionStage<ZLinkStreamWireProtocol.Frame> readFrameAsync() {
                                throw new UnsupportedOperationException();
                            }

                            public CompletionStage<Void> writeAsync(byte[] frame) {
                                throw new UnsupportedOperationException();
                            }

                            public boolean isOpen() {
                                return false;
                            }

                            public void close() {
                                throw failure;
                            }
                        }));
        assertEquals(1, errors.size());
        assertEquals(ZLinkStreamErrorCode.DISCONNECTED, errors.get(0).code());
        assertSame(failure, errors.get(0).exception());
    }

    @Test
    void cancelledQueuedWriteNeverStarts() {
        ZLinkStreamSendChain chain = new ZLinkStreamSendChain();
        CompletableFuture<Void> first = new CompletableFuture<>();
        chain.enqueue(() -> first);
        AtomicInteger writes = new AtomicInteger();
        CompletableFuture<Void> queued =
                chain.enqueue(
                        () -> {
                            writes.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });
        assertTrue(queued.cancel(false));
        first.complete(null);
        assertEquals(0, writes.get());
    }

    @Test
    void pendingCompletionCannotStartExpiredFrameBeforeCleanup() {
        ZLinkStreamSendChain chain = new ZLinkStreamSendChain();
        CompletableFuture<Void> first = new CompletableFuture<>();
        chain.enqueue(() -> first);
        AtomicInteger writes = new AtomicInteger();
        var requests = new ZLinkStreamPendingRequests();
        var pending =
                chain.enqueueRequestDeferred(
                        () -> {
                            writes.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        },
                        failure -> requests.fail(7L, failure),
                        owned ->
                                requests.add(
                                        7L,
                                        "Echo",
                                        owned,
                                        (reply, complete) -> complete.getAsBoolean(),
                                        (failure, complete) -> complete.getAsBoolean()));
        chain.pump();
        pending.whenComplete((reply, error) -> first.complete(null));
        requests.fail(7L, new TimeoutException("request expired"));
        assertEquals(0, writes.get());
    }

    @Test
    void whitespaceNameIsRejectedByWireOwner() {
        var header =
                new ZLinkStreamWireProtocol.Header(
                        ZLinkStreamWireProtocol.KIND_SEND,
                        ZLinkStreamWireProtocol.CODEC_RAW,
                        0,
                        null,
                        " \t",
                        Map.of(),
                        null);
        assertThrows(
                IllegalArgumentException.class, () -> ZLinkStreamWireProtocol.encodeHeader(header));
    }

    @Test
    void actionFailuresPreserveIdentity() {
        IOException io = new IOException("action failure");
        assertSame(
                io,
                assertThrows(
                        IOException.class,
                        () ->
                                ZLinkStreamAssert.expectFailure(
                                        () -> {
                                            throw io;
                                        },
                                        null)));
        IllegalArgumentException invalid = new IllegalArgumentException("action validation");
        assertSame(
                invalid,
                assertThrows(
                        IllegalArgumentException.class,
                        () ->
                                ZLinkStreamAssert.expectFailure(
                                        () -> {
                                            throw invalid;
                                        },
                                        null)));
    }

    @Test
    void actionWrapperPreservesIdentity() {
        CompletionException wrapper = new CompletionException(new IOException("action failure"));
        assertSame(
                wrapper,
                assertThrows(
                        CompletionException.class,
                        () ->
                                ZLinkStreamAssert.expectFailure(
                                        () -> {
                                            throw wrapper;
                                        },
                                        null)));
        assertSame(
                wrapper,
                assertThrows(
                        CompletionException.class,
                        () ->
                                ZLinkStreamAssert.expectTimeout(
                                        () -> {
                                            throw wrapper;
                                        })));
        CompletionException coded =
                new CompletionException(ZLinkStreamException.disconnected("closed"));
        assertSame(
                coded,
                assertThrows(
                        CompletionException.class,
                        () ->
                                ZLinkStreamAssert.expectTimeout(
                                        () -> {
                                            throw coded;
                                        })));
    }

    @Test
    void callerCancellationRemovesRequestBeforeCompletionCallback() {
        var chain = new ZLinkStreamSendChain();
        var first = new CompletableFuture<Void>();
        chain.enqueue(() -> first);
        var requests = new ZLinkStreamPendingRequests();
        var writes = new AtomicInteger();
        var pending =
                chain.enqueueRequestDeferred(
                        () -> {
                            writes.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        },
                        error -> requests.fail(8L, error),
                        owned ->
                                requests.add(
                                        8L,
                                        "Echo",
                                        owned,
                                        (reply, complete) -> complete.getAsBoolean(),
                                        (error, complete) -> complete.getAsBoolean()));
        pending.whenComplete((reply, error) -> first.complete(null));
        assertTrue(pending.cancel(false));
        assertTrue(pending.isCancelled());
        assertEquals(0, writes.get());
        assertFalse(requests.fail(8L, new IllegalStateException("late")));
    }

    @Test
    void scheduledTimeoutRemovesRequestBeforeCompletionCallback() {
        var chain = new ZLinkStreamSendChain();
        var first = new CompletableFuture<Void>();
        chain.enqueue(() -> first);
        var requests = new ZLinkStreamPendingRequests();
        var writes = new AtomicInteger();
        var scheduler = java.util.concurrent.Executors.newSingleThreadScheduledExecutor();
        try {
            var pending =
                    chain.enqueueRequestDeferred(
                            () -> {
                                writes.incrementAndGet();
                                return CompletableFuture.completedFuture(null);
                            },
                            error -> requests.fail(9L, error),
                            owned ->
                                    requests.add(
                                            9L,
                                            "Echo",
                                            owned,
                                            (reply, complete) -> complete.getAsBoolean(),
                                            (error, complete) -> complete.getAsBoolean()));
            var callback = pending.whenComplete((reply, error) -> first.complete(null));
            requests.startTimeout(9L, java.time.Duration.ZERO, scheduler);
            assertThrows(CompletionException.class, callback::join);
            assertEquals(0, writes.get());
        } finally {
            scheduler.shutdownNow();
        }
    }

    @Test
    void replySnapshotPrecedesUserClosingPayload() {
        var requests = new ZLinkStreamPendingRequests();
        var pending = new CompletableFuture<ZLinkStreamEncodedPayload>();
        var snapshot = new java.util.concurrent.atomic.AtomicReference<byte[]>();
        requests.add(
                10L,
                "Echo",
                pending,
                (reply, complete) -> {
                    byte[] bytes = reply.payload().toByteArray();
                    boolean completed = complete.getAsBoolean();
                    if (completed) snapshot.set(bytes);
                    return completed;
                },
                (error, complete) -> complete.getAsBoolean());
        pending.whenComplete((reply, error) -> reply.payload().close());
        requests.complete(
                10L,
                () ->
                        new ZLinkStreamEncodedPayload(
                                "",
                                systems.zlink.contracts.messaging.Message.from("reply"),
                                Map.of()));
        assertArrayEquals(
                "reply".getBytes(java.nio.charset.StandardCharsets.UTF_8), snapshot.get());
    }

    @Test
    void snapshotFailureTerminatesPending() {
        var requests = new ZLinkStreamPendingRequests();
        var pending = new CompletableFuture<ZLinkStreamEncodedPayload>();
        var failure = new IllegalStateException("snapshot");
        var message = systems.zlink.contracts.messaging.Message.from("reply");
        requests.add(
                11L,
                "Echo",
                pending,
                (reply, complete) -> {
                    throw failure;
                },
                (error, complete) -> complete.getAsBoolean());
        requests.complete(11L, () -> new ZLinkStreamEncodedPayload("", message, Map.of()));
        assertSame(failure, assertThrows(CompletionException.class, pending::join).getCause());
        assertFalse(requests.fail(11L, new IllegalStateException("late failure")));
    }
}
