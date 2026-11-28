package systems.zlink.framework.runtime;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.params.ParameterizedTest;
import org.junit.jupiter.params.provider.EnumSource;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.binding.ZLinkJavaRawMeshNodeTestAccess;
import systems.zlink.framework.runtime.binding.ZLinkJavaRawSpotNodeTestAccess;
import systems.zlink.framework.runtime.diagnostics.ZLinkMessageFlowTracer;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationOutcome;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeTestAccess;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinatorTestAccess;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.InstanceRouteFence;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.InstanceSpotFollow;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec.SpotRouteFence;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntimeTestAccess;
import systems.zlink.framework.spots.ZLinkSpotPacketHandler;

import java.util.List;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.TimeoutException;
import java.util.logging.Handler;
import java.util.logging.LogRecord;
import java.util.logging.Logger;

final class InstanceSpotFollowPublicationIntegrationTest extends InstanceSpotHostRelocationFixture {
    private static final List<RoutingId> followOwners = new CopyOnWriteArrayList<>();
    private static final List<String> handledMessages = new CopyOnWriteArrayList<>();
    private static CompletableFuture<Void> followHandled;
    private static CompletableFuture<Void> secondHandled;

    enum Scenario {
        VALID_GENERATION,
        WRONG_GENERATION,
        READY_FOLLOW_BEHIND_STAGED_FOLLOW
    }

    InstanceSpotFollowPublicationIntegrationTest() {
        super(true);
    }

