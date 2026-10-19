package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTimeoutPreemptively;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.RepeatedTest;
import org.junit.jupiter.api.Test;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.*;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget.QueuedTurn;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.binding.ZLinkRelocationRawSessionFixture;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;

import java.lang.reflect.Proxy;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.function.Supplier;

final class ZLinkUserSpotRetireTargetEndpointTest {
    private static final ZLinkStoreCancellation OPEN = () -> false;

    @Test
    void command46MustCloseTheExactRequestSourceFence() {
        var relay =
                new systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec
                        .ReplyRelay(
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceRelocationWireCodec.Operation(1, 2),
                        3,
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceRelocationWireCodec.RelocationId(4, 5),
                        6,
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceRelocationWireCodec.CoordinatorFence(
                                "target-owner",
                                7,
                                RoutingId.from("target-node"),
                                8,
                                "store-version"),
                        9,
                        10,
                        0,
                        0);
        var expectedSource =
                new systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec
                        .RequestSourceFence("source-owner", 11, RoutingId.from("source-node"), 12);
        var wrongLease =
                new systems.zlink.framework.runtime.internal.service.ZLinkServiceRelocationWireCodec
                        .ReplyRelayAck(
                        relay.relocation(),
                        relay.coordinator(),
                        relay.operation(),
                        relay.replyRouteId(),
                        new systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceRelocationWireCodec.RequestSourceFence(
                                "source-owner", 13, RoutingId.from("source-node"), 12),
                        1);

        assertThrows(
                IllegalArgumentException.class,
                () ->
                        ZLinkUserSpotRetireTargetEndpoint.validateCanonicalAck(
                                relay, expectedSource, wrongLease));
    }

    @Test
    void publishedStandaloneActorKeepsFifoClosedWhileNormalizationIsPending() {
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        RoutingId sessionOwnerRid = RoutingId.from("session-owner");
        RoutingId sessionRid = RoutingId.from("session-a");
        long targetNodeGeneration = 17;
        String actorId = "actor-a";
        String actorAuthorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        UUID relocationId = UUID.randomUUID();
        var participants =
                List.of(
                        new ZLinkSpotRetireControl.ParticipantFence(
                                actorAuthorityKey,
                                ZLinkPlacementObjectKind.ACTOR.value(),
                                actorId,
                                "player",
                                true,
                                4,
                                7));
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId, actorId, 4, 7, true, new byte[] {1}, List.of());
        AuthorityState authority = new AuthorityState();
        ZLinkLocationRepository authorityStore = authority.proxy();
        var coordinator = new ZLinkAggregateRelocationCoordinator(authorityStore);
        var prepared =
                coordinator
                        .prepare(
                                new ZLinkAggregateRelocationCoordinator.Request(
                                        relocationId,
                                        1,
                                        1,
                                        List.of(
                                                new ZLinkAggregateRelocationCoordinator.Participant(
                                                        actorAuthorityKey,
                                                        ZLinkPlacementObjectKind.ACTOR,
                                                        4,
                                                        7,
                                                        "actor-version-1",
                                                        ZLinkAuthorityGenerationTransition
                                                                .NEW_OWNER,
                                                        new ZLinkActorAuthorityPayloadCodec()
                                                                .encode(
                                                                        ZLinkActorAuthorityPayloadCodec
                                                                                .State.READY,
                                                                        "player",
                                                                        actorId,
                                                                        "room-a",
                                                                        1,
                                                                        1,
                                                                        "source-owner",
                                                                        12,
                                                                        "mesh",
                                                                        sourceRid,
                                                                        11),
                                                        new byte[0])),
                                        root,
                                        new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
                                        targetNodeGeneration,
                                        new ZLinkPlacementCapacityBundle(1, 0, Optional.empty()),
                                        new ZLinkLocationOwnerToken("target-owner", 23),
                                        "actor-version-1"),
                                OPEN)
                        .toCompletableFuture()
                        .join();
        List<String> operations = new CopyOnWriteArrayList<>();
        var actorBackend = new RestartActorBackend(operations);
        var actorStaging = new ZLinkStandaloneActorRelocationStagingOwner(actorBackend);
        CompletableFuture<Void> bindingRestore = new CompletableFuture<>();
        CompletableFuture<Void> normalization = new CompletableFuture<>();
        CompletableFuture<Void> normalizationEntered = new CompletableFuture<>();
        var endpoint =
                new ZLinkUserSpotRetireTargetEndpoint(
                        targetRid,
                        targetNodeGeneration,
                        coordinator,
                        new ZLinkUserSpotAggregateStagingOwner(new FakeStagingBackend()),
                        ignored -> TestSpot.class,
                        (lane, queued) ->
                                CompletableFuture.failedFuture(
                                        new AssertionError("standalone Actor root has no backlog")),
                        new ZLinkSessionRelocationPeerClient(
                                sessionRouteNode(new ZLinkServiceM6BWireCodec(), operations)),
                        Duration.ofSeconds(1),
                        ignored -> {
                            operations.add("normalize");
                            normalizationEntered.complete(null);
                            return normalization;
                        },
                        null,
                        null,
                        authorityStore,
                        actorStaging,
                        null,
                        (req, generations) -> {
                            assertEquals(java.util.Set.of(actorId), generations.keySet());
                            operations.add("install-target-binding");
                            return bindingRestore;
                        });
        var request =
                new ZLinkSpotRetireControl.StageRequest(
                        new ZLinkSpotRetireControl.Fence(relocationId, 1),
                        sourceRid,
                        11,
                        "source-owner",
                        12,
                        targetRid,
                        targetNodeGeneration,
                        "target-owner",
                        23,
                        "mesh",
                        "room-a",
                        "player",
                        false,
                        true,
                        root,
                        participants,
                        List.of(
                                new ZLinkSpotRetireControl.SessionRouteFence(
                                        actorId,
                                        4,
                                        7,
                                        "actor-version-1",
                                        sessionOwnerRid,
                                        31,
                                        "session-owner-id",
                                        32,
                                        sessionRid,
                                        33)));

        endpoint.stage(request).toCompletableFuture().join();
        coordinator.commit(prepared, OPEN).toCompletableFuture().join();
        authority.reads.set(0);
        CompletionStage<Void> publishing = endpoint.publish(request);
        actorBackend.drain();
        assertFalse(publishing.toCompletableFuture().isDone());
        assertFalse(
                operations.contains("command44"), "route switch must await binding installation");
        assertFalse(normalizationEntered.isDone());
        bindingRestore.complete(null);
        actorBackend.drain();

        var terminal = new CompletableFuture<Void>();
        try {
            assertTrue(
                    operations.indexOf("install-target-binding") >= 0,
                    "missing target binding: " + operations);
            assertTrue(
                    operations.indexOf("install-target-binding") < operations.indexOf("command44"),
                    "binding must precede route switch: " + operations);
            assertTrue(normalizationEntered.isDone());
            assertEquals(
                    1,
                    authority.reads.get(),
                    "publication and command44 must share one committed owner read");
            assertFalse(publishing.toCompletableFuture().isDone());
            var source =
                    new ZLinkInternalMeshNode.PeerAuthorityFence(sourceRid, 11, "source-owner", 12);
            var header =
                    new ZLinkServiceM6BWireCodec.ActorMessage(
                            false,
                            0,
                            null,
                            0,
                            0,
                            1,
                            null,
                            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                                    new ZLinkBackendActorRef(targetRid, actorId, 4),
                                    targetNodeGeneration,
                                    8,
                                    23));
            assertFalse(
                    endpoint.handleActor(
                            source,
                            header,
                            () -> new byte[] {1},
                            List.of(),
                            null,
                            ignored -> {},
                            ignored -> {}),
                    "an adopted published stage must leave ingress to the live Actor");

