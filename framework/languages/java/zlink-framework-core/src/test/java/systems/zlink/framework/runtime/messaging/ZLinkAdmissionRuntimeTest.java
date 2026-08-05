package systems.zlink.framework.runtime.host;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertNull;

import java.time.Duration;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CancellationException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.CountDownLatch;
import java.util.function.Consumer;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

final class ZLinkAdmissionRuntimeTest {
    @Test
    void duplicateGuardIsOwnedByOneCallAndReturnsAnExceptionalStage() {
        java.util.concurrent.atomic.AtomicBoolean first = new java.util.concurrent.atomic.AtomicBoolean();
        java.util.concurrent.atomic.AtomicBoolean second = new java.util.concurrent.atomic.AtomicBoolean();

        assertNull(ZLinkOneWayAdmission.begin(first));
        assertNull(ZLinkOneWayAdmission.begin(second));
        CompletionException duplicate = assertThrows(
            CompletionException.class,
            () -> ZLinkOneWayAdmission.begin(first)
                .toCompletableFuture()
                .join());
        assertEquals(
            ZLinkFrameworkErrorKind.INVALID_OPERATION,
            ((ZLinkFrameworkException) duplicate.getCause()).kind());
    }

    @Test
    void firstAttemptIsImmediateAndOnlyTheExactReadyDestinationRetries() {
        FakeSource source = new FakeSource(Duration.ofSeconds(1));
        AtomicInteger attempts = new AtomicInteger();
        AtomicInteger cleanups = new AtomicInteger();
        ZLinkBackendAdmissionKey target = ZLinkBackendAdmissionKey.channel("orders");

        var result = submit(
            source,
            target,
            () -> attempts.incrementAndGet() == 2,
            cleanups::incrementAndGet).toCompletableFuture();

        assertEquals(1, attempts.get());
        assertFalse(result.isDone());
        source.ready(ZLinkBackendAdmissionKey.channel("other"));
        assertEquals(1, attempts.get());
        source.ready(target);
        assertEquals(2, attempts.get());
        assertEquals(0, systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
        assertEquals(1, cleanups.get());
    }

    @Test
    void readyRacingBetweenFirstFailureAndEnqueueIsPreservedAsOneCredit() {
        FakeSource source = new FakeSource(Duration.ofSeconds(1));
        AtomicInteger attempts = new AtomicInteger();
        ZLinkBackendAdmissionKey key = ZLinkBackendAdmissionKey.channel("orders");

        var result = submit(
            source,
            key,
            () -> {
                int current = attempts.incrementAndGet();
                if (current == 1) {
                    source.ready(key);
                    return false;
                }
                return true;
            },
            () -> { }).toCompletableFuture();

        assertEquals(2, attempts.get());
        assertEquals(0, systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
    }

    @Test
    void oneReadySignalRetriesOnlyOnePendingSubmission() {
        FakeSource source = new FakeSource(Duration.ofSeconds(1));
        AtomicInteger attempts = new AtomicInteger();
        ZLinkBackendAdmissionKey key = ZLinkBackendAdmissionKey.socket();
        var first = submit(
            source, key, () -> attempts.incrementAndGet() >= 3, () -> { })
            .toCompletableFuture();
        var second = submit(
            source, key, () -> attempts.incrementAndGet() >= 4, () -> { })
            .toCompletableFuture();

        assertEquals(2, attempts.get());
        source.ready(key);
        assertEquals(3, attempts.get());
        assertTrue(first.isDone() ^ second.isDone());
        source.ready(key);
        assertEquals(4, attempts.get());
        assertTrue(first.isDone());
        assertTrue(second.isDone());
    }

    @Test
    void sourcePendingCapacityWaitsUntilAReservedSlotIsReleased() {
        FakeSource source = new FakeSource(Duration.ofSeconds(30), 1);
        AtomicInteger attempts = new AtomicInteger();
        AtomicInteger cleanups = new AtomicInteger();
        ZLinkBackendAdmissionKey key = ZLinkBackendAdmissionKey.socket();

        var first = submit(
            source, key, () -> {
                attempts.incrementAndGet();
                return false;
            }, cleanups::incrementAndGet).toCompletableFuture();
        var second = submit(
            source, key, () -> {
                return attempts.incrementAndGet() == 3;
            }, cleanups::incrementAndGet).toCompletableFuture();

        assertFalse(first.isDone());
        assertFalse(second.isDone());
        assertEquals(2, attempts.get());
        assertTrue(first.cancel(false));
        source.ready(key);
        assertEquals(
            0,
            systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(second));
        assertEquals(2, cleanups.get());
    }

    @Test
    void capacityWaiterOverflowDropsPayloadImmediatelyAndSourceRecovers() {
        FakeSource source = new FakeSource(Duration.ofSeconds(30), 1);
        AtomicInteger firstAttempts = new AtomicInteger();
        AtomicInteger secondAttempts = new AtomicInteger();
        AtomicInteger overflowAttempts = new AtomicInteger();
        AtomicInteger cleanups = new AtomicInteger();
        ZLinkBackendAdmissionKey key = ZLinkBackendAdmissionKey.socket();

        var first = submit(
            source, key, () -> firstAttempts.incrementAndGet() == 2,
            cleanups::incrementAndGet).toCompletableFuture();
        var second = submit(
            source, key, () -> secondAttempts.incrementAndGet() == 2,
            cleanups::incrementAndGet).toCompletableFuture();
        var overflow = submit(
            source, key, () -> {
                overflowAttempts.incrementAndGet();
                return false;
            }, cleanups::incrementAndGet).toCompletableFuture();

        CompletionException rejected = assertThrows(
            CompletionException.class, overflow::join);
        assertEquals(
            ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED,
            ((ZLinkFrameworkException) rejected.getCause()).kind());
        assertEquals(1, overflowAttempts.get());
        assertEquals(1, cleanups.get());

        source.ready(key);
        first.join();
        source.ready(key);
        second.join();
        assertEquals(2, firstAttempts.get());
        assertEquals(2, secondAttempts.get());
        assertEquals(3, cleanups.get());
    }

    @Test
    void cancellationRemovesPendingPayloadAndPreventsLateAdmission() {
        FakeSource source = new FakeSource(Duration.ofSeconds(1));
        AtomicInteger attempts = new AtomicInteger();
        AtomicInteger cleanups = new AtomicInteger();
        ZLinkBackendAdmissionKey key = ZLinkBackendAdmissionKey.socket();
        var result = submit(
            source, key, () -> {
                attempts.incrementAndGet();
                return false;
            }, cleanups::incrementAndGet).toCompletableFuture();

        assertTrue(result.cancel(false));
        source.ready(key);
        assertEquals(1, attempts.get());
        assertEquals(1, cleanups.get());
    }

    @Test
    void familyDeadlineCompletesWithTimedOutAndCleansOnce() throws Exception {
        FakeSource source = new FakeSource(Duration.ofNanos(1));
        AtomicInteger cleanups = new AtomicInteger();
        var result = submit(
            source,
            ZLinkBackendAdmissionKey.socket(),
            () -> false,
            cleanups::incrementAndGet).toCompletableFuture();

        assertEquals(
            2,
            systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
        assertEquals(1, cleanups.get());
        assertEquals(1, ZLinkAdmissionRuntime.normalizedTimeoutMillis(Duration.ofNanos(1)));
        assertEquals(1, ZLinkAdmissionRuntime.normalizedTimeoutMillis(Duration.ofNanos(999_999)));
        assertEquals(2, ZLinkAdmissionRuntime.normalizedTimeoutMillis(Duration.ofNanos(1_000_001)));
        assertEquals(Integer.MAX_VALUE,
            ZLinkAdmissionRuntime.normalizedTimeoutMillis(Duration.ofMillis(Integer.MAX_VALUE)));
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkAdmissionRuntime.normalizedTimeoutMillis(Duration.ZERO));
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkAdmissionRuntime.normalizedTimeoutMillis(Duration.ofMillis(-1)));
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkAdmissionRuntime.normalizedTimeoutMillis(Duration.ofDays(365)));
        assertThrows(IllegalArgumentException.class,
            () -> ZLinkAdmissionRuntime.normalizedTimeoutMillis(
                Duration.ofMillis((long) Integer.MAX_VALUE + 1L)));
    }