    @ParameterizedTest
    @EnumSource(Scenario.class)
    void instanceSpotFollowDuringCommittedPublicationBarrier(Scenario scenario) throws Exception {
        boolean wrongGeneration = scenario == Scenario.WRONG_GENERATION;
        followOwners.clear();
        handledMessages.clear();
        followHandled = new CompletableFuture<>();
        secondHandled = new CompletableFuture<>();
        Room.followHandlers = List.of(FollowHandler.class, SecondFollowHandler.class);
        var committed = new CompletableFuture<ZLinkAggregateCommitResult>();
        var release = new CompletableFuture<Void>();
        var construction =
                new ZLinkFrameworkRuntimeTestAccess.Construction()
                        .aggregateRelocationAuthorityStoreDecorator(
                                ZLinkAggregateRelocationCoordinatorTestAccess::interceptingStore);
        var flow = ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
        var drops = new CopyOnWriteArrayList<String>();
        var logger = Logger.getLogger(ZLinkMessageFlowTracer.class.getName());
        var listener =
                new Handler() {
                    @Override
                    public void publish(LogRecord record) {
                        var message = record.getMessage();
                        if (message.contains("phase=dropped") && message.contains(flow.flowId()))
                            drops.add(message);
                    }

                    @Override
                    public void flush() {}

                    @Override
                    public void close() {}
                };
        logger.addHandler(listener);
        try (var source = start(SOURCE);
                var caller = start(CLIENT)) {
            activateFrom(caller);
            var cached = route(awaitReadyAuthority());
            assertEquals(SOURCE, cached.targetNodeRid());
            try (var target = start(TARGET, construction)) {
                awaitPeer(source, TARGET);
                awaitPeer(target, SOURCE);
                var sourceNode = ZLinkFrameworkRuntimeTestAccess.meshNode(source, MESH);
                var callerNode = ZLinkFrameworkRuntimeTestAccess.meshNode(caller, MESH);
                var targetNode = ZLinkFrameworkRuntimeTestAccess.meshNode(target, MESH);
                var ingress =
                        ZLinkJavaRawSpotNodeTestAccess.relocationStagingIngressHandler(
                                targetNode.spotNode());
                var coordinator =
                        (ZLinkAggregateRelocationCoordinator)
                                ZLinkSpotRuntimeTestAccess.aggregateCoordinator(ingress);
                ZLinkAggregateRelocationCoordinatorTestAccess.interceptCommit(
                        coordinator,
                        (repository, arguments) ->
                                repository
                                        .commitAggregate(
                                                (ZLinkAggregateFence) arguments[0],
                                                (ZLinkStoreCancellation) arguments[1])
                                        .thenCompose(
                                                result -> {
                                                    committed.complete(result);
                                                    return release.thenApply(ignored -> result);
                                                }));
                try {
                    var relocation = relocate(source);
                    assertEquals(ZLinkAggregateCommitResult.COMMITTED, await(committed));
                    var live = route(authority());
                    assertEquals(TARGET, live.targetNodeRid());
                    assertEquals(cached.objectGeneration(), live.objectGeneration());
                    assertTrue(live.authorityOwnerGeneration() > cached.authorityOwnerGeneration());
                    assertEquals(
                            ZLinkFrameworkRelocationOutcome.RELOCATED, await(relocation).outcome());
                    var machine =
                            ZLinkSpotRuntimeTestAccess.firstRelocationStateMachine(
                                    ZLinkFrameworkRuntimeTestAccess.spotRetire(target));
                    var publication =
                            ZLinkSpotRuntimeTestAccess.firstTargetAttemptTerminal(machine);
                    assertFalse(publication.isDone(), "target publication must remain blocked");

                    if (scenario == Scenario.READY_FOLLOW_BEHIND_STAGED_FOLLOW) {
                        awaitPeer(caller, TARGET);
                        awaitPeer(target, CLIENT);
                        assertReadyFollowWaitsForStagedFollow(
                                callerNode, cached, flow, coordinator, release, publication);
                        assertEquals(List.of(), drops);
                        assertEquals(List.of(TARGET, TARGET), followOwners);
                        return;
                    }

                    if (wrongGeneration) {
                        sendWrongGeneration(sourceNode, cached, live, flow);
                    } else {
                        sendProbe(callerNode, cached, flow, FollowProbe.class);
                    }

                    long deadline = System.nanoTime() + DEADLINE.toNanos();
                    while (drops.isEmpty()
                            && (wrongGeneration
                                    || !ZLinkSpotRuntimeTestAccess.hasTemporarySpotIngress(
                                            ingress, SPOT_ID))
                            && followOwners.isEmpty()) {
                        assertTrue(
                                System.nanoTime() < deadline,
                                "Follow neither entered target staging nor produced a terminal");
                        Thread.sleep(2);
                    }
                    assertFalse(publication.isDone(), "Follow must arrive before publication");
                    assertEquals(List.of(), followOwners);
                    if (wrongGeneration) {
                        assertEquals(
                                1, drops.size(), "wrong generation must drop before publication");
                    }

                    release.complete(null);
                    await(publication);
                    if (!wrongGeneration && drops.isEmpty()) await(followHandled);
                    assertEquals(
                            wrongGeneration ? 1 : 0,
                            drops.size(),
                            () -> "Follow drops=" + drops + ", handlers=" + followOwners);
                    assertEquals(wrongGeneration ? List.of() : List.of(TARGET), followOwners);
                } finally {
                    release.complete(null);
                }
            }
        } finally {
            logger.removeHandler(listener);
        }
    }

    private static void sendWrongGeneration(
            ZLinkInternalMeshNode sourceNode,
            InstanceRouteFence cached,
            InstanceRouteFence live,
            ZLinkFlowContext.State flow)
            throws Exception {
        var wrong =
                new SpotRouteFence(
                        SPOT_ID,
                        live.objectGeneration(),
                        live.targetNodeRid(),
                        live.targetNodeGeneration(),
                        live.authorityOwnerGeneration() + 1,
                        live.leaseGeneration());
        var follow =
                new InstanceSpotFollow(
                        0, cached.targetNodeGeneration(), SOURCE, null, 0, cached.spotFence());
        try (var envelope = probeHeader(flow, FollowProbe.class);
                var body = Message.from("{}")) {
            await(
                    ZLinkJavaRawMeshNodeTestAccess.sendFrames(
                            sourceNode,
                            wrong,
                            ZLinkJavaRawMeshNodeTestAccess.instanceSpotFollowFrames(
                                    follow, wrong, List.of(envelope, body))));
        }
    }