            var admission = new CompletableFuture<Void>();
            var handled = new CompletableFuture<Void>();
            actorBackend.actorRuntime.submitActorDispatch(
                    null,
                    actorId,
                    0,
                    () -> {
                        handled.complete(null);
                        return terminal;
                    },
                    admission,
                    null,
                    null);
            actorBackend.drain();
            assertTrue(admission.isDone() && !admission.isCompletedExceptionally());
            assertFalse(
                    handled.isDone(),
                    "ordinary Actor handler must wait for route and normalization finalization");
            assertEquals(
                    List.of(
                            "prepare-actor",
                            "publish-actor",
                            "install-target-binding",
                            "command44",
                            "normalize"),
                    operations);
            normalization.complete(null);
            actorBackend.drain();
            publishing.toCompletableFuture().join();
            assertTrue(handled.isDone());
            assertFalse(terminal.isDone(), "publication must not await handler terminal");
            terminal.complete(null);
            actorBackend.drain();
        } finally {
            terminal.complete(null);
            normalization.complete(null);
            actorBackend.drain();
            var closed = actorBackend.actorRuntime.closeAsync().toCompletableFuture();
            actorBackend.drain();
            closed.join();
            actorBackend.rawSession.close();
        }
    }

    @Test
    void asynchronousActorPreparationPreservesTheAdoptedPrefixBeforeOrdinaryIngress() {
        var order = new ArrayList<String>();
        RestartActorBackend backend =
                new RestartActorBackend(new ArrayList<>()) {
                    @Override
                    public CompletionStage<Optional<byte[]>> replay(
                            Object actor,
                            ZLinkStandaloneActorRelocationStagingOwner.Request request,
                            ZLinkActorAcceptedJournal.Record record) {
                        order.add(record.header().packetName());
                        return CompletableFuture.completedFuture(Optional.empty());
                    }
                };
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        var relocationId = UUID.randomUUID();
        byte[] saved =
                ZLinkAcceptedJournalTestRecords.actor("actor-a", 1, "saved", Map.of(), new byte[0]);
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId,
                        "actor-a",
                        4,
                        7,
                        false,
                        new byte[0],
                        List.of(new ZLinkSerialExecutionQueue.QueuedRecord(1, saved)));
        var request =
                new ZLinkStandaloneActorRelocationStagingOwner.Request(
                        relocationId, "actor-a", "player", 4, 7, false, "room-a");
        var staged = owner.stage(request, root).toCompletableFuture().join();
        owner.stageRelayedRecord(
                        staged,
                        ZLinkAcceptedJournalTestRecords.actor(
                                "actor-a", 2, "relay", Map.of(), new byte[0]))
                .toCompletableFuture()
                .join();
        owner.acceptIngress(
                staged,
                ZLinkAcceptedJournalTestRecords.actor(
                        "actor-a", 3, "temporary", Map.of(), new byte[0]),
                null,
                error -> {
                    throw new AssertionError(error);
                });
        var backlog =
                owner.closeDurableBacklog(
                        staged,
                        root,
                        (lane, queued) ->
                                owner.replayActor(
                                                staged,
                                                ZLinkActorAcceptedJournal.decode(queued.payload()))
                                        .thenApply(reply -> null));
        owner.publishHidden(backlog, 8);
        var firstResolve =
                new CompletableFuture<
                        systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget
                                .ActivationSnapshot>();
        var calls = new AtomicInteger();
        var snapshot =
                new systems.zlink.framework.runtime.actors.ZLinkActorDispatchTarget
                        .ActivationSnapshot(null, backend.prepared.actor().context());
        backend.actorRuntime.setActorDispatchTargetResolver(
                actorId ->
                        calls.getAndIncrement() == 0
                                ? firstResolve
                                : CompletableFuture.completedFuture(snapshot));
        try {
            var adopted = owner.drainDurableBacklog(backlog).toCompletableFuture();
            assertFalse(
                    adopted.isDone(),
                    "first accepted record is still preparing its actual FIFO target");
            boolean stagedLate =
                    owner.acceptIngress(
                            staged,
                            ZLinkAcceptedJournalTestRecords.actor(
                                    "actor-a", 4, "late", Map.of(), new byte[0]),
                            null,
                            error -> {
                                throw new AssertionError(error);
                            });
            if (!stagedLate)
                backend.actorRuntime.submitActorDispatch(
                        "actor-a",
                        0,
                        () -> {
                            order.add("late");
                            return CompletableFuture.completedFuture(null);
                        });
            firstResolve.complete(snapshot);
            adopted.join();
            owner.openAdmission(staged);
            backend.drain();
            assertEquals(List.of("saved", "relay", "temporary", "late"), order);
        } finally {
            firstResolve.complete(snapshot);
            if (!backend.operations.contains("open")) backend.openAdmission(backend.prepared);
            backend.drain();
            var closed = backend.actorRuntime.closeAsync().toCompletableFuture();
            backend.drain();
            closed.join();
            backend.rawSession.close();
        }
    }

    @Test
    void adoptedActorRecordRunsInsideTheExistingDeferredJoinHandlerScope() {
        var registered = new CompletableFuture<Void>();
        var handler = new CompletableFuture<Optional<byte[]>>();
        var observed = new java.util.concurrent.atomic.AtomicReference<Throwable>();
        RestartActorBackend backend =
                new RestartActorBackend(new ArrayList<>()) {
                    @Override
                    public CompletionStage<Optional<byte[]>> replay(
                            Object actor,
                            ZLinkStandaloneActorRelocationStagingOwner.Request request,
                            ZLinkActorAcceptedJournal.Record record) {
                        ((ZLinkActorRuntime.PreparedTransferredActor) actor)
                                .actor()
                                .context()
                                .joinSpot("next")
                                .defer();
                        registered.complete(null);
                        return handler;
                    }
                };
        var owner = new ZLinkStandaloneActorRelocationStagingOwner(backend);
        var relocationId = UUID.randomUUID();
        byte[] saved =
                ZLinkAcceptedJournalTestRecords.actor(
                        "actor-a", 1, "deferred", Map.of(), new byte[0]);
        byte[] root =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                        relocationId,
                        "actor-a",
                        4,
                        7,
                        false,
                        new byte[0],
                        List.of(new ZLinkSerialExecutionQueue.QueuedRecord(1, saved)));
        var request =
                new ZLinkStandaloneActorRelocationStagingOwner.Request(
                        relocationId, "actor-a", "player", 4, 7, false, "room-a");
        var staged = owner.stage(request, root).toCompletableFuture().join();
        var backlog =
                owner.closeDurableBacklog(
                        staged,
                        root,
                        (lane, queued) ->
                                owner.replayActor(
                                                staged,
                                                ZLinkActorAcceptedJournal.decode(queued.payload()))
                                        .whenComplete(
                                                (reply, failure) -> {
                                                    if (failure != null) observed.set(failure);
                                                })
                                        .thenApply(reply -> null));
        owner.publishHidden(backlog, 8);
        try {
            var publication = owner.drainDurableBacklog(backlog).toCompletableFuture();
            publication.join();
            assertFalse(registered.isDone());
            owner.openAdmission(staged);
            var outside =
                    assertThrows(
                            systems.zlink.framework.errors.ZLinkFrameworkException.class,
                            () -> backend.prepared.actor().context().joinSpot("outside").defer());
            assertTrue(outside.getMessage().contains("open Framework handler scope"));
            backend.drain();
            assertTrue(
                    registered.isDone(),
                    "adopted record must retain the actual Actor handler scope");
            assertFalse(handler.isDone());
            var failure = new IllegalStateException("handler terminal after deferred registration");
            handler.completeExceptionally(failure);
            backend.drain();
            assertSame(failure, observed.get());
        } finally {
            handler.completeExceptionally(new IllegalStateException("test cleanup"));
            if (!backend.operations.contains("open")) backend.openAdmission(backend.prepared);
            backend.drain();
            var closed = backend.actorRuntime.closeAsync().toCompletableFuture();
            backend.drain();
            closed.join();
            backend.rawSession.close();
        }
    }

    @RepeatedTest(20)
    void targetRestoresDurableBacklogOnlyAfterPublicationAndLifecycleOpen() {
        assertTargetRestoresDurableBacklog(false);
    }

    @Test
    void publishedTargetRetainsSourceReplyCapabilityUntilAdoptedHandlerFails() {
        assertTargetRestoresDurableBacklog(true);
    }

    @Test
    void aggregateActorCannotRunBeforeFinalSetupWithAnEagerActorExecutor() {
        assertTargetRestoresDurableBacklog(false, true);
    }

    private void assertTargetRestoresDurableBacklog(boolean failHandler) {
        assertTargetRestoresDurableBacklog(failHandler, false);
    }

    private void assertTargetRestoresDurableBacklog(boolean failHandler, boolean eagerActorDrain) {
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId targetRid = RoutingId.from("target-node");
        long targetNodeGeneration = 17;
        String spotId = "room-a";
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.spot(spotId);
        String actorAuthorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        AuthorityState authority = new AuthorityState();
        ZLinkLocationRepository authorityStore = authority.proxy();
        var coordinator = new ZLinkAggregateRelocationCoordinator(authorityStore);
        var envelope =
                new ZLinkUserSpotAggregateStagingOwner.Request(
                        TestSpot.class,
                        "room",
                        spotId,
                        3,
                        new byte[] {1},
                        true,
                        new byte[0],
                        List.of(
                                new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                                        actorId,
                                        "player",
                                        new byte[0],
                                        false,
                                        new ZLinkBackendActorRef(targetRid, actorId, 4))),
                        Map.of(
                                "actor:" + actorId,
                                List.of(
                                        new ZLinkSerialExecutionQueue.QueuedRecord(
                                                1,
                                                ZLinkAcceptedJournalTestRecords.actor(
                                                        actorId,
                                                        0,
                                                        "first-push",
                                                        Map.of(),
                                                        new byte[] {1}))),
                                "spot",
                                List.of(
                                        new ZLinkSerialExecutionQueue.QueuedRecord(
                                                1, acceptedSpotRecord(spotId, 1)),
                                        new ZLinkSerialExecutionQueue.QueuedRecord(
                                                2, acceptedSpotRecord(spotId, 2)),
                                        new ZLinkSerialExecutionQueue.QueuedRecord(
                                                3, acceptedSpotRecord(spotId, 3)))));
        UUID aggregateId = UUID.randomUUID();
        var participants =
                List.of(
                        new ZLinkSpotRetireControl.ParticipantFence(
                                actorAuthorityKey, 1, actorId, "player", false, 4, 7),
                        new ZLinkSpotRetireControl.ParticipantFence(
                                authorityKey, 2, spotId, "room", true, 3, 5));
        var prepared =
                coordinator
                        .prepare(
                                new ZLinkAggregateRelocationCoordinator.Request(
                                        aggregateId,
                                        1,
                                        1,
                                        List.of(
                                                new ZLinkAggregateRelocationCoordinator.Participant(
                                                        actorAuthorityKey,
                                                        ZLinkPlacementObjectKind.ACTOR,
                                                        4,
                                                        7,
                                                        "actor-version-1",
                                                        ZLinkAuthorityGenerationTransition
                                                                .NEW_OWNER,
                                                        sourceAuthority(actorId, sourceRid),
                                                        new byte[0]),
                                                new ZLinkAggregateRelocationCoordinator.Participant(
                                                        authorityKey,
                                                        ZLinkPlacementObjectKind.USER_SPOT,
                                                        3,
                                                        5,
                                                        "version-1",
                                                        ZLinkAuthorityGenerationTransition
                                                                .NEW_OWNER,
                                                        sourceAuthority(spotId, sourceRid),
                                                        new byte[0])),
                                        ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                                                envelope, aggregateId, 5, participants),
                                        new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
                                        targetNodeGeneration,
                                        new ZLinkPlacementCapacityBundle(
                                                1,
                                                1,
                                                Optional.of(
                                                        new ZLinkSpotTypeCapacityDelta(
                                                                ZLinkPlacementObjectKind.USER_SPOT,
                                                                "room",
                                                                1))),
                                        new ZLinkLocationOwnerToken("target-owner", 23),
                                        "version-1"),
                                OPEN)
                        .toCompletableFuture()
                        .join();
        FakeStagingBackend backend = new FakeStagingBackend();
        var staging = new ZLinkUserSpotAggregateStagingOwner(backend);
        RoutingId sessionOwnerRid = RoutingId.from("session-owner");
        RoutingId sessionRid = RoutingId.from("session-a");
        var wire = new ZLinkServiceM6BWireCodec();
        ZLinkInternalMeshNode peer = sessionRouteNode(wire, backend.operations);
        var endpoint =
                new ZLinkUserSpotRetireTargetEndpoint(
                        targetRid,
                        targetNodeGeneration,
                        coordinator,
                        staging,
                        ignored -> TestSpot.class,
                        (lane, record) -> {
                            assertTrue(
                                    backend.live.isEmpty(),
                                    "accepted replay must finish before local publication");
                            backend.operations.add("replay:" + record.sequence());
                            return CompletableFuture.completedFuture(null);
                        },
                        new ZLinkSessionRelocationPeerClient(peer),
                        Duration.ofSeconds(1),
                        ignored -> {
                            backend.operations.add("normalize");
                            return CompletableFuture.completedFuture(null);
                        });
        var request =
                new ZLinkSpotRetireControl.StageRequest(
                        new ZLinkSpotRetireControl.Fence(aggregateId, 1),
                        sourceRid,
                        11,
                        "source-owner",
                        12,
                        targetRid,
                        targetNodeGeneration,
                        "target-owner",
                        23,
                        "mesh",
                        spotId,
                        "room",
                        false,
                        true,
                        prepared.request().root(),
                        participants,
                        List.of(
                                new ZLinkSpotRetireControl.SessionRouteFence(
                                        actorId,
                                        4,
                                        7,
                                        "actor-version-1",
                                        sessionOwnerRid,
                                        31,
                                        "session-owner-id",
                                        32,
                                        sessionRid,
                                        33)));

        endpoint.stage(request).toCompletableFuture().join();
        assertTrue(backend.live.isEmpty());
        coordinator.commit(prepared, OPEN).toCompletableFuture().join();
        AtomicInteger relays = new AtomicInteger();
        FakeStagingBackend recoveredBackend = new FakeStagingBackend(true);
        recoveredBackend.drainActorOnComplete = eagerActorDrain;
        var handlerTerminal = new CompletableFuture<List<byte[]>>();
        if (failHandler) recoveredBackend.requestReplay = handlerTerminal;
        var sourceCapabilities = new ZLinkSpotRelocationReplyRoutes();
        var sourceFailures =
                new ArrayList<systems.zlink.framework.errors.ZLinkFrameworkException>();
        var capturedRelays =
                new ArrayList<
                        systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceRelocationWireCodec.ReplyRelay>();
        if (failHandler) {
            byte[] accepted = acceptedSpotRequest(spotId, 4);
            var received =
                    new systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived(
                            systems.zlink.framework.runtime.internal.backend
                                    .ZLinkBackendRequestResult.OK,
                            Optional.of(RoutingId.from("journal-node")),
                            Optional.of(spotId),
                            Optional.of(44L),
                            new byte[0],
                            accepted,
                            List.of(),
                            parts -> {
                                var failure =
                                        systems.zlink.framework.runtime.messaging
                                                .ZLinkFrameworkErrorReply.failure(parts);
                                assertNotNull(
                                        failure,
                                        "failure terminal must use the retained source failure capability");
                                sourceFailures.add(failure);
                                parts.forEach(Message::close);
                            },
                            () -> {});
            sourceCapabilities.register(accepted, received, spotId, 3).releaseForRelocation();
            sourceCapabilities.bindCommitted(
                    List.of(accepted),
                    targetRid,
                    targetNodeGeneration,
                    new ZLinkSpotRelocationReplyRoutes.CommittedFence(authorityKey, 2, 1));
        }
        var recoveredStaging = new ZLinkUserSpotAggregateStagingOwner(recoveredBackend);
        ZLinkInternalMeshNode recoveryPeer =
                recoveryNode(
                        wire,
                        recoveredBackend.operations,
                        relays,
                        new ArrayList<>(),
                        new ArrayList<>(),
                        new ArrayList<>(),
                        failHandler ? sourceCapabilities : null,
                        capturedRelays);
        var recoveredEndpoint =
                new ZLinkUserSpotRetireTargetEndpoint(
                        targetRid,
                        targetNodeGeneration,
                        coordinator,
                        recoveredStaging,
                        ignored -> TestSpot.class,
                        (lane, record) ->
                                CompletableFuture.failedFuture(
                                        new AssertionError(
                                                "restart recovery must use the production"
                                                        + " replayer")),
                        new ZLinkSessionRelocationPeerClient(recoveryPeer),
                        Duration.ofSeconds(1),
                        ignored -> {
                            recoveredBackend.operations.add("normalize");
                            return CompletableFuture.completedFuture(null);
                        },
                        null,
                        ZLinkSpotRetireControl.client(recoveryPeer),
                        authorityStore,
                        null,
                        null,
                        recoveredBackend::restoreBoundSession);
        try {
            recoveredEndpoint.stage(request).toCompletableFuture().join();
            List<byte[]> stagedReplies = new CopyOnWriteArrayList<>();
            AtomicInteger stagedFailures = new AtomicInteger();
            byte[] stagedRecord = acceptedSpotRequest(spotId, 5);
            var stagedHeader =
                    new ZLinkServiceM6BWireCodec.SpotMessage(
                            true,
                            0,
                            43L,
                            301,
                            303,
                            1,
                            "source",
                            new ZLinkServiceM6BWireCodec.SpotRouteFence(
                                    spotId, 3, targetRid, targetNodeGeneration, 5, 23));
            Message wrongSourcePart = Message.from("wrong-source");
            assertFalse(
                    recoveredEndpoint.handleSpot(
                            new ZLinkInternalMeshNode.PeerAuthorityFence(
                                    sourceRid, 11, "wrong-owner", 12),
                            stagedHeader,
                            new byte[0],
                            () -> stagedRecord,
                            stagedRecord.length,
                            List.of(wrongSourcePart),
                            null,
                            null,
                            ignored -> stagedFailures.incrementAndGet()));
            wrongSourcePart.close();
            assertTrue(
                    recoveredEndpoint.handleSpot(
                            new ZLinkInternalMeshNode.PeerAuthorityFence(
                                    sourceRid, 11, "source-owner", 12),
                            stagedHeader,
                            new byte[0],
                            () -> stagedRecord,
                            stagedRecord.length,
                            List.of(Message.from("target-staged-request")),
                            null,
                            reply -> {
                                stagedReplies.add(reply.getFirst().toByteArray());
                                reply.forEach(Message::close);
                            },
                            ignored -> stagedFailures.incrementAndGet()));
            assertTrue(stagedReplies.isEmpty());
            assertEquals(0, stagedFailures.get());
            recoveredEndpoint
                    .stageRelayedRecord(request, acceptedSpotRequest(spotId, 4))
                    .toCompletableFuture()
                    .join();
            var publication = recoveredEndpoint.publish(request).toCompletableFuture();
            try {
                assertFalse(
                        publication.isDone(), "publication must await bound Session restoration");
                assertFalse(recoveredBackend.lifecycleOpen, "restoration must precede admission");
                assertEquals(0, recoveredBackend.admittedTurns, "replay must await restoration");
                assertFalse(recoveredBackend.operations.contains("command44"));
            } finally {
                recoveredBackend.restoreRelease.complete(null);
            }
            publication.join();
            recoveredBackend.executor.drain();
            if (eagerActorDrain)
                assertFalse(
                        recoveredBackend.queuedTerminals.stream()
                                .anyMatch(CompletableFuture::isCompletedExceptionally),
                        "first Actor handler must not run before final setup even when its executor drains immediately");
            if (failHandler) {
                assertFalse(
                        handlerTerminal.isDone(),
                        "publication must finish while adopted handler terminal is pending");
                assertEquals(0, relays.get());
                assertTrue(sourceFailures.isEmpty());
                assertTrue(recoveredBackend.operations.contains("replay-request:4"));
                assertFalse(
                        recoveredBackend.operations.contains("replay-request:5"),
                        "the next Spot record must remain behind the pending handler terminal in its own FIFO");
                assertTrue(
                        recoveredBackend.operations.contains("actor-push"),
                        "the independent Actor FIFO must progress while that Spot terminal is pending");
                handlerTerminal.completeExceptionally(
                        new systems.zlink.framework.errors.ZLinkFrameworkException(
                                systems.zlink.framework.errors.ZLinkFrameworkErrorKind.REJECTED,
                                "adopted request rejected"));
                recoveredBackend.executor.drain();
                assertEquals(1, sourceFailures.size());
                assertEquals(
                        systems.zlink.framework.errors.ZLinkFrameworkErrorKind.REJECTED,
                        sourceFailures.getFirst().kind());
                assertTrue(
                        recoveredBackend.queuedTerminals.stream()
                                .anyMatch(CompletableFuture::isCompletedExceptionally),
                        "the original accepted handler terminal must remain failed after its error relay");
                var delivered = capturedRelays.getFirst();
                var selected = sourceCapabilities.lookupCanonical(delivered, targetRid);
                assertNotNull(
                        selected,
                        "source capability remains addressable for duplicate terminal acknowledgement");
                assertEquals(
                        ZLinkSpotRelocationReplyRoutes.Ack.ALREADY_TERMINAL,
                        sourceCapabilities
                                .deliverCanonical(
                                        selected,
                                        List.of(),
                                        delivered.terminalResult(),
                                        delivered.failureCode())
                                .toCompletableFuture()
                                .join());
                assertEquals(1, sourceFailures.size());
            }

            assertEquals(List.of(actorId, "spot"), recoveredBackend.live);
            assertEquals(
                    List.of(
                            "prepare",
                            "restore",
                            "prepare-actor",
                            "publish-actor",
                            "publish",
                            "command44",
                            "normalize",
                            "timers",
                            "replay-send:1",
                            "replay-send:2",
                            "replay-send:3",
                            "replay-request:4",
                            "replay-request:5"),
                    recoveredBackend.operations.stream()
                            .filter(
                                    operation ->
                                            !operation.equals("actor-push")
                                                    && !operation.equals("complete-actor"))
                            .toList(),
                    "saved, relay and temporary work must remain durable until the"
                            + " target is published and its lifecycle admission is open");
            assertEquals(
                    1, recoveredBackend.operations.stream().filter("actor-push"::equals).count());
            assertEquals(
                    1,
                    recoveredBackend.operations.stream().filter("complete-actor"::equals).count());
            assertTrue(
                    recoveredBackend.operations.indexOf("complete-actor")
                            > recoveredBackend.operations.indexOf("timers"));
            assertTrue(
                    recoveredBackend.operations.indexOf("actor-push")
                            > recoveredBackend.operations.indexOf("complete-actor"));
            assertTrue(
                    recoveredBackend.operations.indexOf("actor-push")
                            > recoveredBackend.operations.indexOf("timers"),
                    "the independent Actor lane must execute only after final publication opens handlers");
            assertEquals(failHandler ? 0 : 1, stagedReplies.size());
            if (!failHandler) assertArrayEquals(new byte[] {9}, stagedReplies.getFirst());
            assertEquals(failHandler ? 1 : 0, stagedFailures.get());
            assertEquals(
                    1,
                    relays.get(),
                    "the preserved request reply must relay once without an ACK journal retry");
            assertEquals(
                    6,
                    recoveredBackend.admittedTurns,
                    "each saved, relay and temporary item owns one shared permit turn");
            assertEquals(
                    failHandler ? 2 : 1,
                    recoveredBackend.maxActiveTurns,
                    failHandler
                            ? "the independent Actor lane may run while the Spot handler terminal remains pending"
                            : "synchronously completed replay turns must not overlap");
            recoveredBackend.rawSession.assertFirstPush();
        } finally {
            recoveredBackend.restoreRelease.complete(null);
            handlerTerminal.completeExceptionally(new IllegalStateException("test cleanup"));
            recoveredBackend.executor.drain();
            var closed = recoveredBackend.actorRuntime.closeAsync().toCompletableFuture();
            try {
                assertTimeoutPreemptively(
                        Duration.ofSeconds(5),
                        () -> {
                            // State-lane completions can submit lifecycle work after a drain
                            // returns.
                            // The fixture owns this executor until the runtime's terminal
                            // completion.
                            while (!closed.isDone() && !Thread.currentThread().isInterrupted()) {
                                recoveredBackend.executor.drain();
                                Thread.yield();
                            }
                            assertTrue(closed.isDone(), "Actor runtime cleanup did not complete");
                            closed.join();
                        });
            } finally {
                recoveredBackend.rawSession.close();
            }
        }
    }

    @Test
    void crossNodeReplyRelayIsAddressedToTheRelocationSourceNode() {
        //  The frozen record's request-source fence names "journal-node"
        //  while the relocation source (the node that owns the landed reply
        //  capability) is "source-node". Command 33 must be addressed to the
        //  relocation source, not to the request-source fence.
        RoutingId sourceRid = RoutingId.from("source-node");
        RoutingId journalRid = RoutingId.from("journal-node");
        RoutingId targetRid = RoutingId.from("target-node");
        long targetNodeGeneration = 17;
        long targetAttemptGeneration = 41;
        String spotId = "room-a";
        String actorId = "actor-a";
        String authorityKey = ZLinkAuthorityKeyCodec.spot(spotId);
        String actorAuthorityKey = ZLinkAuthorityKeyCodec.actor(actorId);
        AuthorityState authority = new AuthorityState();
        ZLinkLocationRepository authorityStore = authority.proxy();
        var coordinator = new ZLinkAggregateRelocationCoordinator(authorityStore);
        UUID aggregateId = UUID.randomUUID();
        var participants =
                List.of(
                        new ZLinkSpotRetireControl.ParticipantFence(
                                actorAuthorityKey, 1, actorId, "player", false, 4, 7),
                        new ZLinkSpotRetireControl.ParticipantFence(
                                authorityKey, 2, spotId, "room", true, 3, 5));
        var finalEnvelope =
                new ZLinkUserSpotAggregateStagingOwner.Request(
                        TestSpot.class,
                        "room",
                        spotId,
                        3,
                        new byte[] {1},
                        true,
                        new byte[0],
                        List.of(
                                new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                                        actorId,
                                        "player",
                                        new byte[0],
                                        false,
                                        new ZLinkBackendActorRef(targetRid, actorId, 4))),
                        Map.of(
                                "spot",
                                List.of(
                                        new ZLinkSerialExecutionQueue.QueuedRecord(
                                                1, acceptedSpotRequest(spotId, 2)))));
        var storeParticipants =
                List.of(
                        new ZLinkAggregateRelocationCoordinator.Participant(
                                actorAuthorityKey,
                                ZLinkPlacementObjectKind.ACTOR,
                                4,
                                7,
                                "actor-version-1",
                                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                sourceAuthority(actorId, sourceRid),
                                new byte[0]),
                        new ZLinkAggregateRelocationCoordinator.Participant(
                                authorityKey,
                                ZLinkPlacementObjectKind.USER_SPOT,
                                3,
                                5,
                                "version-1",
                                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                                sourceAuthority(spotId, sourceRid),
                                new byte[0]));
        var storeRequest =
                new ZLinkAggregateRelocationCoordinator.Request(
                        aggregateId,
                        targetAttemptGeneration,
                        targetAttemptGeneration,
                        storeParticipants,
                        ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                                finalEnvelope, aggregateId, 5, participants),
                        new ZLinkMeshNodeDescriptorKey("mesh", targetRid),
                        targetNodeGeneration,
                        new ZLinkPlacementCapacityBundle(
                                1,
                                1,
                                Optional.of(
                                        new ZLinkSpotTypeCapacityDelta(
                                                ZLinkPlacementObjectKind.USER_SPOT, "room", 1))),
                        new ZLinkLocationOwnerToken("target-owner", 23),
                        "version-1");
        var finalPrepared = coordinator.prepare(storeRequest, OPEN).toCompletableFuture().join();
        coordinator.commit(finalPrepared, OPEN).toCompletableFuture().join();

        AtomicInteger relays = new AtomicInteger();
        List<RoutingId> relayDestinations = new CopyOnWriteArrayList<>();
        List<
                        systems.zlink.framework.runtime.internal.service
                                .ZLinkServiceRelocationWireCodec.RequestSourceFence>
                relayExpectedSources = new CopyOnWriteArrayList<>();
        List<Long> relayAttempts = new CopyOnWriteArrayList<>();
        FakeStagingBackend backend = new FakeStagingBackend();
        var staging = new ZLinkUserSpotAggregateStagingOwner(backend);
        var wire = new ZLinkServiceM6BWireCodec();
        ZLinkInternalMeshNode peer =
                recoveryNode(
                        wire,
                        backend.operations,
                        relays,
                        relayDestinations,
                        relayExpectedSources,
                        relayAttempts);
        var endpoint =
                new ZLinkUserSpotRetireTargetEndpoint(
                        targetRid,
                        targetNodeGeneration,
                        coordinator,
                        staging,
                        ignored -> TestSpot.class,
                        (lane, record) ->
                                CompletableFuture.failedFuture(
                                        new AssertionError(
                                                "canonical recovery must use the production"
                                                        + " replayer")),
                        new ZLinkSessionRelocationPeerClient(peer),
                        Duration.ofSeconds(1),
                        ignored -> CompletableFuture.completedFuture(null),
                        null,
                        ZLinkSpotRetireControl.client(peer),
                        authorityStore,
                        null);
        var request =
                new ZLinkSpotRetireControl.StageRequest(
                        new ZLinkSpotRetireControl.Fence(aggregateId, targetAttemptGeneration),
                        sourceRid,
                        11,
                        "source-owner",
                        12,
                        targetRid,
                        targetNodeGeneration,
                        "target-owner",
                        23,
                        "mesh",
                        spotId,
                        "room",
                        false,
                        true,
                        storeRequest.root(),
                        participants);

        endpoint.stage(request).toCompletableFuture().join();
        endpoint.publish(request).toCompletableFuture().join();

        assertEquals(
                1,
                relays.get(),
                "the preserved request reply must relay once without an ACK journal retry");
        assertEquals(
                List.of(sourceRid),
                relayDestinations,
                "command 33 must be addressed to the relocation source node");
        assertEquals(1, relayExpectedSources.size());
        relayExpectedSources.forEach(
                expected ->
                        assertEquals(
                                journalRid,
                                expected.nodeRid(),
                                "the expected source keeps the request-source lease fence"));
        assertEquals(
                List.of(targetAttemptGeneration),
                relayAttempts,
                "command 33 must echo the PREPARE targetAttemptGeneration");
    }

    private static byte[] acceptedSpotRecord(String spotId, int value) {
        return ZLinkAcceptedJournalTestRecords.spot(
                "source", spotId, 0, "packet-" + value, Map.of(), new byte[] {(byte) value});
    }

    private static byte[] acceptedSpotRequest(String spotId, int value) {
        return ZLinkAcceptedJournalTestRecords.spot(
                "source",
                spotId,
                40 + value,
                "packet-" + value,
                Map.of(),
                new byte[] {(byte) value});
    }

    private static byte[] sourceAuthority(String spotId, RoutingId sourceRid) {
        return new ZLinkServiceAuthorityPayloadCodec()
                .encodeUser(
                        ZLinkServiceAuthorityPayloadCodec.State.READY,
                        "room",
                        spotId,
                        "source-owner",
                        12,
                        "mesh",
                        sourceRid,
                        11);
    }

    private static ZLinkInternalMeshNode sessionRouteNode(
            ZLinkServiceM6BWireCodec wire, List<String> operations) {
        return (ZLinkInternalMeshNode)
                Proxy.newProxyInstance(
                        ZLinkInternalMeshNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalMeshNode.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("sendSessionRelocationRoute")) {
                                var command =
                                        wire.decodeSessionRelocationRoute((byte[]) arguments[1]);
                                operations.add("command44");
                                return CompletableFuture.completedFuture(null);
                            }
                            if (method.getDeclaringClass() == Object.class) {
                                return method.invoke(proxy, arguments);
                            }
                            throw new UnsupportedOperationException(method.getName());
                        });
    }

    private static ZLinkInternalMeshNode recoveryNode(
            ZLinkServiceM6BWireCodec wire, List<String> operations, AtomicInteger relays) {
        return recoveryNode(
                wire,
                operations,
                relays,
                new CopyOnWriteArrayList<>(),
                new CopyOnWriteArrayList<>(),
                new CopyOnWriteArrayList<>());
    }

    private static ZLinkInternalMeshNode recoveryNode(
            ZLinkServiceM6BWireCodec wire,
            List<String> operations,
            AtomicInteger relays,
            List<RoutingId> relayDestinations,
            List<
                            systems.zlink.framework.runtime.internal.service
                                    .ZLinkServiceRelocationWireCodec.RequestSourceFence>
                    relayExpectedSources,
            List<Long> relayAttempts) {
        return recoveryNode(
                wire,
                operations,
                relays,
                relayDestinations,
                relayExpectedSources,
                relayAttempts,
                null,
                new ArrayList<>());
    }

    private static ZLinkInternalMeshNode recoveryNode(
            ZLinkServiceM6BWireCodec wire,
            List<String> operations,
            AtomicInteger relays,
            List<RoutingId> relayDestinations,
            List<
                            systems.zlink.framework.runtime.internal.service
                                    .ZLinkServiceRelocationWireCodec.RequestSourceFence>
                    relayExpectedSources,
            List<Long> relayAttempts,
            ZLinkSpotRelocationReplyRoutes sourceCapabilities,
            List<
                            systems.zlink.framework.runtime.internal.service
                                    .ZLinkServiceRelocationWireCodec.ReplyRelay>
                    capturedRelays) {
        var relocationWire =
                new systems.zlink.framework.runtime.internal.service
                        .ZLinkServiceRelocationWireCodec();
        return (ZLinkInternalMeshNode)
                Proxy.newProxyInstance(
                        ZLinkInternalMeshNode.class.getClassLoader(),
                        new Class<?>[] {ZLinkInternalMeshNode.class},
                        (proxy, method, arguments) -> {
                            if (method.getName().equals("sendSessionRelocationRoute")) {
                                var command =
                                        wire.decodeSessionRelocationRoute((byte[]) arguments[1]);
                                operations.add("command44");
                                return CompletableFuture.completedFuture(null);
                            }
                            if (method.getName().equals("requestRelocationReplyRelay")) {
                                relayDestinations.add((RoutingId) arguments[0]);
                                var expected =
                                        (systems.zlink.framework.runtime.internal.service
                                                        .ZLinkServiceRelocationWireCodec
                                                        .RequestSourceFence)
                                                arguments[1];
                                relayExpectedSources.add(expected);
                                var relay = relocationWire.decodeReplyRelay((byte[]) arguments[2]);
                                relayAttempts.add(relay.targetAttemptGeneration());
                                capturedRelays.add(relay);
                                int attempt = relays.incrementAndGet();
                                int status = attempt == 1 ? 1 : 2;
                                if (sourceCapabilities != null) {
                                    var selected =
                                            sourceCapabilities.lookupCanonical(
                                                    relay, relay.coordinator().nodeRid());
                                    assertNotNull(
                                            selected,
                                            "publication must preserve the real source reply capability");
                                    @SuppressWarnings("unchecked")
                                    var payload = (List<byte[]>) arguments[3];
                                    var ack =
                                            sourceCapabilities
                                                    .deliverCanonical(
                                                            selected,
                                                            payload,
                                                            relay.terminalResult(),
                                                            relay.failureCode())
                                                    .toCompletableFuture()
                                                    .join();
                                    assertTrue(
                                            ack
                                                    != ZLinkSpotRelocationReplyRoutes.Ack
                                                            .NOT_ACKNOWLEDGED);
                                    status =
                                            ack
                                                            == ZLinkSpotRelocationReplyRoutes.Ack
                                                                    .ALREADY_TERMINAL
                                                    ? 2
                                                    : 1;
                                }
                                return CompletableFuture.completedFuture(
                                        relocationWire.encodeReplyRelayAck(
                                                new systems.zlink.framework.runtime.internal.service
                                                        .ZLinkServiceRelocationWireCodec
                                                        .ReplyRelayAck(
                                                        relay.relocation(),
                                                        relay.coordinator(),
                                                        relay.operation(),
                                                        relay.replyRouteId(),
                                                        expected,
                                                        status)));
                            }
                            if (method.getDeclaringClass() == Object.class) {
                                return method.invoke(proxy, arguments);
                            }
                            throw new UnsupportedOperationException(method.getName());
                        });
    }

    private static final class AuthorityState {
        private final Map<String, ZLinkAuthoritySnapshot> rows = new ConcurrentHashMap<>();
        private ZLinkAggregatePrepareRequest prepared;
        private boolean progress;
        private final AtomicInteger reads = new AtomicInteger();
        private String progressStoreVersion;

        ZLinkLocationRepository proxy() {
            return (ZLinkLocationRepository)
                    Proxy.newProxyInstance(
                            ZLinkLocationRepository.class.getClassLoader(),
                            new Class<?>[] {ZLinkLocationRepository.class},
                            (proxy, method, arguments) ->
                                    switch (method.getName()) {
                                        case "prepareAggregate" ->
                                                prepare(
                                                        (ZLinkAggregatePrepareRequest)
                                                                arguments[0]);
                                        case "commitAggregate" ->
                                                commit((ZLinkAggregateFence) arguments[0]);
                                        case "abortAggregate" ->
                                                CompletableFuture.completedFuture(
                                                        ZLinkAggregateAbortResult.ABORTED);
                                        case "read" -> {
                                            reads.incrementAndGet();
                                            yield CompletableFuture.completedFuture(
                                                    rows.getOrDefault((String) arguments[0], null)
                                                                    == null
                                                            ? new ZLinkAuthorityMissing(
                                                                    Instant.now())
                                                            : rows.get((String) arguments[0]));
                                        }
                                        case "compareExchange" ->
                                                compareExchange(
                                                        (String) arguments[0],
                                                        (ZLinkAuthorityExpectation) arguments[1],
                                                        (ZLinkAuthorityMutation) arguments[2]);
                                        case "readAggregateProgress" ->
                                                readAggregateProgress(
                                                        (ZLinkAggregateFence) arguments[0]);
                                        case "removeAggregateProgress" ->
                                                removeAggregateProgress(
                                                        (ZLinkAggregateFence) arguments[0],
                                                        (String) arguments[1]);
                                        default ->
                                                throw new UnsupportedOperationException(
                                                        method.getName());
                                    });
        }

        private CompletionStage<ZLinkAggregatePrepareResult> prepare(
                ZLinkAggregatePrepareRequest request) {
            prepared = request;
            return CompletableFuture.completedFuture(
                    new ZLinkAggregatePrepared(
                            new ZLinkAggregateFence(
                                    request.aggregateId(), request.aggregateGeneration())));
        }

        private CompletionStage<ZLinkAggregateCommitResult> commit(ZLinkAggregateFence fence) {
            if (prepared == null
                    || !prepared.aggregateId().equals(fence.aggregateId())
                    || prepared.aggregateGeneration() != fence.aggregateGeneration()) {
                return CompletableFuture.completedFuture(ZLinkAggregateCommitResult.STALE);
            }
            for (var participant : prepared.participants()) {
                boolean actor = participant.authorityKey().contains(":a:");
                rows.put(
                        participant.authorityKey(),
                        new ZLinkAuthoritySnapshot(
                                "version-2",
                                participant.authorityPayload(),
                                actor ? 4 : 3,
                                actor ? 8 : 6,
                                prepared.targetOwner().ownerId(),
                                prepared.targetOwner().leaseGeneration(),
                                new ZLinkPlacementAllocation(
                                        ZLinkPlacementAllocationState.ACTIVE,
                                        actor
                                                ? ZLinkPlacementObjectKind.ACTOR
                                                : ZLinkPlacementObjectKind.USER_SPOT,
                                        actor ? "player" : "room",
                                        prepared.targetDescriptor(),
                                        prepared.targetDescriptorLifecycleGeneration(),
                                        prepared.capacityBundle()),
                                Instant.now()));
            }
            progress = true;
            progressStoreVersion = "aggregate-commit";
            return CompletableFuture.completedFuture(ZLinkAggregateCommitResult.COMMITTED);
        }

        private CompletionStage<Optional<ZLinkAggregateProgressSnapshot>> readAggregateProgress(
                ZLinkAggregateFence fence) {
            return CompletableFuture.completedFuture(
                    !progress ? Optional.empty() : Optional.of(progressSnapshot(fence)));
        }

        private CompletionStage<Boolean> removeAggregateProgress(
                ZLinkAggregateFence fence, String expectedStoreVersion) {
            if (!progress || !progressStoreVersion.equals(expectedStoreVersion)) {
                return CompletableFuture.completedFuture(false);
            }
            progress = false;
            progressStoreVersion = null;
            return CompletableFuture.completedFuture(true);
        }

        private ZLinkAggregateProgressSnapshot progressSnapshot(ZLinkAggregateFence fence) {
            return new ZLinkAggregateProgressSnapshot(fence, progressStoreVersion, prepared);
        }

        private CompletionStage<ZLinkAuthorityWriteResult> compareExchange(
                String key,
                ZLinkAuthorityExpectation expectation,
                ZLinkAuthorityMutation mutation) {
            ZLinkAuthoritySnapshot current = rows.get(key);
            if (!(expectation instanceof ZLinkAuthorityExpectFound found)
                    || current == null
                    || !current.storeVersion().equals(found.storeVersion())
                    || !(mutation instanceof ZLinkAuthorityPut put)) {
                return CompletableFuture.completedFuture(
                        new ZLinkAuthorityConflict(
                                current == null
                                        ? new ZLinkAuthorityMissing(Instant.now())
                                        : current));
            }
            ZLinkAuthoritySnapshot stored =
                    new ZLinkAuthoritySnapshot(
                            "cas-" + key,
                            put.payload(),
                            current.objectGeneration(),
                            current.authorityOwnerGeneration(),
                            current.ownerId(),
                            current.ownerLeaseGeneration(),
                            current.allocation(),
                            Instant.now());
            rows.put(key, stored);
            return CompletableFuture.completedFuture(
                    new ZLinkAuthorityStored(
                            stored.storeVersion(),
                            stored.payload(),
                            stored.objectGeneration(),
                            stored.authorityOwnerGeneration(),
                            stored.ownerId(),
                            stored.ownerLeaseGeneration(),
                            stored.allocation(),
                            stored.storeNow()));
        }
    }

    private static final class FakeStagingBackend
            implements ZLinkUserSpotAggregateStagingOwner.StagingBackend {
        private final ArrayList<String> operations = new ArrayList<>();
        private final ArrayList<String> live = new ArrayList<>();
        private int replaySequence;
        private CompletionStage<List<byte[]>> requestReplay;
        private boolean drainActorOnComplete;
        private final List<CompletableFuture<Void>> queuedTerminals = new ArrayList<>();
        private boolean lifecycleOpen;
        private int admittedTurns;
        private final CompletableFuture<Void> restoreRelease = new CompletableFuture<>();
        private final ZLinkActorRuntime actorRuntime;
        private final ZLinkInternalSpotNode sessionNode;
        private ZLinkRelocationRawSessionFixture rawSession;

        private FakeStagingBackend() {
            this(false);
        }

        private FakeStagingBackend(boolean boundSessionReplay) {
            if (!boundSessionReplay) {
                actorRuntime = null;
                sessionNode = null;
                return;
            }
            rawSession = new ZLinkRelocationRawSessionFixture();
            sessionNode = rawSession.node().spotNode();
            actorRuntime =
                    new ZLinkActorRuntime(
                            sessionNode,
                            Map.of("player", BoundActorFactory.class),
                            Map.of(),
                            Duration.ofSeconds(1),
                            Duration.ofSeconds(5),
                            new ZLinkLocationOptions(),
                            new ZLinkJsonMessageSerializer(),
                            ZLinkHandlerActivator.reflection(),
                            systems.zlink.framework.streams.ZLinkStreamCodec.JSON,
                            null,
                            executor);
            sessionNode.setRemoteSessionBindingOwner(actorRuntime::applyRemoteSessionBinding);
        }

        private CompletionStage<Void> restoreBoundSession(
                ZLinkSpotRetireControl.StageRequest request, Map<String, Long> generations) {
            assertEquals(List.of("actor-a", "spot"), live);
            assertFalse(lifecycleOpen, "binding must be restored before admission opens");
            return restoreRelease.thenCompose(
                    ignored ->
                            ZLinkUserSpotRetireRuntime.installBoundSessions(
                                    sessionNode, request, generations));
        }

        @Override
        public CompletionStage<Optional<byte[]>> replayActor(
                Object spot, Object prepared, ZLinkActorAcceptedJournal.Record record) {
            assertTrue(lifecycleOpen);
            var actor = ((ZLinkActorRuntime.PreparedTransferredActor) prepared).actor();
            operations.add("actor-push");
            return actor.context()
                    .boundSession()
                    .send(new BoundPush("first-replay"))
                    .submit()
                    .thenApply(ignored -> Optional.empty());
        }

        private int activeTurns;
        private int maxActiveTurns;

        @Override
        public CompletionStage<Object> prepareSpot(
                ZLinkUserSpotAggregateStagingOwner.Request request) {
            operations.add("prepare");
            return CompletableFuture.completedFuture("spot");
        }

        @Override
        public CompletionStage<Void> restoreSpot(
                Object prepared,
                ZLinkUserSpotAggregateStagingOwner.Request request,
                ZLinkRelocationCancellation cancellation) {
            operations.add("restore");
            return CompletableFuture.completedFuture(null);
        }

        private final CompletableFuture<Void> moving = new CompletableFuture<>();
        private final ZLinkManualExecutor executor = new ZLinkManualExecutor();
        private final ZLinkSerialExecutionQueue fifo =
                new ZLinkSerialExecutionQueue(
                        executor,
                        systems.zlink.framework.execution.ZLinkExecutionLanePolicy.generic());

        {
            fifo.enqueueLifecycleBarrier(() -> moving);
        }

        @Override
        public CompletionStage<QueuedTurn> prepareIngress(Object actor) {
            if (actorRuntime != null)
                return actorRuntime.prepareTransferredActorDispatch(
                        (ZLinkActorRuntime.PreparedTransferredActor) actor);
            return CompletableFuture.completedFuture(
                    executor.actorDispatch((String) actor, actor, moving));
        }

        @Override
        public CompletionStage<Void> enqueueIngress(
                Object preparedSpot,
                List<QueuedTurn> dispatches,
                String laneId,
                byte[] record,
                Supplier<CompletionStage<Void>> turn,
                CompletableFuture<Void> admission) {
            admittedTurns++;
            if (laneId.startsWith("actor:")) {
                var dispatch =
                        dispatches.stream()
                                .filter(
                                        turnHandle ->
                                                turnHandle
                                                        .actorId()
                                                        .equals(
                                                                laneId.substring(
                                                                        "actor:".length())))
                                .findFirst()
                                .orElseThrow();
                if (actorRuntime != null) {
                    var terminal =
                            actorRuntime.submitActorDispatchLazyRecord(
                                    dispatch,
                                    () -> record,
                                    record.length,
                                    () -> {
                                        activeTurns++;
                                        maxActiveTurns = Math.max(maxActiveTurns, activeTurns);
                                        return turn.get()
                                                .whenComplete((ignored, failure) -> activeTurns--);
                                    },
                                    () -> {},
                                    admission);
                    queuedTerminals.add(terminal.toCompletableFuture());
                    return terminal;
                }
                return dispatch.target()
                        .executeActorLazyRecord(
                                dispatch.activation(),
                                () -> record,
                                record.length,
                                turn,
                                () -> {},
                                admission);
            }
            var terminal =
                    fifo.enqueueRelocatableLazyRecord(
                            () -> record,
                            record.length,
                            () -> {
                                activeTurns++;
                                maxActiveTurns = Math.max(maxActiveTurns, activeTurns);
                                return turn.get().whenComplete((ignored, failure) -> activeTurns--);
                            },
                            () -> {},
                            admission);
            queuedTerminals.add(terminal.toCompletableFuture());
            return terminal;
        }

        @Override
        public CompletionStage<Void> resumeIngress(Object preparedSpot, Object hold) {
            moving.complete(null);
            executor.drain();
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Object> prepareActor(
                ZLinkUserSpotAggregateStagingOwner.ActorParticipant participant,
                ZLinkRelocationCancellation cancellation) {
            operations.add("prepare-actor");
            if (actorRuntime == null)
                return CompletableFuture.completedFuture(participant.actorId());
            return actorRuntime
                    .prepareRelocatedActor(
                            participant.actorId(),
                            participant.actorType(),
                            participant.state(),
                            participant.restoreSnapshot(),
                            null,
                            cancellation,
                            participant.preparedActorRef())
                    .thenApply(actor -> actor);
        }

        @Override
        public CompletionStage<Void> publishSpot(Object prepared) {
            operations.add("publish");
            live.add((String) prepared);

            return CompletableFuture.completedFuture(null);
        }

        @Override
        public void publishActor(
                Object preparedSpot,
                Object prepared,
                String targetSpotId,
                Long targetOwnerGeneration) {
            operations.add("publish-actor");
            if (actorRuntime == null) live.add((String) prepared);
            else {
                var actor = (ZLinkActorRuntime.PreparedTransferredActor) prepared;
                actorRuntime.publishPreparedTransferredActor(actor);
                live.add(actor.actorId());
            }
        }

        @Override
        public void completeActor(Object prepared) {
            operations.add("complete-actor");
            if (actorRuntime != null)
                actorRuntime.completePreparedTransferredActor(
                        (ZLinkActorRuntime.PreparedTransferredActor) prepared);
            if (drainActorOnComplete) executor.drain();
        }

        @Override
        public void publishTimers(Object prepared) {
            operations.add("timers");
            lifecycleOpen = true;
        }

        @Override
        public CompletionStage<List<byte[]>> replaySpot(
                Object prepared, ZLinkSpotAcceptedJournal.Record record) {
            assertTrue(lifecycleOpen, "durable backlog handler ran before target lifecycle open");
            assertEquals(
                    List.of("actor-a", "spot"),
                    live,
                    "durable backlog handler ran before target publication");
            int sequence = ++replaySequence;
            if (record.requestSequence().isPresent()) {
                operations.add("replay-request:" + sequence);
                return requestReplay == null
                        ? CompletableFuture.completedFuture(List.of(new byte[] {9}))
                        : requestReplay;
            }
            operations.add("replay-send:" + sequence);
            return CompletableFuture.completedFuture(List.of());
        }

        @Override
        public CompletionStage<Void> discardActor(Object prepared) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> discardSpot(Object prepared) {
            return CompletableFuture.completedFuture(null);
        }
    }

    public static final class BoundActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new BoundActor(context));
        }
    }

    private record BoundActor(ZLinkActorContext context) implements ZLinkActor {}

    private record BoundPush(String value) {}

    private static class RestartActorBackend
            implements ZLinkStandaloneActorRelocationStagingOwner.Backend {
        @Override
        public CompletionStage<List<CompletableFuture<Void>>> admitBacklogPrefix(
                Object actor,
                ZLinkStandaloneActorRelocationStagingOwner.Request request,
                List<ZLinkSerialExecutionQueue.QueuedRecord> records,
                java.util.function.BiFunction<
                                ZLinkSerialExecutionQueue.QueuedRecord,
                                CompletableFuture<Void>,
                                java.util.function.Supplier<CompletionStage<Void>>>
                        replay) {
            if (records.isEmpty()) return CompletableFuture.completedFuture(List.of());
            var jobs =
                    new systems.zlink.framework.runtime.internal.dispatch.ZLinkApplicationJobQueue(
                            systems.zlink.framework.configuration.ZLinkApplicationJobQueueProfile
                                    .BALANCED,
                            java.util.OptionalLong.of(1),
                            new systems.zlink.framework.runtime.internal.dispatch
                                    .ZLinkApplicationJobQueue.ProcessorCandidates(
                                    1, null, null, null));
            return actorRuntime
                    .prepareTransferredActorDispatch(
                            (ZLinkActorRuntime.PreparedTransferredActor) actor)
                    .thenCompose(
                            dispatch ->
                                    dispatch.activation()
                                            .relocationLane()
                                            .admitDurablePrefix(
                                                    records,
                                                    (record, terminal) -> {
                                                        var operation =
                                                                replay.apply(record, terminal);
                                                        return () ->
                                                                actorRuntime.runActorDispatchTurn(
                                                                        request.actorId(),
                                                                        operation);
                                                    },
                                                    jobs))
                    .whenComplete(
                            (accepted, failure) -> {
                                if (failure != null) jobs.close();
                                else
                                    CompletableFuture.allOf(
                                                    accepted.toArray(CompletableFuture[]::new))
                                            .whenComplete(
                                                    (ignored, terminalFailure) -> jobs.close());
                            });
        }

        private final List<String> operations;
        private final ZLinkManualExecutor tasks = new ZLinkManualExecutor();
        private ZLinkActorRuntime.PreparedTransferredActor prepared;
        private final ZLinkRelocationRawSessionFixture rawSession =
                new ZLinkRelocationRawSessionFixture();
        private final ZLinkActorRuntime actorRuntime =
                new ZLinkActorRuntime(
                        rawSession.node().spotNode(),
                        Map.of("player", BoundActorFactory.class),
                        Map.of(),
                        Duration.ofSeconds(1),
                        Duration.ofSeconds(5),
                        new ZLinkLocationOptions(),
                        new ZLinkJsonMessageSerializer(),
                        ZLinkHandlerActivator.reflection(),
                        systems.zlink.framework.streams.ZLinkStreamCodec.JSON,
                        null,
                        tasks);

        private void drain() {
            tasks.drain();
        }

        private RestartActorBackend(List<String> operations) {
            this.operations = operations;
        }

        @Override
        public CompletionStage<Object> prepare(
                ZLinkStandaloneActorRelocationStagingOwner.Request request,
                byte[] state,
                ZLinkRelocationCancellation cancellation) {
            operations.add("prepare-actor");
            return actorRuntime
                    .prepareRelocatedActor(
                            request.actorId(),
                            request.stableType(),
                            new byte[0],
                            false,
                            null,
                            cancellation,
                            new ZLinkBackendActorRef(
                                    rawSession.node().spotNode().routingId(),
                                    request.actorId(),
                                    request.objectGeneration()))
                    .thenApply(
                            actor -> {
                                prepared = actor;
                                return actor;
                            });
        }

        @Override
        public CompletionStage<Optional<byte[]>> replay(
                Object actor,
                ZLinkStandaloneActorRelocationStagingOwner.Request request,
                ZLinkActorAcceptedJournal.Record record) {
            return CompletableFuture.failedFuture(
                    new AssertionError("the direct-Join restart root has no accepted journal"));
        }

        @Override
        public void publish(
                Object actor,
                ZLinkStandaloneActorRelocationStagingOwner.Request request,
                long targetOwnerGeneration) {
            operations.add("publish-actor");
            actorRuntime.publishPreparedTransferredActor(
                    (ZLinkActorRuntime.PreparedTransferredActor) actor);
        }

        @Override
        public void openAdmission(Object actor) {
            operations.add("open");
            actorRuntime.completePreparedTransferredActor(
                    (ZLinkActorRuntime.PreparedTransferredActor) actor);
        }

        @Override
        public CompletionStage<Void> discard(
                Object actor, ZLinkStandaloneActorRelocationStagingOwner.Request request) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class TestSpot implements ZLinkSpot<ZLinkActor> {
        @Override
        public ZLinkSpotContext context() {
            return null;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }
}
