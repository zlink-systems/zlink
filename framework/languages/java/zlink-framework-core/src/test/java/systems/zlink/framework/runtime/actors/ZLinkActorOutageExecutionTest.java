package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.execution.*;
import systems.zlink.framework.runtime.internal.backend.*;
import systems.zlink.framework.runtime.locations.*;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.spots.ZLinkSpotSerialExecutor;
import systems.zlink.framework.spots.*;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.*;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkActorOutageExecutionTest {
    @RepeatedTest(100)
    void memberDisposalIsDirectionalInBothGateModes() {
        for (boolean provider : new boolean[] {false, true}) {
            for (var mode : ZLinkUserSpotExecutionMode.values()) {
                try (var f = new ZLinkLocationStoreOutageTest.Fixture(provider, true)) {
                    var nativeDisposal = new CompletableFuture<Void>();
                    var detached =
                            Map.of(
                                    "first:1",
                                    new CompletableFuture<Void>(),
                                    "first:2",
                                    new CompletableFuture<Void>(),
                                    "second:1",
                                    new CompletableFuture<Void>());
                    var node =
                            (ZLinkInternalSpotNode)
                                    Proxy.newProxyInstance(
                                            ZLinkInternalSpotNode.class.getClassLoader(),
                                            new Class<?>[] {ZLinkInternalSpotNode.class},
                                            (proxy, method, args) ->
                                                    switch (method.getName()) {
                                                        case "routingId" -> RoutingId.from("node");
                                                        case "destroyActor" -> {
                                                            var ref =
                                                                    (ZLinkBackendActorRef) args[0];
                                                            detached.get(
                                                                            ref.actorId()
                                                                                    + ":"
                                                                                    + ref
                                                                                            .generation())
                                                                    .complete(null);
                                                            yield nativeDisposal;
                                                        }
                                                        default -> throw new AssertionError(method);
                                                    });
                    var actors =
                            new ZLinkActorRuntime(
                                    node,
                                    Map.of(),
                                    Duration.ofSeconds(5),
                                    new ZLinkJsonMessageSerializer());
                    actors.setMeshName("mesh");
                    actors.setLocationLifecycle(new ZLinkLocationLifecycle(f.runtime));
                    var spotQueue =
                            new ZLinkSerialExecutionQueue(
                                    Runnable::run, ZLinkExecutionLanePolicy.spot());
                    var serials =
                            new ZLinkSpotSerialExecutor(
                                    spotQueue, Runnable::run, Runnable::run, mode, false);
                    ZLinkSpot<ZLinkActor> spot =
                            new ZLinkSpot<>() {
                                public ZLinkSpotContext context() {
                                    return null;
                                }

                                public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
                                    return CompletableFuture.completedFuture(null);
                                }

                                public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
                                    return CompletableFuture.completedFuture(null);
                                }
                            };
                    var first = actor(actors, "first", spot);
                    var second = actor(actors, "second", spot);
                    var currentFirst = new java.util.concurrent.atomic.AtomicReference<>(first);
                    actors.setActorDispatchTargetResolver(
                            id ->
                                    CompletableFuture.completedFuture(
                                            new ZLinkActorDispatchTarget.ActivationSnapshot(
                                                    serials,
                                                    id.equals("first")
                                                            ? currentFirst.get()
                                                            : second)));
                    var spotDisposals = new AtomicInteger();
                    var spotExecution =
                            f.runtime.execution(
                                    () -> true,
                                    () -> {
                                        spotDisposals.incrementAndGet();
                                        actors.discardLocalMembers(spot);
                                        spotQueue.discardApplications();
                                    });
                    spotQueue.executionPolicy(spotExecution);
                    spotExecution.ready().toCompletableFuture().join();
                    actors.confirmReady("first").toCompletableFuture().join();
                    actors.confirmReady("second").toCompletableFuture().join();
                    var firstTurn =
                            actors.dispatches.prepareAsync("first").toCompletableFuture().join();
                    var secondTurn =
                            actors.dispatches.prepareAsync("second").toCompletableFuture().join();
                    var calls = new AtomicInteger();
                    actors.dispatches
                            .enqueue(
                                    firstTurn,
                                    () -> {
                                        calls.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    })
                            .toCompletableFuture()
                            .join();
                    var oldRemote = new CompletableFuture<Void>();
                    var oldEntered = new CompletableFuture<Void>();
                    var oldCompletion =
                            actors.dispatches.enqueue(
                                    firstTurn,
                                    () -> {
                                        oldEntered.complete(null);
                                        return ZLinkSerialExecutionQueue.manageCurrentInTurn(
                                                        oldRemote)
                                                .thenRun(calls::incrementAndGet);
                                    });
                    oldEntered.join();
                    // Definitive member loss must not close the shared Spot or its other member.
                    actors.deactivateActorOnOwnershipLoss("first").toCompletableFuture().join();
                    detached.get("first:1").join();
                    assertNull(first.actorRef());
                    assertNull(first.actor());
                    assertFalse(
                            nativeDisposal.isDone(),
                            "local memory is detached before native disposal completes");
                    assertThrows(
                            RuntimeException.class,
                            () ->
                                    actors.dispatches
                                            .enqueue(
                                                    firstTurn,
                                                    () -> {
                                                        calls.incrementAndGet();
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    })
                                            .toCompletableFuture()
                                            .join());
                    var replacement = actor(actors, "first", spot, 2);
                    currentFirst.set(replacement);
                    actors.confirmReady("first").toCompletableFuture().join();
                    var freshTurn =
                            actors.dispatches.prepareAsync("first").toCompletableFuture().join();
                    oldRemote.complete(null);
                    assertThrows(
                            RuntimeException.class,
                            () -> oldCompletion.toCompletableFuture().join());
                    actors.dispatches
                            .enqueue(
                                    freshTurn,
                                    () -> {
                                        calls.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    })
                            .toCompletableFuture()
                            .join();
                    actors.dispatches
                            .enqueue(
                                    secondTurn,
                                    () -> {
                                        calls.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    })
                            .toCompletableFuture()
                            .join();
                    assertEquals(0, spotDisposals.get());
                    f.now.incrementAndGet();
                    f.failRenewal();
                    f.now.set(f.runtime.ownerLeaseSnapshot().outageCutoffNanos() - 1);
                    actors.dispatches
                            .enqueue(
                                    secondTurn,
                                    () -> {
                                        calls.incrementAndGet();
                                        return CompletableFuture.completedFuture(null);
                                    })
                            .toCompletableFuture()
                            .join();
                    assertThrows(
                            IllegalStateException.class,
                            () -> actors.create("new", "type").submit());
                    assertThrows(
                            IllegalStateException.class,
                            () -> actors.locations.findStoredActorRefExact("second"));
                    assertThrows(
                            IllegalStateException.class,
                            () -> actors.relocateActorJoin(null, Duration.ofSeconds(1)));
                    // Discarding the actual Spot object cascades through existing membership.
                    spotExecution.discard();
                    assertThrows(
                            RuntimeException.class,
                            () ->
                                    actors.dispatches
                                            .enqueue(
                                                    secondTurn,
                                                    () -> {
                                                        calls.incrementAndGet();
                                                        return CompletableFuture.completedFuture(
                                                                null);
                                                    })
                                            .toCompletableFuture()
                                            .join());
                    assertEquals(4, calls.get());
                    detached.get("first:2").join();
                    detached.get("second:1").join();
                    assertNull(replacement.actorRef());
                    assertNull(second.actorRef());
                    nativeDisposal.complete(null);
                    actors.dispatches.awaitQuiescence().toCompletableFuture().join();
                    assertTrue(
                            actors.localActorAsync("first").toCompletableFuture().join().isEmpty());
                    assertTrue(
                            actors.localActorAsync("second")
                                    .toCompletableFuture()
                                    .join()
                                    .isEmpty());
                    spotQueue.close();
                    actors.closeAsync().toCompletableFuture().join();
                }
            }
        }
    }

    @RepeatedTest(100)
    void relocationCompletedBeforeOutageIsSteadyReady() {
        for (boolean provider : new boolean[] {false, true}) {
            try (var f = new ZLinkLocationStoreOutageTest.Fixture(provider, true)) {
                var actors = ZLinkActorRelocationStagingTest.runtime(new AtomicInteger());
                actors.setLocationLifecycle(new ZLinkLocationLifecycle(f.runtime));
                var prepared =
                        actors.prepareRelocatedActor(
                                        "settled",
                                        "probe",
                                        new byte[0],
                                        false,
                                        null,
                                        () -> false,
                                        null)
                                .toCompletableFuture()
                                .join();
                actors.publishPreparedTransferredActor(prepared);
                var context = (ZLinkActorRuntime.DefaultActorContext) prepared.actor().context();
                var firstFailure =
                        context.moveCompletion()
                                .thenRun(
                                        () -> {
                                            f.now.incrementAndGet();
                                            f.failRenewal();
                                        });
                actors.completePreparedTransferredActor(prepared);
                firstFailure.toCompletableFuture().join();
                var removed = new CompletableFuture<Void>();
                actors.setCountChanged(() -> removed.complete(null));
                var turn = actors.dispatches.prepareAsync("settled").toCompletableFuture().join();
                var calls = new AtomicInteger();
                long cutoff = f.runtime.ownerLeaseSnapshot().outageCutoffNanos();
                f.now.set(cutoff - 1);
                actors.dispatches
                        .enqueue(
                                turn,
                                () -> {
                                    calls.incrementAndGet();
                                    return CompletableFuture.completedFuture(null);
                                })
                        .toCompletableFuture()
                        .join();
                f.now.set(cutoff);
                f.fireCutoff();
                assertThrows(
                        RuntimeException.class,
                        () ->
                                actors.dispatches
                                        .enqueue(
                                                turn,
                                                () -> {
                                                    calls.incrementAndGet();
                                                    return CompletableFuture.completedFuture(null);
                                                })
                                        .toCompletableFuture()
                                        .join());
                removed.join();
                assertTrue(
                        actors.localActorAsync("settled").toCompletableFuture().join().isEmpty());
                assertEquals(1, calls.get());
                actors.closeAsync().toCompletableFuture().join();
            }
        }
    }

    private ZLinkActorRuntime.DefaultActorContext actor(
            ZLinkActorRuntime actors, String id, ZLinkSpot<?> spot) {
        return actor(actors, id, spot, 1);
    }

    private ZLinkActorRuntime.DefaultActorContext actor(
            ZLinkActorRuntime actors, String id, ZLinkSpot<?> spot, long generation) {
        var ref = new ZLinkBackendActorRef(RoutingId.from("node"), id, generation);
        var context = actors.new DefaultActorContext(ref);
        ZLinkActor actor = () -> context;
        context.setActor(actor);
        actors.actorRegistry.register(id, "type", actor, context);
        context.markJoined(ref, "spot", spot);
        return context;
    }
}
