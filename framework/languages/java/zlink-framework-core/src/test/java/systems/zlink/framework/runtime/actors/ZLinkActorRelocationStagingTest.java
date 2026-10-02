package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;

final class ZLinkActorRelocationStagingTest {
    @Test
    void preparedActorIsNotVisibleUntilAggregatePublication() {
        AtomicInteger destroys = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(destroys);

        var prepared =
                runtime.prepareRelocatedActor(
                                "actor-a", "probe", new byte[0], false, null, () -> false, null)
                        .toCompletableFuture()
                        .join();

        assertTrue(
                runtime.localActor("actor-a").isEmpty(),
                "factory completion must not expose partial target staging");
        assertSame(prepared.actor(), runtime.publishPreparedTransferredActor(prepared));
        assertSame(prepared.actor(), runtime.localActor("actor-a").orElseThrow());
        runtime.completePreparedTransferredActor(prepared);
        assertEquals(0, destroys.get());
    }

    @Test
    void preparedTransferredActorKeepsMoveCompletionPendingUntilAdmissionOpens() {
        ZLinkActorRuntime runtime = runtime(new AtomicInteger());
        var prepared =
                runtime.prepareRelocatedActor(
                                "actor-admission-barrier",
                                "probe",
                                new byte[0],
                                false,
                                null,
                                () -> false,
                                null)
                        .toCompletableFuture()
                        .join();

        runtime.publishPreparedTransferredActor(prepared);
        CompletionStage<Void> completion = runtime.awaitMoveCompletion(prepared.actor());

        assertFalse(completion.toCompletableFuture().isDone());
        assertTrue(runtime.isMoving(prepared.actor()));

        runtime.completePreparedTransferredActor(prepared);

        completion.toCompletableFuture().join();
        assertFalse(runtime.isMoving(prepared.actor()));
    }

    @Test
    void discardedActorNeverEntersRegistryAndReleasesBackendResource() {
        AtomicInteger destroys = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(destroys);
        var prepared =
                runtime.prepareRelocatedActor(
                                "actor-b", "probe", new byte[0], false, null, () -> false, null)
                        .toCompletableFuture()
                        .join();

        runtime.discardPreparedTransferredActor(prepared).toCompletableFuture().join();

        assertTrue(runtime.localActor("actor-b").isEmpty());
        assertEquals(1, destroys.get());
        assertThrows(
                IllegalStateException.class,
                () -> runtime.publishPreparedTransferredActor(prepared));
    }

    @Test
    void relocationSourceCleanupWaitsForActiveActorTurn() {
        AtomicInteger closes = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(new AtomicInteger(), closes);
        var prepared = publishedActor(runtime, "actor-relocation");
        systems.zlink.framework.runtime.internal.handlers.ZLinkActorHandlerInstances.instance(
                prepared.actor(), CloseableProbeHandler.class);
        CompletableFuture<Void> release = new CompletableFuture<>();
        CompletionStage<Void> active =
                runtime.submitActorDispatch(prepared.actorId(), () -> release);

        CompletionStage<Void> cleanup = runtime.completeRelocationSource(prepared.actorId());

        assertFalse(cleanup.toCompletableFuture().isDone());
        assertEquals(0, closes.get());
        release.complete(null);
        CompletableFuture.allOf(active.toCompletableFuture(), cleanup.toCompletableFuture()).join();
        assertEquals(1, closes.get());
        assertTrue(runtime.localActor(prepared.actorId()).isEmpty());
    }

    @Test
    void relocationSourceHandlerFailureStillClearsContextAndRestoresFreshInstance() {
        AtomicInteger closes = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(new AtomicInteger(), closes, true);
        var prepared = publishedActor(runtime, "actor-failed-retirement");
        runtime.markJoinedEntrySpot(
                prepared.actor(), prepared.actorRef(), RoutingId.from("node-a"));
        systems.zlink.framework.runtime.internal.handlers.ZLinkActorHandlerInstances.instance(
                prepared.actor(), CloseableProbeHandler.class);

        assertThrows(
                java.util.concurrent.CompletionException.class,
                () ->
                        runtime.completeRelocationSource(prepared.actorId())
                                .toCompletableFuture()
                                .join());

        assertTrue(runtime.localActor(prepared.actorId()).isEmpty());
        assertTrue(
                prepared.actor().context().spotId().isEmpty(),
                "retirement must clear context after handler failure");
        runtime.completeRelocationSource(prepared.actorId()).toCompletableFuture().join();
        assertEquals(1, closes.get(), "failed source cleanup must not run again");
        var restored = publishedActor(runtime, prepared.actorId());
        assertNotSame(prepared.actor(), restored.actor());
        assertEquals(
                prepared.actorRef().generation(), restored.actor().context().objectGeneration());
    }