    private void assertReadyFollowWaitsForStagedFollow(
            ZLinkInternalMeshNode callerNode,
            InstanceRouteFence cached,
            ZLinkFlowContext.State flow,
            ZLinkAggregateRelocationCoordinator coordinator,
            CompletableFuture<Void> releasePublication,
            CompletableFuture<Void> publication)
            throws Exception {
        var readEntered = new CompletableFuture<Void>();
        var releaseRead = new CompletableFuture<Void>();
        ZLinkAggregateRelocationCoordinatorTestAccess.interceptRead(
                coordinator,
                (repository, arguments) -> {
                    var key = (String) arguments[0];
                    var read = repository.read(key, (ZLinkStoreCancellation) arguments[1]);
                    return AUTHORITY.equals(key) && readEntered.complete(null)
                            ? read.thenCombine(releaseRead, (result, ignored) -> result)
                            : read;
                });
        try {
            sendProbe(callerNode, cached, flow, FollowProbe.class);
            await(readEntered);
            releasePublication.complete(null);
            await(publication);
            sendProbe(callerNode, route(authority()), flow, SecondFollowProbe.class);
            assertThrows(
                    TimeoutException.class,
                    () -> secondHandled.get(1, TimeUnit.SECONDS),
                    "Ready message B entered its handler while Follow A was awaiting the Store");
        } finally {
            releaseRead.complete(null);
        }
        await(secondHandled);
        assertEquals(List.of("A", "B"), handledMessages);
    }

    private static void sendProbe(
            ZLinkInternalMeshNode callerNode,
            InstanceRouteFence route,
            ZLinkFlowContext.State flow,
            Class<?> packet)
            throws Exception {
        try (var envelope = probeHeader(flow, packet);
                var body = Message.from("{}")) {
            await(
                    callerNode.submitInstanceSpotSend(
                            route, SPOT_TYPE, null, new byte[0], List.of(envelope, body)));
        }
    }

    private static Message probeHeader(ZLinkFlowContext.State flow, Class<?> packet) {
        return ZLinkChannelEnvelope.encodeHeader(
                ZLinkChannelEnvelope.create(
                        ZLinkChannelEnvelope.KIND_COMMAND,
                        "",
                        packet.getSimpleName(),
                        "application/json",
                        null,
                        Map.of(),
                        flow));
    }

    private static InstanceRouteFence route(ZLinkAuthoritySnapshot snapshot) {
        var allocation = snapshot.allocation();
        return new InstanceRouteFence(
                allocation.descriptor().rid(),
                allocation.descriptorLifecycleGeneration(),
                SPOT_ID,
                snapshot.objectGeneration(),
                snapshot.ownerId(),
                snapshot.authorityOwnerGeneration(),
                snapshot.ownerLeaseGeneration(),
                snapshot.storeVersion());
    }

    public record FollowProbe() {}

    public record SecondFollowProbe() {}

    public static final class FollowHandler implements ZLinkSpotPacketHandler<Room, FollowProbe> {
        @Override
        public CompletionStage<Void> handle(Room spot, FollowProbe message) {
            followOwners.add(spot.context().nodeRid());
            handledMessages.add("A");
            followHandled.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class SecondFollowHandler
            implements ZLinkSpotPacketHandler<Room, SecondFollowProbe> {
        @Override
        public CompletionStage<Void> handle(Room spot, SecondFollowProbe message) {
            followOwners.add(spot.context().nodeRid());
            handledMessages.add("B");
            secondHandled.complete(null);
            return CompletableFuture.completedFuture(null);
        }
    }
}
