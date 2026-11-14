package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ExecutionException;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.ScheduledThreadPoolExecutor;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;

class ZLinkStreamTimeoutIsolationTest {
    private static final long GRACE_MILLIS = 200;

    @Test
    void blockingImmediateFailureCallbackDoesNotDelayOtherConnectorTimers() throws Exception {
        assertBlockingRequestIsIsolated(true);
    }

    @Test
    void blockingSynchronousContinuationDoesNotDelayOtherConnectorTimers() throws Exception {
        assertBlockingRequestIsIsolated(false);
    }

    private static void assertBlockingRequestIsIsolated(boolean blockCallback) throws Exception {
        try (var serverA = new TcpStreamConnectorTestServer();
                var serverB = new TcpStreamConnectorTestServer()) {
            var a = ZLinkStreamConnectorFactory.create(options(serverA, false));
            var b = ZLinkStreamConnectorFactory.create(options(serverB, true));
            var entered = new CountDownLatch(1);
            var release = new CountDownLatch(1);
            var exited = new CountDownLatch(1);
            Runnable block =
                    () -> {
                        entered.countDown();
                        try {
                            awaitRelease(release);
                        } finally {
                            exited.countDown();
                        }
                    };
            try {
                ConnectorTestAwait.await(a.connect());
                if (blockCallback) {
                    a.onReplyReceived(
                            context -> {
                                assertEquals(
                                        ZLinkStreamErrorCode.REQUEST_TIMEOUT,
                                        context.error().code());
                                block.run();
                            });
                }
                var pendingA = request(a, Duration.ofMillis(100));
                if (!blockCallback) pendingA.whenComplete((reply, error) -> block.run());
                assertTrue(
                        entered.await(5, TimeUnit.SECONDS), "A must enter its blocking callback");

                var heartbeat = new CompletableFuture<Long>();
                b.onDisconnected(
                        event -> {
                            if (event.closeReason() == ZLinkStreamCloseReason.HEARTBEAT_TIMEOUT)
                                heartbeat.complete(System.nanoTime());
                            return CompletableFuture.completedFuture(null);
                        });
                long connectedAt = System.nanoTime();
                ConnectorTestAwait.await(b.connect());
                long requestAt = System.nanoTime();
                var pendingB = request(b, Duration.ofMillis(100));
                var timedOut =
                        pendingB.handle(
                                (reply, error) -> {
                                    assertInstanceOf(ZLinkStreamException.class, error);
                                    assertEquals(
                                            ZLinkStreamErrorCode.REQUEST_TIMEOUT,
                                            ((ZLinkStreamException) error).errorCode());
                                    return System.nanoTime();
                                });
                assertAll(
                        () -> assertBefore(timedOut, requestAt, 100, "B request timeout"),
                        () -> assertBefore(heartbeat, connectedAt, 450, "B heartbeat timeout"));
                assertEquals(1, release.getCount(), "A remains blocked during both B deadlines");
            } finally {
                release.countDown();
                if (entered.getCount() == 0) assertTrue(exited.await(5, TimeUnit.SECONDS));
                ConnectorTestAwait.await(a.close());
                ConnectorTestAwait.await(b.close());
            }
        }
    }

    @Test
    void blockedHeartbeatBodyNeverOverlapsItsNextTick() throws Exception {
        try (var server = new TcpStreamConnectorTestServer()) {
            var configuration = ZLinkStreamConnectorConfiguration.from(options(server, true));
            var entered = new CountDownLatch(1);
            var release = new CountDownLatch(1);
            var exited = new CountDownLatch(1);
            var active = new AtomicInteger();
            var maximum = new AtomicInteger();
            var calls = new AtomicInteger();
            // The silent server sends no inbound frames. Only the lifecycle's real heartbeat
            // sender is replaced, so the body blocks before returning its completion stage.
            var lifecycle =
                    new ZLinkStreamConnectionLifecycle(
                            configuration,
                            ZLinkStreamExecutionResources.timeouts(),
                            new ZLinkStreamDispatchQueue(),
                            new ZLinkStreamPendingRequests(),
                            null,
                            ignored -> {},
                            () -> {},
                            ignored -> {},
                            () -> {},
                            name -> {
                                assertEquals(
                                        ZLinkStreamReceiveDispatcher.HEARTBEAT_PING_NAME, name);
                                maximum.accumulateAndGet(active.incrementAndGet(), Math::max);
                                calls.incrementAndGet();
                                entered.countDown();
                                try {
                                    awaitRelease(release);
                                    return CompletableFuture.completedFuture(null);
                                } finally {
                                    active.decrementAndGet();
                                    exited.countDown();
                                }
                            },
                            () -> {});
            try {
                lifecycle.connect().toCompletableFuture().get(5, TimeUnit.SECONDS);
                assertTrue(entered.await(5, TimeUnit.SECONDS));
                // Wait for four more periods using an independent clock: a broken shared
                // timer must not make this observation wait behind the blocked heartbeat.
                var clock = new ScheduledThreadPoolExecutor(1);
                try {
                    clock.schedule(() -> {}, 200, TimeUnit.MILLISECONDS).get(5, TimeUnit.SECONDS);
                } finally {
                    clock.shutdownNow();
                }
                assertEquals(1, calls.get(), "unfinished heartbeat must not start another body");
                assertEquals(1, maximum.get());
                lifecycle.close().toCompletableFuture().get(5, TimeUnit.SECONDS);
            } finally {
                release.countDown();
                if (entered.getCount() == 0) assertTrue(exited.await(5, TimeUnit.SECONDS));
                lifecycle.close().toCompletableFuture().get(5, TimeUnit.SECONDS);
            }
        }
    }