    @Test
    void idleActorWithoutMessageFollowDoesNotBlockShutdownClose() {
        AtomicInteger closes = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(new AtomicInteger(), closes);
        var prepared = publishedActor(runtime, "actor-shutdown");
        systems.zlink.framework.runtime.internal.handlers.ZLinkActorHandlerInstances.instance(
                prepared.actor(), CloseableProbeHandler.class);

        assertTrue(runtime.awaitDrainBarrier().toCompletableFuture().isDone());
        assertTrue(
                runtime.drainComplete(),
                "an idle Actor without a Message Follow source is not unfinished accepted work");

        CompletionStage<Void> close = runtime.closeAsync();
        assertSame(close, runtime.closeAsync(), "Actor teardown has one completion owner");
        close.toCompletableFuture().join();
        assertEquals(1, closes.get());
        assertTrue(runtime.localActor(prepared.actorId()).isEmpty());
    }

    @Test
    void entrySpotDestroyWaitsForActiveActorTurn() {
        AtomicInteger closes = new AtomicInteger();
        AtomicInteger destroys = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(destroys, closes);
        var prepared = publishedActor(runtime, "actor-destroy");
        systems.zlink.framework.runtime.internal.handlers.ZLinkActorHandlerInstances.instance(
                prepared.actor(), CloseableProbeHandler.class);
        CompletableFuture<Void> release = new CompletableFuture<>();
        CompletionStage<Void> active =
                runtime.submitActorDispatch(prepared.actorId(), () -> release);

        CompletionStage<Void> cleanup =
                runtime.destroyFromEntrySpot(RoutingId.from("node-a"), prepared.actor());

        assertFalse(cleanup.toCompletableFuture().isDone());
        assertEquals(0, closes.get());
        assertEquals(0, destroys.get());
        release.complete(null);
        CompletableFuture.allOf(active.toCompletableFuture(), cleanup.toCompletableFuture()).join();
        assertEquals(1, closes.get());
        assertEquals(1, destroys.get());
    }

    @Test
    void entrySpotMembershipDoesNotRequireDurableUserSpotAuthority() {
        ZLinkActorRuntime runtime = runtime(new AtomicInteger());
        ZLinkActor actor =
                runtime.getOrCreateLocalActor("actor-entry", ProbeActor.class)
                        .toCompletableFuture()
                        .join()
                        .orElseThrow();
        RoutingId entryNode = RoutingId.from("entry-node");
        ZLinkBackendActorRef actorRef = new ZLinkBackendActorRef(entryNode, "actor-entry", 3);

        assertDoesNotThrow(() -> runtime.markJoinedEntrySpot(actor, actorRef, entryNode));
        assertEquals(entryNode, runtime.entrySpotNodeRid(actor));
        assertTrue(actor.context().spotId().isPresent());
    }

    @Test
    void ownershipLossCleanupWaitsForActiveActorTurn() {
        AtomicInteger closes = new AtomicInteger();
        AtomicInteger destroys = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(destroys, closes);
        var prepared = publishedActor(runtime, "actor-ownership");
        systems.zlink.framework.runtime.internal.handlers.ZLinkActorHandlerInstances.instance(
                prepared.actor(), CloseableProbeHandler.class);
        CompletableFuture<Void> release = new CompletableFuture<>();
        CompletionStage<Void> active =
                runtime.submitActorDispatch(prepared.actorId(), () -> release);

        CompletionStage<Void> cleanup = runtime.deactivateActorOnOwnershipLoss(prepared.actorId());

        assertFalse(cleanup.toCompletableFuture().isDone());
        assertEquals(0, closes.get());
        assertEquals(0, destroys.get());
        release.complete(null);
        CompletableFuture.allOf(active.toCompletableFuture(), cleanup.toCompletableFuture()).join();
        assertEquals(1, closes.get());
        assertEquals(1, destroys.get());
    }

