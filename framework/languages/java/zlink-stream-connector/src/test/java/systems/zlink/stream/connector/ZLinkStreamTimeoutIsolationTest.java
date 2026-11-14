package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;

import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
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
    void scheduledFutureKeepsJdkQueueCancellationAndCompletesOnHandoff() throws Exception {
        var timers = ZLinkStreamExecutionResources.timeouts();
        var scheduler = (ScheduledThreadPoolExecutor) timers;
        var cancelled = timers.schedule(() -> fail("cancelled deadline ran"), 1, TimeUnit.DAYS);
        try {
            assertEquals(
                    "java.util.concurrent.ScheduledThreadPoolExecutor$ScheduledFutureTask",
                    cancelled.getClass().getName(),
                    "the queue must retain the JDK future and its heap index");
            assertTrue(scheduler.getQueue().contains(cancelled));
        } finally {
            assertTrue(cancelled.cancel(false));
        }
        assertTrue(cancelled.isCancelled());
        assertFalse(
                scheduler.getQueue().contains(cancelled), "cancel must remove the queued future");
        var entered = new CountDownLatch(1);
        var release = new CountDownLatch(1);
        var exited = new CountDownLatch(1);
        ScheduledFuture<?> running =
                timers.schedule(
                        () -> {
                            entered.countDown();
                            try {
                                awaitRelease(release);
                            } finally {
                                exited.countDown();
                            }
                        },
                        0,
                        TimeUnit.MILLISECONDS);
        try {
            assertTrue(entered.await(5, TimeUnit.SECONDS));
            assertNull(running.get(5, TimeUnit.SECONDS));
            assertTrue(running.isDone(), "the future completes when the body is handed off");
            var independent = new CountDownLatch(1);
            timers.schedule(independent::countDown, 0, TimeUnit.MILLISECONDS);
            assertTrue(
                    independent.await(5, TimeUnit.SECONDS),
                    "a blocked body must not hold the clock");
            assertEquals(1, exited.getCount(), "the first body remains blocked");
        } finally {
            release.countDown();
            assertTrue(exited.await(5, TimeUnit.SECONDS));
        }

        var callableEntered = new CountDownLatch(1);
        var callableRelease = new CountDownLatch(1);
        var callableBody = new CompletableFuture<Integer>();
        var callable =
                timers.schedule(
                        () -> {
                            callableEntered.countDown();
                            awaitRelease(callableRelease);
                            callableBody.complete(42);
                            return 42;
                        },
                        0,
                        TimeUnit.MILLISECONDS);
        try {
            assertTrue(callableEntered.await(5, TimeUnit.SECONDS));
            assertNull(callable.get(5, TimeUnit.SECONDS));
            assertFalse(callableBody.isDone(), "the callable body remains blocked after handoff");
        } finally {
            callableRelease.countDown();
        }
        assertEquals(42, callableBody.get(5, TimeUnit.SECONDS));
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
