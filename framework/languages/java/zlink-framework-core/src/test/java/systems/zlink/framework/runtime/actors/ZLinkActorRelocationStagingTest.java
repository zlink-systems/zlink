package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;

final class ZLinkActorRelocationStagingTest {
    @Test
    void preparedActorIsNotVisibleUntilAggregatePublication() {
        AtomicInteger destroys = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(destroys);

        var prepared = runtime.prepareRelocatedActor(
                "actor-a",
                "probe",
                new byte[0],
                false,
                null,
                () -> false,
                null)
            .toCompletableFuture().join();

        assertTrue(runtime.localActor("actor-a").isEmpty(),
            "factory completion must not expose partial target staging");
        assertSame(
            prepared.actor(),
            runtime.publishPreparedTransferredActor(prepared));
        assertSame(
            prepared.actor(),
            runtime.localActor("actor-a").orElseThrow());
        runtime.completePreparedTransferredActor(prepared);
        assertEquals(0, destroys.get());
    }

    @Test
    void preparedTransferredActorKeepsMoveCompletionPendingUntilAdmissionOpens() {
        ZLinkActorRuntime runtime = runtime(new AtomicInteger());
        var prepared = runtime.prepareRelocatedActor(
                "actor-admission-barrier",
                "probe",
                new byte[0],
                false,
                null,
                () -> false,
                null)
            .toCompletableFuture().join();

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
        var prepared = runtime.prepareRelocatedActor(
                "actor-b",
                "probe",
                new byte[0],
                false,
                null,
                () -> false,
                null)
            .toCompletableFuture().join();

        runtime.discardPreparedTransferredActor(prepared)
            .toCompletableFuture().join();

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
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkActorHandlerInstances.instance(
                prepared.actor(),
                CloseableProbeHandler.class);
        CompletableFuture<Void> release = new CompletableFuture<>();
        CompletionStage<Void> active = runtime.submitActorDispatch(
            prepared.actorId(),
            () -> release);

        CompletionStage<Void> cleanup =
            runtime.completeRelocationSource(prepared.actorId());

        assertFalse(cleanup.toCompletableFuture().isDone());
        assertEquals(0, closes.get());
        release.complete(null);
        CompletableFuture.allOf(
            active.toCompletableFuture(),
            cleanup.toCompletableFuture()).join();
        assertEquals(1, closes.get());
        assertTrue(runtime.localActor(prepared.actorId()).isEmpty());
    }

    @Test
    void localJoinSourceNotificationDoesNotBlockTargetCompletion() {
        ZLinkActorRuntime runtime = runtime(new AtomicInteger());
        var prepared = publishedActor(runtime, "actor-local-join");
        CompletableFuture<Void> sourceNotification = new CompletableFuture<>();
        runtime.setSourceActorLeaver(ignored -> sourceNotification);

        runtime.beginLocalMove(prepared.actor());
        ZLinkActorRuntime.LocalMoveSource source =
            new ZLinkActorRuntime.LocalMoveSource(new Object(), "source-spot");
        runtime.notifySourceForLocalMove(prepared.actor(), source);

        assertTrue(runtime.isMoving(prepared.actor()));
        runtime.completeRemoteMove(prepared.actor());
        assertFalse(runtime.isMoving(prepared.actor()));
        assertFalse(sourceNotification.isDone());

        sourceNotification.completeExceptionally(
            new IllegalStateException("source leave notification failed"));
        assertTrue(sourceNotification.isCompletedExceptionally());
        assertFalse(runtime.isMoving(prepared.actor()));
    }