    @Test
    void boundSessionSendWithoutFenceAtMovingBoundaryIsRetryableUnavailable() {
        ZLinkActorRuntime runtime = runtime(new AtomicInteger());
        var prepared = publishedActor(runtime, "actor-moving-fence");
        runtime.beginRemoteMove(prepared.actor()).toCompletableFuture().join();
        assertTrue(runtime.isMoving(prepared.actor()));
        try (Message payload = Message.from(new byte[] {1})) {
            ZLinkFrameworkException failure =
                    assertThrows(
                            ZLinkFrameworkException.class,
                            () ->
                                    runtime.captureMovingPacket(
                                            prepared.actor(),
                                            new ZLinkStreamHeader(
                                                    "probe-send", Map.of(), Optional.empty()),
                                            payload));
            // Pre-Captured boundary: the frame was never accepted, so the
            // caller must observe the retryable moving/unavailable terminal
            // (matching the .NET/Node contract), not a configuration failure.
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, failure.kind());
            assertFalse(failure instanceof ZLinkConfigurationException);
            assertTrue(failure.getMessage().contains("is moving"));
            assertTrue(failure.getMessage().contains("an exact accepted journal fence"));
        }
    }

    private static ZLinkActorRuntime.PreparedTransferredActor publishedActor(
            ZLinkActorRuntime runtime, String actorId) {
        var prepared =
                runtime.prepareRelocatedActor(
                                actorId, "probe", new byte[0], false, null, () -> false, null)
                        .toCompletableFuture()
                        .join();
        runtime.publishPreparedTransferredActor(prepared);
        runtime.completePreparedTransferredActor(prepared);
        return prepared;
    }

    private static ZLinkActorRuntime runtime(AtomicInteger destroys) {
        return runtime(destroys, new AtomicInteger());
    }

    private static ZLinkActorRuntime runtime(AtomicInteger destroys, AtomicInteger closes) {
        return runtime(destroys, closes, false);
    }

    private static ZLinkActorRuntime runtime(
            AtomicInteger destroys, AtomicInteger closes, boolean failClose) {
        ZLinkInternalSpotNode node =
                (ZLinkInternalSpotNode)
                        Proxy.newProxyInstance(
                                ZLinkInternalSpotNode.class.getClassLoader(),
                                new Class<?>[] {ZLinkInternalSpotNode.class},
                                (proxy, method, arguments) ->
                                        switch (method.getName()) {
                                            case "routingId" -> RoutingId.from("node-a");
                                            case "createActor" -> {
                                                ((Message) arguments[1]).close();
                                                yield new ZLinkBackendActorRef(
                                                        RoutingId.from("node-a"),
                                                        (String) arguments[0],
                                                        7);
                                            }
                                            case "destroyActor" -> {
                                                destroys.incrementAndGet();
                                                yield CompletableFuture.completedFuture(null);
                                            }
                                            case "close" -> null;
                                            default -> defaultValue(method.getReturnType());
                                        });
        return new ZLinkActorRuntime(
                node,
                Map.of("probe", ProbeFactory.class),
                Duration.ofSeconds(5),
                new ZLinkJsonMessageSerializer(),
                handlerType -> {
                    if (handlerType == CloseableProbeHandler.class) {
                        return new CloseableProbeHandler(closes, failClose);
                    }
                    return ZLinkHandlerActivator.reflection().create(handlerType);
                });
    }

    private static Object defaultValue(Class<?> type) {
        if (!type.isPrimitive()) return null;
        if (type == boolean.class) return false;
        if (type == byte.class) return (byte) 0;
        if (type == short.class) return (short) 0;
        if (type == int.class) return 0;
        if (type == long.class) return 0L;
        if (type == float.class) return 0F;
        if (type == double.class) return 0D;
        if (type == char.class) return '\0';
        return null;
    }

    public static final class ProbeFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new ProbeActor(context));
        }
    }

    private record ProbeActor(ZLinkActorContext context) implements ZLinkActor {}

    public static final class CloseableProbeHandler implements AutoCloseable {
        private final AtomicInteger closes;
        private final boolean failClose;

        private CloseableProbeHandler(AtomicInteger closes, boolean failClose) {
            this.closes = closes;
            this.failClose = failClose;
        }

        @Override
        public void close() {
            closes.incrementAndGet();
            if (failClose) {
                throw new IllegalStateException("injected source handler close failure");
            }
        }
    }
}
