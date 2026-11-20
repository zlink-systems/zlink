package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.RepetitionInfo;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.time.Clock;
import java.time.Duration;
import java.time.Instant;
import java.time.ZoneOffset;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkLocationRecoveryShutdownTest {
    @RepeatedTest(40)
    void stopMustJoinPublicationWhoseListenerHasNotReturned(RepetitionInfo repetition)
            throws Exception {
        exercise(
                true,
                repetition.getCurrentRepetition() % 2 == 0,
                repetition.getCurrentRepetition() % 4 < 2);
    }

    @Test
    void completedPublicationControl() throws Exception {
        exercise(false, false, false);
    }

    @RepeatedTest(40)
    void recoveryTerminalCallbackCanImmediatelyStartTheNextRenewal() throws Exception {
        var clock = new java.util.concurrent.atomic.AtomicLong(1L);
        var scheduler = new ZLinkLocationFirstHeartbeatTest.RecordingScheduler();
        var storeWrite = new CompletableFuture<Void>();
        var publications = new AtomicInteger();
        var timeout = Duration.ofMillis(50);
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(
                                new ZLinkInMemoryLocationStore(
                                        Clock.fixed(Instant.EPOCH, ZoneOffset.UTC))),
                        "recovery-terminal",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        timeout,
                        Duration.ZERO,
                        clock::get,
                        scheduler,
                        new ZLinkStateLane(Runnable::run))) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            runtime.setOwnerLeaseRecoveryListener(
                    ignored ->
                            publications.incrementAndGet() == 1
                                    ? runtime.writeDescriptor(() -> storeWrite)
                                    : CompletableFuture.completedFuture(null));
            var scheduled = scheduler.nextTask();
            var publication = runtime.publishWithStartupOwnerLease();
            // A terminal observer may immediately submit the next operation. The lane remains
            // available while this observer waits for that operation's completion.
            var next =
                    publication.handle(
                            (ignored, failure) ->
                                    runtime.renewOwnerLeaseOnce().toCompletableFuture().join());
            clock.addAndGet(timeout.toNanos());
            scheduled.get(1, TimeUnit.SECONDS).run();
            assertTrue(next.toCompletableFuture().get(1, TimeUnit.SECONDS));
            assertEquals(2, publications.get());
        } finally {
            storeWrite.complete(null);
        }
    }

    @RepeatedTest(40)
    void completedRecoveryMustRepublishBeforeItsDrainAndStopMustJoinBoth() throws Exception {
        var firstPublication = new CompletableFuture<Void>();
        var callbackEntered = new CountDownLatch(1);
        var callbackRelease = new CountDownLatch(1);
        var publications = new AtomicInteger();
        try (var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(
                                new ZLinkInMemoryLocationStore(
                                        Clock.fixed(Instant.EPOCH, ZoneOffset.UTC))),
                        "recovery-before-drain",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(500),
                        Duration.ZERO,
                        () -> 0L,
                        new ZLinkLocationFirstHeartbeatTest.RecordingScheduler(),
                        new ZLinkStateLane(Runnable::run))) {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            runtime.setOwnerLeaseRecoveryListener(
                    ignored ->
                            publications.incrementAndGet() == 1
                                    ? firstPublication
                                    : CompletableFuture.completedFuture(null));
            runtime.publishWithStartupOwnerLease();
            var descriptor = runtime.writeDescriptor(() -> new CompletableFuture<Void>());
            var callback =
                    descriptor.handle(
                            (ignored, failure) -> {
                                var next =
                                        runtime.publishWithStartupOwnerLease()
                                                .toCompletableFuture();
                                callbackEntered.countDown();
                                await(callbackRelease);
                                next.join();
                                return null;
                            });
            // Recovery completion propagates failure to accepted descriptor writes before drain.
            // Keep that terminal callback active while the second recovery finishes.
            var publisher =
                    CompletableFuture.runAsync(
                            () ->
                                    firstPublication.completeExceptionally(
                                            new IllegalStateException("first recovery failed")));
            try {
                assertTrue(callbackEntered.await(5, TimeUnit.SECONDS));
                assertEquals(2, publications.get(), "completed recovery must not accept a join");
                var stop = runtime.stop().toCompletableFuture();
                assertThrows(
                        TimeoutException.class,
                        () -> stop.get(50, TimeUnit.MILLISECONDS),
                        "stop must retain the first recovery drain after replacement");
                callbackRelease.countDown();
                callback.toCompletableFuture().get(5, TimeUnit.SECONDS);
                publisher.get(5, TimeUnit.SECONDS);
                stop.get(500, TimeUnit.MILLISECONDS);
            } finally {
                callbackRelease.countDown();
                publisher.get(5, TimeUnit.SECONDS);
            }
        } finally {
            callbackRelease.countDown();
            firstPublication.complete(null);
        }
    }

    private void exercise(boolean holdListener, boolean lateFailure, boolean storeBeforeStop)
            throws Exception {
        var lane = new ZLinkStateLane();
        var laneEntered = new CountDownLatch(1);
        var laneRelease = new CountDownLatch(1);
        var listenerEntered = new CountDownLatch(1);
        var listenerRelease = new CountDownLatch(1);
        var descriptorWrite = new CompletableFuture<Void>();
        var descriptorTerminal = new CompletableFuture<Void>();
        var publications = new AtomicInteger();
        var runtime =
                new ZLinkLocationRuntime(
                        ZLinkRegisteredLocationStores.fromUnified(
                                new ZLinkInMemoryLocationStore(
                                        Clock.fixed(Instant.EPOCH, ZoneOffset.UTC))),
                        "dg4104",
                        Duration.ofSeconds(15),
                        Duration.ofSeconds(1),
                        Duration.ofMillis(500),
                        Duration.ZERO,
                        () -> 0L,
                        new ZLinkLocationFirstHeartbeatTest.RecordingScheduler(),
                        lane);
        try {
            runtime.start(RoutingId.from("node-a")).toCompletableFuture().join();
            runtime.setOwnerLeaseRecoveryListener(
                    ignored -> {
                        var pending =
                                runtime.writeDescriptor(() -> descriptorWrite)
                                        .thenRun(publications::incrementAndGet);
                        pending.whenComplete(
                                (ignoredResult, failure) -> descriptorTerminal.complete(null));
                        listenerEntered.countDown();
                        await(listenerRelease);
                        return pending;
                    });
            var laneTurn =
                    lane.runAsync(
                            () -> {
                                laneEntered.countDown();
                                await(laneRelease);
                            });
            assertTrue(laneEntered.await(5, TimeUnit.SECONDS));
            var publication = runtime.publishWithStartupOwnerLease().toCompletableFuture();
            laneRelease.countDown();
            laneTurn.toCompletableFuture().get(5, TimeUnit.SECONDS);
            assertTrue(listenerEntered.await(5, TimeUnit.SECONDS));
            if (!holdListener) {
                listenerRelease.countDown();
                descriptorWrite.complete(null);
                publication.get(5, TimeUnit.SECONDS);
            }
            if (holdListener && storeBeforeStop) {
                if (lateFailure)
                    descriptorWrite.completeExceptionally(
                            new IllegalStateException("late Store failure"));
                else descriptorWrite.complete(null);
            }
            if (holdListener && storeBeforeStop) descriptorTerminal.get(5, TimeUnit.SECONDS);
            var stop = runtime.stop().toCompletableFuture();
            if (holdListener) {
                try {
                    assertThrows(
                            TimeoutException.class,
                            () -> stop.get(50, TimeUnit.MILLISECONDS),
                            "stop must observe listener return before completing");
                } finally {
                    listenerRelease.countDown();
                }
            }
            stop.get(500, TimeUnit.MILLISECONDS);
            assertTrue(publication.isDone(), "stop cancels the recovery publication wait");
            if (holdListener) {
                assertTrue(publication.isCompletedExceptionally());
                if (lateFailure)
                    descriptorWrite.completeExceptionally(
                            new IllegalStateException("late Store failure"));
                else descriptorWrite.complete(null);
                assertEquals(
                        storeBeforeStop && !lateFailure ? 1 : 0,
                        publications.get(),
                        "late Store completion must not publish after stop");
            } else assertEquals(1, publications.get());
        } finally {
            laneRelease.countDown();
            listenerRelease.countDown();
            descriptorWrite.complete(null);
            runtime.close();
        }
    }

    private static void await(CountDownLatch latch) {
        try {
            if (!latch.await(5, TimeUnit.SECONDS))
                throw new AssertionError("diagnostic latch timeout");
        } catch (InterruptedException failure) {
            Thread.currentThread().interrupt();
            throw new AssertionError(failure);
        }
    }
}
