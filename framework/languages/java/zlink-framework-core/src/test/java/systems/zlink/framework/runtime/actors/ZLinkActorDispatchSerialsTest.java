package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotSame;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.spots.ZLinkSpotSerialExecutor;

import java.lang.reflect.Field;
import java.lang.reflect.Proxy;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;

final class ZLinkActorDispatchSerialsTest {
    @Test
    void relocationLaneUsesCapturedActivationAfterQueueRemoval() {
        ZLinkActorDispatchTarget owner = actorTarget();
        ZLinkActorDispatchSerials dispatches = dispatches(new AtomicReference<>(owner));
        var claim = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        owner.removeActorQueueAsync("actor-1", claim.activation()).toCompletableFuture().join();
        var recreated = owner.claimActorQueue("actor-1").toCompletableFuture().join();
        assertNotSame(claim.activation().relocationLane(), recreated.relocationLane());
        assertSame(claim.activation().relocationLane(), dispatches.relocationLane("actor-1"));
        assertSame(
                claim.activation().relocationLane(),
                dispatches.relocationLaneAsync("actor-1").toCompletableFuture().join());
    }

    @Test
    void teardownRetargetDoesNotCreateUnusedTargetQueue() throws Exception {
        ZLinkActorDispatchTarget source = actorTarget();
        ZLinkActorDispatchTarget target = actorTarget();
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>(source);
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        CompletableFuture<Void> cleanup = new CompletableFuture<>();
        CompletableFuture<Void> cleanupEntered = new CompletableFuture<>();
        var teardown =
                dispatches.beginTeardown(
                        "actor-1",
                        () -> {
                            cleanupEntered.complete(null);
                            return cleanup;
                        });
        cleanupEntered.join();
        owner.set(target);
        var claim = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        assertSame(source, claim.target());
        Field queuesField = ZLinkSpotSerialExecutor.class.getDeclaredField("actorQueues");
        queuesField.setAccessible(true);
        Field laneField = ZLinkSpotSerialExecutor.class.getDeclaredField("stateLane");
        laneField.setAccessible(true);
        ZLinkStateLane targetState = (ZLinkStateLane) laneField.get(target);
        var targetQueues = (java.util.Map<?, ?>) queuesField.get(target);
        try {
            assertTrue(
                    targetState.runNowOrQueue(targetQueues::isEmpty).toCompletableFuture().join());
        } finally {
            cleanup.complete(null);
            teardown.toCompletableFuture().join();
        }
    }

    @Test
    void retiredClaimCannotEnterRecreatedActivation() {
        ZLinkActorDispatchTarget owner = actorTarget();
        ZLinkActorDispatchSerials dispatches = dispatches(new AtomicReference<>(owner));
        var retired = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        dispatches
                .beginTeardown("actor-1", () -> CompletableFuture.completedFuture(null))
                .toCompletableFuture()
                .join();
        var recreated = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        AtomicInteger calls = new AtomicInteger();
        var stale =
                dispatches.enqueue(
                        retired,
                        () -> {
                            calls.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });
        assertThrows(CompletionException.class, () -> stale.toCompletableFuture().join());
        assertEquals(0, calls.get());
        dispatches
                .enqueue(
                        recreated,
                        () -> {
                            calls.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        })
                .toCompletableFuture()
                .join();
        assertEquals(1, calls.get());
    }

    @Test
    void prepareAsyncDoesNotBlockWhenMetadataOwnerIsBusy() throws Exception {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        Field field = ZLinkActorDispatchSerials.class.getDeclaredField("stateLane");
        field.setAccessible(true);
        ZLinkStateLane lane = (ZLinkStateLane) field.get(dispatches);
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        lane.runAsync(
                () -> {
                    entered.complete(null);
                    release.join();
                    return null;
                });
        entered.join();
        try {
            var prepared = dispatches.prepareAsync("actor-1");
            assertFalse(prepared.toCompletableFuture().isDone());
            release.complete(null);
            assertEquals("actor-1", prepared.toCompletableFuture().join().actorId());
        } finally {
            release.complete(null);
        }
    }