    @Test
    void scheduledFutureTracksBodyCompletionExceptionsAndCancellation() throws Exception {
        var timers = ZLinkStreamExecutionResources.timeouts();
        var scheduler = (ScheduledThreadPoolExecutor) timers;
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        ScheduledFuture<?> running =
                timers.schedule(
                        () -> {
                            entered.countDown();
                            awaitRelease(release);
                        },
                        0,
                        TimeUnit.MILLISECONDS);
        try {
            assertTrue(entered.await(5, TimeUnit.SECONDS));
            assertFalse(running.isDone(), "handoff alone must not complete the future");
        } finally {
            release.countDown();
        }
        running.get(5, TimeUnit.SECONDS);
        assertTrue(running.isDone());
        var callable = timers.schedule(() -> 42, 0, TimeUnit.MILLISECONDS);
        assertEquals(42, callable.get(5, TimeUnit.SECONDS));
        var failure = new IllegalStateException("periodic body failed");
        var periodic =
                timers.scheduleAtFixedRate(
                        () -> {
                            throw failure;
                        },
                        0,
                        10,
                        TimeUnit.MILLISECONDS);
        assertSame(
                failure,
                assertThrows(ExecutionException.class, () -> periodic.get(5, TimeUnit.SECONDS))
                        .getCause());
        var cancelled = timers.schedule(() -> fail("cancelled deadline ran"), 1, TimeUnit.DAYS);
        assertTrue(scheduler.getQueue().contains(cancelled));
        assertTrue(cancelled.cancel(false));
        assertTrue(cancelled.isCancelled());
        assertFalse(
                scheduler.getQueue().contains(cancelled), "cancel must remove the queued wrapper");
    }

    private static void assertBefore(
            CompletableFuture<Long> event, long start, long scheduledMillis, String name)
            throws Exception {
        long deadline = start + TimeUnit.MILLISECONDS.toNanos(scheduledMillis + GRACE_MILLIS);
        long observed;
        try {
            observed = event.get(Math.max(1, deadline - System.nanoTime()), TimeUnit.NANOSECONDS);
        } catch (TimeoutException ex) {
            throw new AssertionError(
                    name
                            + " missing at scheduled="
                            + scheduledMillis
                            + "ms + grace="
                            + GRACE_MILLIS
                            + "ms while A is blocked",
                    ex);
        }
        long elapsed = TimeUnit.NANOSECONDS.toMillis(observed - start);
        System.out.printf(
                "%s: scheduled=%dms observed=%dms grace=%dms%n",
                name, scheduledMillis, elapsed, GRACE_MILLIS);
        assertTrue(observed <= deadline, name + " exceeded deadline: " + elapsed + "ms");
    }

    private static CompletableFuture<ZLinkStreamEncodedPayload> request(
            ZLinkStreamConnector connector, Duration timeout) {
        return connector
                .request(
                        new ZLinkStreamEncodedPayload("NoReply", Message.from("request"), Map.of()))
                .timeout(timeout)
                .submit()
                .toCompletableFuture();
    }

    private static ZLinkStreamConnectorOptions options(
            TcpStreamConnectorTestServer server, boolean heartbeat) {
        return new ZLinkStreamConnectorOptions(
                server.endpoint(),
                ZLinkStreamDispatchMode.IMMEDIATE,
                Duration.ofSeconds(1),
                1,
                Duration.ofSeconds(1),
                64 * 1024,
                heartbeat,
                Duration.ofMillis(50),
                Duration.ofMillis(400),
                false,
                Duration.ofMillis(25),
                Duration.ofMillis(250),
                2.0);
    }

    private static void awaitRelease(CountDownLatch release) {
        try {
            release.await();
        } catch (InterruptedException ex) {
            Thread.currentThread().interrupt();
            throw new AssertionError(ex);
        }
    }
}