    @Test
    void entrySpotDestroyWaitsForActiveActorTurn() {
        AtomicInteger closes = new AtomicInteger();
        AtomicInteger destroys = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(destroys, closes);
        var prepared = publishedActor(runtime, "actor-destroy");
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkActorHandlerInstances.instance(
                prepared.actor(),
                CloseableProbeHandler.class);
        CompletableFuture<Void> release = new CompletableFuture<>();
        CompletionStage<Void> active = runtime.submitActorDispatch(
            prepared.actorId(),
            () -> release);

        CompletionStage<Void> cleanup = runtime.destroyFromEntrySpot(
            RoutingId.from("node-a"),
            prepared.actor());

        assertFalse(cleanup.toCompletableFuture().isDone());
        assertEquals(0, closes.get());
        assertEquals(0, destroys.get());
        release.complete(null);
        CompletableFuture.allOf(
            active.toCompletableFuture(),
            cleanup.toCompletableFuture()).join();
        assertEquals(1, closes.get());
        assertEquals(1, destroys.get());
    }

    @Test
    void entrySpotMembershipDoesNotRequireDurableUserSpotAuthority() {
        ZLinkActorRuntime runtime = runtime(new AtomicInteger());
        ZLinkActor actor = runtime.getOrCreateLocalActor(
                "actor-entry", ProbeActor.class)
            .toCompletableFuture()
            .join()
            .orElseThrow();
        RoutingId entryNode = RoutingId.from("entry-node");
        ZLinkBackendActorRef actorRef = new ZLinkBackendActorRef(
            entryNode,
            "actor-entry",
            3);

        assertDoesNotThrow(() -> runtime.markJoinedEntrySpot(
            actor,
            actorRef,
            entryNode));
        assertEquals(entryNode, runtime.entrySpotNodeRid(actor));
        assertTrue(actor.context().spotId().isPresent());
    }

    @Test
    void ownershipLossCleanupWaitsForActiveActorTurn() {
        AtomicInteger closes = new AtomicInteger();
        AtomicInteger destroys = new AtomicInteger();
        ZLinkActorRuntime runtime = runtime(destroys, closes);
        var prepared = publishedActor(runtime, "actor-ownership");
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkActorHandlerInstances.instance(
                prepared.actor(),
                CloseableProbeHandler.class);
        CompletableFuture<Void> release = new CompletableFuture<>();
        CompletionStage<Void> active = runtime.submitActorDispatch(
            prepared.actorId(),
            () -> release);

        CompletionStage<Void> cleanup =
            runtime.deactivateActorOnOwnershipLoss(prepared.actorId());

        assertFalse(cleanup.toCompletableFuture().isDone());
        assertEquals(0, closes.get());
        assertEquals(0, destroys.get());
        release.complete(null);
        CompletableFuture.allOf(
            active.toCompletableFuture(),
            cleanup.toCompletableFuture()).join();
        assertEquals(1, closes.get());
        assertEquals(1, destroys.get());
    }

    private static ZLinkActorRuntime.PreparedTransferredActor publishedActor(
        ZLinkActorRuntime runtime,
        String actorId) {
        var prepared = runtime.prepareRelocatedActor(
                actorId,
                "probe",
                new byte[0],
                false,
                null,
                () -> false,
                null)
            .toCompletableFuture().join();
        runtime.publishPreparedTransferredActor(prepared);
        runtime.completePreparedTransferredActor(prepared);
        return prepared;
    }

    private static ZLinkActorRuntime runtime(AtomicInteger destroys) {
        return runtime(destroys, new AtomicInteger());
    }

    private static ZLinkActorRuntime runtime(
        AtomicInteger destroys,
        AtomicInteger closes) {
        ZLinkInternalSpotNode node = (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "routingId" -> RoutingId.from("node-a");
                case "createActor" -> {
                    ((systems.zlink.contracts.messaging.Message) arguments[1]).close();
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
                    return new CloseableProbeHandler(closes);
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
        public java.util.concurrent.CompletionStage<ZLinkActor> create(
            ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new ProbeActor(context));
        }
    }

    private record ProbeActor(ZLinkActorContext context) implements ZLinkActor {
    }

    public static final class CloseableProbeHandler implements AutoCloseable {
        private final AtomicInteger closes;

        private CloseableProbeHandler(AtomicInteger closes) {
            this.closes = closes;
        }

        @Override
        public void close() {
            closes.incrementAndGet();
        }
    }
}