    @Test
    void preparedClaimKeepsItsQueueOwnerAfterRetarget() {
        ZLinkActorDispatchTarget source = actorTarget();
        ZLinkActorDispatchTarget target = actorTarget();
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>(source);
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        var claim = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        owner.set(target);
        dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        CompletableFuture<Void> held = new CompletableFuture<>();
        var submitted = dispatches.enqueue(claim, () -> held);
        assertSame(source, claim.target());
        assertFalse(source.awaitActorQuiescence("actor-1").toCompletableFuture().isDone());
        assertTrue(target.awaitActorQuiescence("actor-1").toCompletableFuture().isDone());
        held.complete(null);
        submitted.toCompletableFuture().join();
    }

    @Test
    void teardownRetargetUsesTheSealedActivationQueue() {
        ZLinkActorDispatchTarget source = actorTarget();
        ZLinkActorDispatchTarget target = actorTarget();
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>(source);
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        CompletableFuture<Void> cleanupEntered = new CompletableFuture<>();
        CompletableFuture<Void> cleanup = new CompletableFuture<>();
        var teardown =
                dispatches.beginTeardown(
                        "actor-1",
                        () -> {
                            cleanupEntered.complete(null);
                            return cleanup;
                        });
        cleanupEntered.join();
        try {
            owner.set(target);
            // spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:81:
            // Actor queue owns payload admission; prepare only captures that queue.
            // spec/server/00-foundation/06-framework-api.ko.md:926: admission seal -> Rejected.
            var claim = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
            assertSame(source, claim.target());
            var submitted =
                    dispatches.enqueue(claim, () -> CompletableFuture.completedFuture(null));
            CompletionException failure =
                    assertThrows(
                            CompletionException.class,
                            () -> submitted.toCompletableFuture().join());
            assertEquals(
                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind.REJECTED,
                    ((systems.zlink.framework.errors.ZLinkFrameworkException) failure.getCause())
                            .kind());
            assertTrue(target.awaitActorQuiescence("actor-1").toCompletableFuture().isDone());
        } finally {
            cleanup.complete(null);
            teardown.toCompletableFuture().join();
        }
    }

    @Test
    void retargetRetainsTheLifecycleBarrierClaim() {
        ZLinkActorDispatchTarget source = actorTarget();
        ZLinkActorDispatchTarget target = actorTarget();
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>(source);
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> lifecycle = new CompletableFuture<>();
        var barrier =
                dispatches.enqueueBarrier(
                        "actor-1",
                        () -> {
                            entered.complete(null);
                            return lifecycle;
                        });
        entered.join();
        owner.set(target);
        AtomicInteger handlers = new AtomicInteger();
        var claim = dispatches.prepareAsync("actor-1").toCompletableFuture().join();
        var submitted =
                dispatches.enqueue(
                        claim,
                        () -> {
                            handlers.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });
        assertFalse(submitted.toCompletableFuture().isDone());
        assertEquals(0, handlers.get());
        lifecycle.complete(null);
        barrier.toCompletableFuture().join();
        submitted.toCompletableFuture().join();
        assertEquals(1, handlers.get());
    }

    @Test
    void retainedCommitWaitsForBusySpotOwnerWithoutBlockingCaller() throws Exception {
        ZLinkSpotSerialExecutor spot = (ZLinkSpotSerialExecutor) actorTarget();
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>(spot);
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        var seal = spot.trySealActorRelocation("actor-1").orElseThrow();
        Field laneField = ZLinkSpotSerialExecutor.class.getDeclaredField("stateLane");
        laneField.setAccessible(true);
        ZLinkStateLane lane = (ZLinkStateLane) laneField.get(spot);
        CompletableFuture<Void> entered = new CompletableFuture<>();
        CompletableFuture<Void> release = new CompletableFuture<>();
        assertTrue(
                lane.tryPost(
                        () -> {
                            entered.complete(null);
                            return release;
                        }));
        entered.join();
        try {
            var retained = dispatches.retainCommitAsync("actor-1", seal).toCompletableFuture();
            assertFalse(retained.isDone());
            release.complete(null);
            var commit = retained.join().orElseThrow();
            systems.zlink.framework.runtime.internal.relocation.ZLinkRetainedSerialQueueCommit.Cut
                    cut;
            do {
                cut = commit.cut();
            } while (!commit.tryEstablishAndFinishCapture(cut));
            commit.complete();
        } finally {
            release.complete(null);
        }
    }

