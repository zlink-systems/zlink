package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import static systems.zlink.framework.runtime.actors.ZLinkSessionActorBindingContractTest.runtime;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ActorRef;
import systems.zlink.framework.runtime.actors.ZLinkSessionActorBindingContractTest.FakeStream;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.streams.ZLinkStreamActorControl;

import java.time.Duration;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.function.BooleanSupplier;

final class ZLinkSessionActorReplacementControlOrderTest {
    @RepeatedTest(20)
    void sameSessionRetiresOldSlotBeforeAnnouncingReplacement() throws Exception {
        var stream = new FakeStream();
        var runtime = runtime(stream);
        String actorId = "actor-1";
        var oldOwner = RoutingId.from("actor-node-a");
        var newOwner = RoutingId.from("actor-node-b");
        var original =
                (ZLinkBoundActor)
                        runtime.bind(new ActorRef(actorId, 7, "game", oldOwner))
                                .toCompletableFuture()
                                .get(5, TimeUnit.SECONDS);

        // Wait for the first bind's off-lane transition publication before starting the next bind.
        await(
                () -> !runtime.hasBindingTransition(actorId),
                "initial bind transition did not finish");

        var acknowledgement = new CompletableFuture<Void>();
        stream.bindingAcknowledgement = acknowledgement;
        var replacement =
                runtime.bind(new ActorRef(actorId, 7, "game", newOwner)).toCompletableFuture();
        var occupied = new CompletableFuture<Void>();
        var release = new CompletableFuture<Void>();
        var holder =
                runtime.refreshActorLease(
                        new ZLinkBackendActorRef(oldOwner, actorId, 7),
                        original.bindingGeneration(),
                        1,
                        () -> {
                            occupied.complete(null);
                            release.join();
                            return false;
                        });
        Thread acknowledge =
                Thread.ofPlatform().daemon().unstarted(() -> acknowledgement.complete(null));
        try {
            occupied.get(5, TimeUnit.SECONDS);
            acknowledge.start();
            // Main waits for removal's turn. On the PR, both turns queue before the thread exits.
            await(
                    () -> acknowledge.getState() == Thread.State.WAITING || !acknowledge.isAlive(),
                    "replacement acknowledgement did not reach the occupied state lane");
        } finally {
            release.complete(null);
        }
        holder.toCompletableFuture().get(5, TimeUnit.SECONDS);
        assertTrue(acknowledge.join(Duration.ofSeconds(5)));
        replacement.get(5, TimeUnit.SECONDS);

        await(() -> stream.controlNames().size() == 3, "replacement controls were not submitted");
        assertEquals(
                List.of(
                        ZLinkStreamActorControl.BOUND,
                        ZLinkStreamActorControl.UNBOUND,
                        ZLinkStreamActorControl.BOUND),
                stream.controlNames());
        assertEquals(
                List.of(1, 1, 2),
                List.of(stream.controlSlot(0), stream.controlSlot(1), stream.controlSlot(2)));
    }

    private static void await(BooleanSupplier condition, String message) {
        long deadline = System.nanoTime() + TimeUnit.SECONDS.toNanos(5);
        while (!condition.getAsBoolean()) {
            assertTrue(System.nanoTime() < deadline, message);
            Thread.onSpinWait();
        }
    }
}
