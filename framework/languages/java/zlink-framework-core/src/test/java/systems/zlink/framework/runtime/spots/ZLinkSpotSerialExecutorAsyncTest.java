package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.lang.reflect.Field;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkSpotSerialExecutorAsyncTest {
    @Test
    @org.junit.jupiter.api.Timeout(5)
    void activationTerminalKeepsQueuedReadinessAheadOfHeldJob() throws Exception {
        var workers = new java.util.concurrent.LinkedBlockingQueue<Runnable>();
        var queue =
                new ZLinkSerialExecutionQueue(
                        workers::add, ZLinkExecutionLanePolicy.spotReturningGate());
        var serials =
                new ZLinkSpotSerialExecutor(
                        queue,
                        Runnable::run,
                        workers::add,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        var prepared = queue.prepareActivation(() -> CompletableFuture.completedFuture(null));
        workers.take().run();
        var finishing = queue.finishActivationPreparation();
        var events = new java.util.ArrayList<String>();
        var boundary =
                serials.enqueueSpotBarrierNext(
                        () -> {
                            events.add("continued");
                            return CompletableFuture.completedFuture(null);
                        });
        var held =
                serials.executeAcceptedSpot(
                        new byte[] {1},
                        ignored -> {
                            events.add("held");
                            return CompletableFuture.completedFuture(null);
                        },
                        () -> {});
        workers.take().run();
        CompletableFuture.allOf(
                        prepared.toCompletableFuture(),
                        finishing.toCompletableFuture(),
                        boundary.toCompletableFuture(),
                        held.toCompletableFuture())
                .get(5, TimeUnit.SECONDS);
        serials.closeAsync().toCompletableFuture().get(5, TimeUnit.SECONDS);
        assertEquals(java.util.List.of("continued", "held"), events);
    }

    @Test
    @org.junit.jupiter.api.Timeout(5)
    void readinessBarrierHoldsApplicationJobsWhileSealPreparationYields() throws Exception {
        var workers = new java.util.concurrent.LinkedBlockingQueue<Runnable>();
        var serials =
                new ZLinkSpotSerialExecutor(
                        new ZLinkSerialExecutionQueue(
                                workers::add, ZLinkExecutionLanePolicy.spot()),
                        Runnable::run,
                        workers::add,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        var sealPrepared = new CompletableFuture<Void>();
        var events = new java.util.ArrayList<String>();
        var boundary =
                serials.enqueueSpotBarrierNext(
                        () -> {
                            events.add("boundary");
                            return ZLinkSerialExecutionQueue.yieldCurrent(sealPrepared)
                                    .thenRun(() -> events.add("continued"));
                        });
        workers.take().run();
        var held =
                serials.executeAcceptedSpot(
                        new byte[] {1},
                        ignored -> {
                            events.add("held");
                            return CompletableFuture.completedFuture(null);
                        },
                        () -> {});
        // Run the scheduled gate turn while the seal is still being prepared.
        workers.take().run();
        boolean ranBeforeSeal = held.toCompletableFuture().isDone();
        sealPrepared.complete(null);
        workers.take().run();
        CompletableFuture.allOf(boundary.toCompletableFuture(), held.toCompletableFuture())
                .get(5, TimeUnit.SECONDS);
        serials.closeAsync().toCompletableFuture().get(5, TimeUnit.SECONDS);
        assertFalse(ranBeforeSeal);
        assertEquals(java.util.List.of("boundary", "continued", "held"), events);
    }

    @Test
    void namedSharedSpotSubmissionWithInlineOwnerYieldsOnce() throws Exception {
        sharedSpotSubmission(false, false);
    }

    @Test
    void namedSharedSpotSubmissionWithBusyOwnerYieldsBeforeClaimCompletes() throws Exception {
        sharedSpotSubmission(false, true);
    }

    @Test
    void claimedSharedSpotSubmissionWithInlineOwnerYieldsOnce() throws Exception {
        sharedSpotSubmission(true, false);
    }

    @Test
    void claimedSharedSpotSubmissionDoesNotWaitForBusyRegistryOwner() throws Exception {
        sharedSpotSubmission(true, true);
    }

    private static void sharedSpotSubmission(boolean claimed, boolean busyOwner) throws Exception {
        var serials =
                new ZLinkSpotSerialExecutor(
                        new ZLinkSerialExecutionQueue(
                                Runnable::run, ZLinkExecutionLanePolicy.spot()),
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.SPOT_WIDE,
                        false);
        var activation =
                serials.claimActorQueue("actor").toCompletableFuture().get(5, TimeUnit.SECONDS);
        CompletableFuture<Void> release = new CompletableFuture<>();
        Field laneField = ZLinkSpotSerialExecutor.class.getDeclaredField("stateLane");
        laneField.setAccessible(true);
        ZLinkStateLane owner = (ZLinkStateLane) laneField.get(serials);
        if (busyOwner) {
            CompletableFuture<Void> entered = new CompletableFuture<>();
            assertTrue(
                    owner.tryPost(
                            () -> {
                                entered.complete(null);
                                return release;
                            }));
            entered.get(5, TimeUnit.SECONDS);
        }
        try {
            AtomicInteger calls = new AtomicInteger();
            java.util.function.Supplier<java.util.concurrent.CompletionStage<Void>> operation =
                    () -> {
                        calls.incrementAndGet();
                        return CompletableFuture.completedFuture(null);
                    };
            CompletableFuture<Void> admission = new CompletableFuture<>();
            var terminal =
                    serials.executeSpot(
                            0,
                            () ->
                                    claimed
                                            ? serials.executeActor(
                                                    activation, 0, operation, admission)
                                            : serials.executeActor(
                                                    "actor", 0, operation, admission));
            if (busyOwner && !claimed) {
                assertFalse(terminal.toCompletableFuture().isDone());
                assertEquals(0, calls.get());
            }
            if (claimed) terminal.toCompletableFuture().get(5, TimeUnit.SECONDS);
            release.complete(null);
            terminal.toCompletableFuture().get(5, TimeUnit.SECONDS);
            admission.get(5, TimeUnit.SECONDS);
            assertEquals(1, calls.get());
        } finally {
            release.complete(null);
            serials.closeAsync().toCompletableFuture().get(5, TimeUnit.SECONDS);
        }
    }

    @Test
    void awaitAllLanesReturnsStageWithoutWaitingForBusyStateLane() throws Exception {
        var serials =
                new ZLinkSpotSerialExecutor(
                        new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.spot()),
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.PER_ACTOR,
                        false);
        Field laneField = ZLinkSpotSerialExecutor.class.getDeclaredField("stateLane");
        laneField.setAccessible(true);
        ZLinkStateLane owner = (ZLinkStateLane) laneField.get(serials);
        ZLinkStateLane caller = new ZLinkStateLane();
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        assertTrue(
                owner.tryPost(
                        () -> {
                            entered.complete(null);
                            return release;
                        }));
        entered.join();
        try {
            var waiting =
                    caller.runAsync(
                                    () ->
                                            serials.awaitAllLanes(
                                                    ZLinkSerialExecutionQueue.Quiescence.ALL))
                            .thenCompose(stage -> stage);
            assertFalse(waiting.toCompletableFuture().isDone());
            release.complete(null);
            waiting.toCompletableFuture().join();
        } finally {
            release.complete(null);
            caller.closeAsync();
            serials.close();
        }
    }

    @Test
    void closeAsyncCompletesAfterTheStateLaneReleasesItsQueues() throws Exception {
        var serials =
                new ZLinkSpotSerialExecutor(
                        new ZLinkSerialExecutionQueue(ZLinkExecutionLanePolicy.spot()),
                        Runnable::run,
                        Runnable::run,
                        ZLinkUserSpotExecutionMode.PER_ACTOR,
                        false);
        Field laneField = ZLinkSpotSerialExecutor.class.getDeclaredField("stateLane");
        laneField.setAccessible(true);
        ZLinkStateLane owner = (ZLinkStateLane) laneField.get(serials);
        ZLinkStateLane caller = new ZLinkStateLane();
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        assertTrue(
                owner.tryPost(
                        () -> {
                            entered.complete(null);
                            return release;
                        }));
        entered.join();
        try {
            var closing = caller.runAsync(serials::closeAsync).thenCompose(stage -> stage);
            assertFalse(closing.toCompletableFuture().isDone());
            release.complete(null);
            closing.toCompletableFuture().join();
        } finally {
            release.complete(null);
            caller.closeAsync();
        }
    }
}
