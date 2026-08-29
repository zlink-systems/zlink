package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.runtime.spots.ZLinkSpotSerialExecutor;

final class ZLinkActorDispatchSerialsTest {
    @Test
    void queuedBarrierWaitsForActiveActorDispatch() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> active = new CompletableFuture<>();
        List<String> order = new ArrayList<>();

        var first = dispatches.enqueue(
            dispatches.prepare("actor-1"),
            () -> {
                order.add("dispatch-started");
                return active.thenRun(() -> order.add("dispatch-completed"));
            });
        var barrier = dispatches.enqueue(
            dispatches.prepare("actor-1"),
            () -> {
                order.add("handoff-started");
                return CompletableFuture.completedFuture(null);
            });

        assertFalse(barrier.toCompletableFuture().isDone());
        active.complete(null);
        CompletableFuture.allOf(
            first.toCompletableFuture(), barrier.toCompletableFuture()).join();

        assertEquals(
            List.of("dispatch-started", "dispatch-completed", "handoff-started"),
            order);
    }

    @Test
    void allActorBarrierWaitsForEveryIndependentLane() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> actorA = new CompletableFuture<>();
        CompletableFuture<Void> actorB = new CompletableFuture<>();

        dispatches.enqueue(
            dispatches.prepare("actor-a"),
            () -> actorA);
        dispatches.enqueue(
            dispatches.prepare("actor-b"),
            () -> actorB);

        CompletableFuture<Void> barrier =
            dispatches.awaitQuiescence().toCompletableFuture();
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

        CompletionStage<Void> active = dispatches.enqueue(
            dispatches.prepare("actor-1"),
            () -> release);
        CompletionStage<Void> accepted = dispatches.enqueue(
            dispatches.prepare("actor-1"),
            () -> CompletableFuture.completedFuture(null));
        CompletionStage<Void> teardown = dispatches.beginTeardown(
            "actor-1",
            () -> {
                cleanupCount.incrementAndGet();
                return CompletableFuture.completedFuture(null);
            });

        assertFalse(teardown.toCompletableFuture().isDone());
        assertThrows(
            IllegalStateException.class,
            () -> dispatches.prepare("actor-1"));
        assertEquals(0, cleanupCount.get());

        release.complete(null);
        CompletableFuture.allOf(
            active.toCompletableFuture(),
            accepted.toCompletableFuture(),
            teardown.toCompletableFuture()).join();
        assertEquals(1, cleanupCount.get());
    }

    @Test
    void teardownStartedInsideCurrentTurnDoesNotWaitForItself() {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        AtomicInteger cleanupCount = new AtomicInteger();

        CompletionStage<Void> active = dispatches.enqueue(
            dispatches.prepare("actor-1"),
            () -> dispatches.beginTeardown(
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

        CompletionStage<Void> first = dispatches.enqueue(
            dispatches.prepare("actor-1"),
            new byte[] {1},
            () -> dispatches.runTurn("actor-1", () -> {
                order.add("first");
                return CompletableFuture.completedFuture(null);
            }));
        CompletionStage<Void> second = dispatches.enqueue(
            dispatches.prepare("actor-1"),
            new byte[] {2},
            () -> dispatches.runTurn("actor-1", () -> {
                order.add("second");
                return CompletableFuture.completedFuture(null);
            }));

        CompletableFuture.allOf(
            first.toCompletableFuture(),
            second.toCompletableFuture()).join();
        assertEquals(List.of("first", "second"), order);
    }

    @Test
    void yieldedContinuationDoesNotBlockTheNextActorTurn() throws Exception {
        ZLinkActorDispatchSerials dispatches = new ZLinkActorDispatchSerials();
        CompletableFuture<Void> remote = new CompletableFuture<>();
        CompletableFuture<Void> firstStarted = new CompletableFuture<>();
        CompletableFuture<Void> secondStarted = new CompletableFuture<>();

        CompletionStage<Void> first = dispatches.enqueue(
            dispatches.prepare("actor-1"),
            () -> {
                firstStarted.complete(null);
                return ZLinkSerialExecutionQueue.yieldCurrent(remote);
            });
        CompletionStage<Void> second = dispatches.enqueue(
            dispatches.prepare("actor-1"),
            () -> {
                secondStarted.complete(null);
                return CompletableFuture.completedFuture(null);
            });

        firstStarted.get();
        secondStarted.get();
        assertFalse(first.toCompletableFuture().isDone());

        remote.complete(null);
        CompletableFuture.allOf(
            first.toCompletableFuture(),
            second.toCompletableFuture()).join();
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

        enqueueLazy(dispatches, order, "A-return")
            .toCompletableFuture()
            .join();

        assertEquals(List.of("A", "B", "A-return"), order);
    }

    @Test
    void removeRetiresLastPreparedTargetAfterResolverLosesActor() {
        AtomicReference<ZLinkActorDispatchTarget> owner = new AtomicReference<>();
        ZLinkActorDispatchSerials dispatches = dispatches(owner);
        ZLinkActorDispatchTarget spotA = actorTarget();
        List<String> order = new ArrayList<>();

        owner.set(spotA);
        enqueueLazy(dispatches, order, "before-remove")
            .toCompletableFuture()
            .join();
        var seal = dispatches.trySeal("actor-1").orElseThrow();
        dispatches.commit("actor-1", seal).orElseThrow();

        owner.set(null);
        dispatches.remove("actor-1");
        owner.set(spotA);
        enqueueLazy(dispatches, order, "after-remove")
            .toCompletableFuture()
            .join();

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
        dispatches.remove("actor-1");
        owner.set(spotA);
        enqueueLazy(dispatches, order, "after-remove")
            .toCompletableFuture()
            .join();

        assertEquals(List.of("after-remove"), order);
    }

    private static ZLinkActorDispatchSerials dispatches(
        AtomicReference<ZLinkActorDispatchTarget> owner) {
        return new ZLinkActorDispatchSerials(
            new Object(), actorId -> actorId, Runnable::run,
            actorId -> owner.get());
    }

    private static ZLinkActorDispatchTarget actorTarget() {
        return new ZLinkSpotSerialExecutor(
            new ZLinkSerialExecutionQueue(
                Runnable::run, ZLinkExecutionLanePolicy.spot()),
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
        var seal = dispatches.trySeal("actor-1").orElseThrow();
        dispatches.commit("actor-1", seal).orElseThrow();
        dispatches.beginTeardown("actor-1", () -> {
            owner.set(null);
            return CompletableFuture.completedFuture(null);
        }).toCompletableFuture().join();
    }

    private static CompletionStage<Void> enqueueLazy(
        ZLinkActorDispatchSerials dispatches,
        List<String> order,
        String step) {
        return dispatches.enqueueLazyRecord(
            dispatches.prepare("actor-1"),
            () -> new byte[] {1},
            1,
            () -> {
                order.add(step);
                return CompletableFuture.completedFuture(null);
            },
            () -> { });
    }
}
