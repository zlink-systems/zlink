package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import java.io.IOException;
import java.net.URI;
import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicLong;

final class ZLinkStreamR3RegressionTest {
    @Test
    void transportCloseFailureBecomesDisconnectedEvent() {
        List<ZLinkStreamError> errors = new ArrayList<>();
        var configuration =
                ZLinkStreamConnectorConfiguration.from(
                        ZLinkStreamConnectorOptions.createDefault(URI.create("tcp://127.0.0.1:1")));
        var lifecycle =
                new ZLinkStreamConnectionLifecycle(
                        configuration,
                        null,
                        null,
                        null,
                        null,
                        errors::add,
                        null,
                        null,
                        null,
                        null,
                        null);
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
        var requests = ZLinkStreamPendingRequestsTest.pendingRequests();
        var pending = enqueueRequest(requests, chain, 7L, Duration.ofSeconds(30), writes);
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
        var requests = ZLinkStreamPendingRequestsTest.pendingRequests();
        var writes = new AtomicInteger();
        var pending = enqueueRequest(requests, chain, 8L, Duration.ofSeconds(30), writes);
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
        var writes = new AtomicInteger();
        var scheduler = java.util.concurrent.Executors.newSingleThreadScheduledExecutor();
        var time = new AtomicLong();
        var release = new CompletableFuture<Void>();
        try {
            scheduler.execute(release::join);
            var requests = new ZLinkStreamPendingRequests(scheduler, time::get, () -> 0);
            var pending = enqueueRequest(requests, chain, 9L, Duration.ofNanos(1), writes);
            var callback = pending.whenComplete((reply, error) -> first.complete(null));
            time.set(1);
            release.complete(null);
            assertThrows(CompletionException.class, callback::join);
            assertEquals(0, writes.get());
        } finally {
            release.complete(null);
            scheduler.shutdownNow();
        }
    }

    private static CompletableFuture<ZLinkStreamEncodedPayload> enqueueRequest(
            ZLinkStreamPendingRequests requests,
            ZLinkStreamSendChain chain,
            long sequence,
            Duration timeout,
            AtomicInteger writes) {
        var request =
                requests.start(
                        null,
                        "Echo",
                        timeout,
                        null,
                        (reply, complete) -> complete.getAsBoolean(),
                        (error, complete) -> complete.getAsBoolean(),
                        ignored -> {},
                        ignored -> {});
        return request.enqueueAttempt(
                sequence,
                ignored ->
                        chain.enqueueDeferred(
                                () -> {
                                    writes.incrementAndGet();
                                    return CompletableFuture.completedFuture(null);
                                },
                                error -> requests.fail(sequence, error)));
    }

    @Test
    void replySnapshotPrecedesUserClosingPayload() {
        var requests = ZLinkStreamPendingRequestsTest.pendingRequests();
        var snapshot = new java.util.concurrent.atomic.AtomicReference<byte[]>();
        var pending =
                ZLinkStreamPendingRequestsTest.addAttempt(
                        requests,
                        10L,
                        "Echo",
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
        var requests = ZLinkStreamPendingRequestsTest.pendingRequests();
        var failure = new IllegalStateException("snapshot");
        var message = systems.zlink.contracts.messaging.Message.from("reply");
        var pending =
                ZLinkStreamPendingRequestsTest.addAttempt(
                        requests,
                        11L,
                        "Echo",
                        (reply, complete) -> {
                            throw failure;
                        },
                        (error, complete) -> complete.getAsBoolean());
        requests.complete(11L, () -> new ZLinkStreamEncodedPayload("", message, Map.of()));
        assertSame(failure, assertThrows(CompletionException.class, pending::join).getCause());
        assertFalse(requests.fail(11L, new IllegalStateException("late failure")));
    }
}
