package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;

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
                return ZLinkAsyncSerialQueue.yieldCurrent(remote);
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
}