    @Test
    void teardownRemovalFailureCompletesTerminalExceptionally() {
        ZLinkActorDispatchTarget spot = actorTarget();
        IllegalStateException removalFailure = new IllegalStateException("remove failed");
        ZLinkActorDispatchTarget failing =
                (ZLinkActorDispatchTarget)
                        Proxy.newProxyInstance(
                                ZLinkActorDispatchTarget.class.getClassLoader(),
                                new Class<?>[] {ZLinkActorDispatchTarget.class},
                                (proxy, method, args) ->
                                        method.getName().equals("removeActorQueueAsync")
                                                ? CompletableFuture.failedFuture(removalFailure)
                                                : method.invoke(spot, args));
        ZLinkActorDispatchSerials dispatches = dispatches(new AtomicReference<>(failing));
        CompletionStage<Void> teardown =
                dispatches.beginTeardown("actor-1", () -> CompletableFuture.completedFuture(null));
        CompletionException terminal =
                assertThrows(
                        CompletionException.class, () -> teardown.toCompletableFuture().join());
        assertSame(removalFailure, terminal.getCause());
    }

    @Test
    void teardownKeepsAdmissionClosedUntilSpotRemovalFinishes() {
        ZLinkActorDispatchTarget spot = actorTarget();
        CompletableFuture<Void> removeEntered = new CompletableFuture<>();
        CompletableFuture<Void> removal = new CompletableFuture<>();
        ZLinkActorDispatchTarget delayed =
                (ZLinkActorDispatchTarget)
                        Proxy.newProxyInstance(
                                ZLinkActorDispatchTarget.class.getClassLoader(),
                                new Class<?>[] {ZLinkActorDispatchTarget.class},
                                (proxy, method, args) -> {
                                    if (method.getName().equals("removeActorQueueAsync")) {
                                        removeEntered.complete(null);
                                        return removal;
                                    }
                                    return method.invoke(spot, args);
                                });
        ZLinkActorDispatchSerials dispatches = dispatches(new AtomicReference<>(delayed));
        CompletionStage<Void> teardown =
                dispatches.beginTeardown("actor-1", () -> CompletableFuture.completedFuture(null));
        removeEntered.join();
        try {
            assertFalse(teardown.toCompletableFuture().isDone());
            // spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md:81:
            // The sealed Actor queue rejects admission when enqueue is attempted.
            // spec/server/00-foundation/06-framework-api.ko.md:926: admission seal -> Rejected.
            var rejected =
                    dispatches.enqueue(
                            dispatches.prepare("actor-1"),
                            () -> CompletableFuture.completedFuture(null));
            CompletionException rejection =
                    assertThrows(
                            CompletionException.class, () -> rejected.toCompletableFuture().join());
            assertEquals(
                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind.REJECTED,
                    ((systems.zlink.framework.errors.ZLinkFrameworkException) rejection.getCause())
                            .kind());
        } finally {
            removal.complete(null);
        }
        teardown.toCompletableFuture().join();
    }

    @Test
    void queuedBarrierWaitsForActiveActorDispatch() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> active = new CompletableFuture<>();
        List<String> order = new ArrayList<>();

        var first =
                dispatches.enqueue(
                        dispatches.prepare("actor-1"),
                        () -> {
                            order.add("dispatch-started");
                            return active.thenRun(() -> order.add("dispatch-completed"));
                        });
        var barrier =
                dispatches.enqueue(
                        dispatches.prepare("actor-1"),
                        () -> {
                            order.add("handoff-started");
                            return CompletableFuture.completedFuture(null);
                        });

        assertFalse(barrier.toCompletableFuture().isDone());
        active.complete(null);
        CompletableFuture.allOf(first.toCompletableFuture(), barrier.toCompletableFuture()).join();