    @Test
    void acceptedCommitCannotBeOverwrittenByCancellation() {
        FakeSource source = new FakeSource(Duration.ofSeconds(1));
        var result = submit(
            source,
            ZLinkBackendAdmissionKey.socket(),
            () -> true,
            () -> { }).toCompletableFuture();

        assertEquals(0, systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
        assertFalse(result.cancel(false));
        assertEquals(0, systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
    }

    @Test
    void payloadCleanupFailureDoesNotOverwriteAcceptedCommit() {
        FakeSource source = new FakeSource(Duration.ofSeconds(1));

        var result = submit(
            source,
            ZLinkBackendAdmissionKey.socket(),
            () -> true,
            () -> { throw new IllegalStateException("cleanup failed"); })
            .toCompletableFuture();

        assertEquals(0, systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
    }

    @Test
    void sourceShutdownTerminatesPendingAndRunsReentrantCleanupOutsideLock() {
        FakeSource source = new FakeSource(Duration.ofSeconds(30));
        AtomicInteger cleanups = new AtomicInteger();
        var result = submit(
            source,
            ZLinkBackendAdmissionKey.socket(),
            () -> false,
            () -> {
                cleanups.incrementAndGet();
                source.close();
            }).toCompletableFuture();

        source.close();

        assertEquals(5, systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
        assertEquals(1, cleanups.get());
    }

    @Test
    void disposalAndReadyRaceLeaveOneTerminalAndOneCleanup() throws Exception {
        for (int iteration = 0; iteration < 100; iteration++) {
            FakeSource source = new FakeSource(Duration.ofSeconds(30), 1);
            AtomicInteger attempts = new AtomicInteger();
            AtomicInteger cleanups = new AtomicInteger();
            ZLinkBackendAdmissionKey key = ZLinkBackendAdmissionKey.socket();
            var result = submit(
                source,
                key,
                () -> attempts.incrementAndGet() == 2,
                cleanups::incrementAndGet).toCompletableFuture();
            CountDownLatch start = new CountDownLatch(1);
            Thread ready = Thread.ofVirtual().start(() -> {
                await(start);
                source.ready(key);
            });
            Thread shutdown = Thread.ofVirtual().start(() -> {
                await(start);
                source.close();
            });

            start.countDown();
            ready.join();
            shutdown.join();

            Integer terminal = systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result);
            assertTrue(
                terminal == 0
                    || terminal == 5);
            assertTrue(attempts.get() == 1 || attempts.get() == 2);
            assertEquals(1, cleanups.get());
            source.close();
            source.ready(key);
            assertEquals(1, cleanups.get());
        }
    }

    @Test
    void actorGenerationReadySignalCannotWakeAnotherGeneration() {
        FakeSource source = new FakeSource(Duration.ofSeconds(1));
        AtomicInteger attempts = new AtomicInteger();
        ZLinkBackendAdmissionKey generationSeven = ZLinkBackendAdmissionKey.actor(
            systems.zlink.contracts.core.RoutingId.from("node-a"), "actor-a", 7L);
        ZLinkBackendAdmissionKey generationEight = ZLinkBackendAdmissionKey.actor(
            systems.zlink.contracts.core.RoutingId.from("node-a"), "actor-a", 8L);
        var result = submit(
            source,
            generationSeven,
            () -> attempts.incrementAndGet() == 2,
            () -> { }).toCompletableFuture();

        source.ready(generationEight);
        assertEquals(1, attempts.get());
        assertFalse(result.isDone());

        source.ready(generationSeven);
        assertEquals(0, systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result));
        assertEquals(2, attempts.get());
    }

    @Test
    void nodeReadySignalRetriesEachPendingRouteForThatNode() {
        FakeSource source = new FakeSource(Duration.ofSeconds(1));
        var nodeA = systems.zlink.contracts.core.RoutingId.from("node-a");
        var nodeB = systems.zlink.contracts.core.RoutingId.from("node-b");
        AtomicInteger actorAAttempts = new AtomicInteger();
        AtomicInteger actorBAttempts = new AtomicInteger();
        AtomicInteger actorOtherAttempts = new AtomicInteger();

        var actorA = submit(
            source,
            ZLinkBackendAdmissionKey.actor(nodeA, "actor-a", 1),
            () -> actorAAttempts.incrementAndGet() == 2,
            () -> { }).toCompletableFuture();
        var actorB = submit(
            source,
            ZLinkBackendAdmissionKey.actor(nodeA, "actor-b", 1),
            () -> actorBAttempts.incrementAndGet() == 2,
            () -> { }).toCompletableFuture();
        var actorOther = submit(
            source,
            ZLinkBackendAdmissionKey.actor(nodeB, "actor-c", 1),
            () -> actorOtherAttempts.incrementAndGet() == 2,
            () -> { }).toCompletableFuture();

        source.ready(ZLinkBackendAdmissionKey.node(nodeA));

        assertEquals(0, systems.zlink.framework.runtime.messaging
            .OneWayTestStatus.status(actorA));
        assertEquals(0, systems.zlink.framework.runtime.messaging
            .OneWayTestStatus.status(actorB));
        assertFalse(actorOther.isDone());
        assertEquals(2, actorAAttempts.get());
        assertEquals(2, actorBAttempts.get());
        assertEquals(1, actorOtherAttempts.get());

        actorOther.cancel(false);
    }

    @Test
    void nodeReadyCreditWakesAnActorSubmittedAfterTheReadinessSignal() {
        FakeSource source = new FakeSource(Duration.ofSeconds(1));
        var node = systems.zlink.contracts.core.RoutingId.from("node-a");
        AtomicInteger attempts = new AtomicInteger();

        var result = submit(
            source,
            ZLinkBackendAdmissionKey.actor(node, "actor-a", 1),
            () -> {
                int current = attempts.incrementAndGet();
                if (current == 1) {
                    source.ready(ZLinkBackendAdmissionKey.node(node));
                    return false;
                }
                return true;
            },
            () -> { }).toCompletableFuture();

        assertEquals(0, systems.zlink.framework.runtime.messaging
            .OneWayTestStatus.status(result));
        assertEquals(2, attempts.get());
    }

    @Test
    void timeoutTerminalIsNotReplayedWhenTheRouteRecovers() throws Exception {
        FakeSource source = new FakeSource(Duration.ofNanos(1));
        AtomicInteger oldAttempts = new AtomicInteger();
        AtomicInteger newAttempts = new AtomicInteger();
        ZLinkBackendAdmissionKey key = ZLinkBackendAdmissionKey.node(
            systems.zlink.contracts.core.RoutingId.from("node-a"));
        var oldOperation = submit(
            source,
            key,
            () -> {
                oldAttempts.incrementAndGet();
                return false;
            },
            () -> { }).toCompletableFuture();

        assertEquals(2,
            systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(oldOperation));
        source.ready(key);
        var newOperation = submit(
            source,
            key,
            () -> {
                newAttempts.incrementAndGet();
                return true;
            },
            () -> { }).toCompletableFuture();

        assertEquals(0, systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(newOperation));
        assertEquals(1, oldAttempts.get());
        assertEquals(1, newAttempts.get());
    }

    @Test
    void timeoutCancellationAndShutdownRaceHasOneWinnerAndOneCleanup() throws Exception {
        for (int iteration = 0; iteration < 100; iteration++) {
            FakeSource source = new FakeSource(Duration.ofMillis(1), 1);
            AtomicInteger attempts = new AtomicInteger();
            AtomicInteger cleanups = new AtomicInteger();
            var result = submit(
                source,
                ZLinkBackendAdmissionKey.socket(),
                () -> {
                    attempts.incrementAndGet();
                    return false;
                },
                cleanups::incrementAndGet).toCompletableFuture();
            CountDownLatch start = new CountDownLatch(1);
            Thread cancellation = Thread.ofVirtual().start(() -> {
                await(start);
                result.cancel(false);
            });
            Thread shutdown = Thread.ofVirtual().start(() -> {
                await(start);
                source.close();
            });

            start.countDown();
            cancellation.join();
            shutdown.join();

            if (!result.isCancelled()) {
                Integer status = systems.zlink.framework.runtime.messaging.OneWayTestStatus.status(result);
                assertTrue(status == 2
                    || status == 5);
            } else {
                assertThrows(CancellationException.class, result::join);
            }
            assertEquals(1, attempts.get());
            assertEquals(1, cleanups.get());
            source.close();
            assertFalse(result.cancel(false));
            assertEquals(1, cleanups.get());
        }
    }

    private static void await(CountDownLatch latch) {
        try {
            latch.await();
        } catch (InterruptedException error) {
            Thread.currentThread().interrupt();
            throw new AssertionError(error);
        }
    }

    private static java.util.concurrent.CompletionStage<Void> submit(
        FakeSource source,
        ZLinkBackendAdmissionKey key,
        java.util.function.Supplier<Boolean> attempt,
        Runnable cleanup) {
        return ZLinkAdmissionRuntime.submit(
            source,
            key,
            attempt,
            cleanup,
            ignored -> source,
            ignored -> source.admissionTimeout(),
            ignored -> source.admissionPendingCapacity(),
            (ignored, handler) -> source.setAdmissionReadyHandler(handler),
            (ignored, handler) -> source.setAdmissionShutdownHandler(handler));
    }

    private static final class FakeSource implements ZLinkBackendObject {
        private final Duration timeout;
        private final int pendingCapacity;
        private Consumer<ZLinkBackendAdmissionKey> ready = ignored -> { };
        private Runnable shutdown = () -> { };

        FakeSource(Duration timeout) {
            this(timeout, 4096);
        }

        FakeSource(Duration timeout, int pendingCapacity) {
            this.timeout = timeout;
            this.pendingCapacity = pendingCapacity;
        }

        @Override public String name() { return "fake"; }
        @Override public void close() { shutdown.run(); }
        public Duration admissionTimeout() { return timeout; }
        public int admissionPendingCapacity() { return pendingCapacity; }
        public void setAdmissionReadyHandler(
            Consumer<ZLinkBackendAdmissionKey> handler) {
            ready = handler;
        }
        public void setAdmissionShutdownHandler(Runnable handler) {
            shutdown = handler;
        }

        void ready(ZLinkBackendAdmissionKey key) {
            ready.accept(key);
        }
    }

}