        assertEquals(List.of("dispatch-started", "dispatch-completed", "handoff-started"), order);
    }

    @Test
    void allActorBarrierWaitsForEveryIndependentLane() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> actorA = new CompletableFuture<>();
        CompletableFuture<Void> actorB = new CompletableFuture<>();

        dispatches.enqueue(dispatches.prepare("actor-a"), () -> actorA);
        dispatches.enqueue(dispatches.prepare("actor-b"), () -> actorB);

        CompletableFuture<Void> barrier = dispatches.awaitQuiescence().toCompletableFuture();
        assertFalse(barrier.isDone());
        actorA.complete(null);
        assertFalse(barrier.isDone());
        actorB.complete(null);
        barrier.join();
    }

    @Test
    void teardownWaitsForAcceptedTurnsAndClosesAdmission() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> release = new CompletableFuture<>();
        AtomicInteger cleanupCount = new AtomicInteger();

        CompletionStage<Void> active =
                dispatches.enqueue(dispatches.prepare("actor-1"), () -> release);
        CompletionStage<Void> accepted =
                dispatches.enqueue(
                        dispatches.prepare("actor-1"),
                        () -> CompletableFuture.completedFuture(null));
        CompletionStage<Void> teardown =
                dispatches.beginTeardown(
                        "actor-1",
                        () -> {
                            cleanupCount.incrementAndGet();
                            return CompletableFuture.completedFuture(null);
                        });

        assertFalse(teardown.toCompletableFuture().isDone());
        var rejected =
                dispatches.enqueue(
                        dispatches.prepare("actor-1"),
                        () -> CompletableFuture.completedFuture(null));
        CompletionException rejection =
                assertThrows(
                        CompletionException.class, () -> rejected.toCompletableFuture().join());
        assertEquals(
                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.REJECTED,
                ((systems.zlink.framework.errors.ZLinkFrameworkException) rejection.getCause())
                        .kind());
        assertEquals(0, cleanupCount.get());

        release.complete(null);
        CompletableFuture.allOf(
                        active.toCompletableFuture(),
                        accepted.toCompletableFuture(),
                        teardown.toCompletableFuture())
                .join();
        assertEquals(1, cleanupCount.get());
    }

    @Test
    void teardownStartedInsideCurrentTurnDoesNotWaitForItself() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        AtomicInteger cleanupCount = new AtomicInteger();

        CompletionStage<Void> active =
                dispatches.enqueue(
                        dispatches.prepare("actor-1"),
                        () ->
                                dispatches.beginTeardown(
                                        "actor-1",
                                        () -> {
                                            cleanupCount.incrementAndGet();
                                            return CompletableFuture.completedFuture(null);
                                        }));

        active.toCompletableFuture().join();
        dispatches.awaitQuiescence().toCompletableFuture().join();
        assertEquals(1, cleanupCount.get());
    }

    @Test
    void consecutiveAcceptedPacketTurnsReleaseTheActorLane() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        List<String> order = new ArrayList<>();

        CompletionStage<Void> first =
                dispatches.enqueue(
                        dispatches.prepare("actor-1"),
                        new byte[] {1},
                        () -> {
                            order.add("first");
                            return CompletableFuture.completedFuture(null);
                        });
        CompletionStage<Void> second =
                dispatches.enqueue(
                        dispatches.prepare("actor-1"),
                        new byte[] {2},
                        () -> {
                            order.add("second");
                            return CompletableFuture.completedFuture(null);
                        });

        CompletableFuture.allOf(first.toCompletableFuture(), second.toCompletableFuture()).join();
        assertEquals(List.of("first", "second"), order);
    }

    @Test
    void yieldedContinuationDoesNotBlockTheNextActorTurn() throws Exception {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        CompletableFuture<Void> secondStarted = new CompletableFuture<>();

        CompletionStage<Void> first =
                dispatches.enqueue(
                        dispatches.prepare("actor-1"),
                        () -> {
                            firstStarted.complete(null);
                            return ZLinkSerialExecutionQueue.yieldCurrent(remote);
                        });
        CompletionStage<Void> second =
                dispatches.enqueue(
                        dispatches.prepare("actor-1"),
                        () -> {
                            secondStarted.complete(null);
                            return CompletableFuture.completedFuture(null);
                        });

        firstStarted.get();
        secondStarted.get();
        assertFalse(first.toCompletableFuture().isDone());

        remote.complete(null);
        CompletableFuture.allOf(first.toCompletableFuture(), second.toCompletableFuture()).join();
    }

    @Test
    void committedQueuesAreRetiredAcrossActorReturnToPreviousSpot() {
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>();
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        ZLinkActorDispatchTarget spotA = actorTarget();
        ZLinkActorDispatchTarget spotB = actorTarget();
        List<String> order = new ArrayList<>();

        relocateAndRetire(dispatches, owner, spotA, order, "A");
        relocateAndRetire(dispatches, owner, spotB, order, "B");
        owner.set(spotA);

        enqueueLazy(dispatches, order, "A-return").toCompletableFuture().join();

        assertEquals(List.of("A", "B", "A-return"), order);
    }

    @Test
    void removeRetiresLastPreparedTargetAfterResolverLosesActor() {
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>();
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        ZLinkActorDispatchTarget spotA = actorTarget();
        List<String> order = new ArrayList<>();

        owner.set(spotA);
        enqueueLazy(dispatches, order, "before-remove").toCompletableFuture().join();
        dispatches.awaitQuiescence().toCompletableFuture().join();
        var seal = dispatches.trySeal("actor-1").orElseThrow();
        dispatches.commit("actor-1", seal).orElseThrow();

        owner.set(null);
        dispatches.removeAsync("actor-1").toCompletableFuture().join();
        owner.set(spotA);
        enqueueLazy(dispatches, order, "after-remove").toCompletableFuture().join();

        assertEquals(List.of("before-remove", "after-remove"), order);
    }

    @Test
    void removeRetiresQueueCreatedOnlyForRelocation() {
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>();
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        ZLinkActorDispatchTarget spotA = actorTarget();
        List<String> order = new ArrayList<>();

        owner.set(spotA);
        dispatches.relocationLane("actor-1");
        var seal = dispatches.trySeal("actor-1").orElseThrow();
        dispatches.commit("actor-1", seal).orElseThrow();

        owner.set(null);
        dispatches.removeAsync("actor-1").toCompletableFuture().join();
        owner.set(spotA);
        enqueueLazy(dispatches, order, "after-remove").toCompletableFuture().join();

        assertEquals(List.of("after-remove"), order);
    }

    private static ZLinkActorDispatchSerials dispatches(
            AtomicReference<ZLinkActorDispatchTarget> owner) {
        return new ZLinkActorDispatchSerials(
                new Object(),
                actorId -> actorId,
                Runnable::run,
                actorId ->
                        CompletableFuture.completedFuture(
                                new ZLinkActorDispatchTarget.ActivationSnapshot(
                                        owner.get(), actorId)));
    }

    private static ZLinkActorDispatchTarget actorTarget() {
        return new ZLinkSpotSerialExecutor(
                new ZLinkSerialExecutionQueue(Runnable::run, ZLinkExecutionLanePolicy.spot()),
                Runnable::run,
                Runnable::run,
                ZLinkUserSpotExecutionMode.PER_ACTOR,
                false);
    }

    private static void relocateAndRetire(
            ZLinkActorDispatchSerials dispatches,
            AtomicReference<ZLinkActorDispatchTarget> owner,
            ZLinkActorDispatchTarget target,
            List<String> order,
            String step) {
        owner.set(target);
        enqueueLazy(dispatches, order, step).toCompletableFuture().join();
        dispatches.awaitQuiescence().toCompletableFuture().join();
        var seal = dispatches.trySeal("actor-1").orElseThrow();
        dispatches.commit("actor-1", seal).orElseThrow();
        dispatches
                .beginTeardown(
                        "actor-1",
                        () -> {
                            owner.set(null);
                            return CompletableFuture.completedFuture(null);
                        })
                .toCompletableFuture()
                .join();
    }

    private static CompletionStage<Void> enqueueLazy(
            ZLinkActorDispatchSerials dispatches, List<String> order, String step) {
        return dispatches.enqueueLazyRecord(
                dispatches.prepare("actor-1"),
                () -> new byte[] {1},
                1,
                () -> {
                    order.add(step);
                    return CompletableFuture.completedFuture(null);
                },
                () -> {});
    }
}
