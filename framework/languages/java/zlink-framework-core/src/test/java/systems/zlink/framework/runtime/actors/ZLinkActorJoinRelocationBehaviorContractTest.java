package systems.zlink.framework.runtime.actors;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Proxy;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Queue;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.CopyOnWriteArrayList;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.TimeUnit;
import java.util.concurrent.atomic.AtomicBoolean;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkActorContext;
import systems.zlink.framework.actors.ZLinkActorFactory;
import systems.zlink.framework.actors.ZLinkActorJoinCompletion;
import systems.zlink.framework.locations.ZLinkActivationConcurrency;
import systems.zlink.framework.locations.ZLinkCapacityUsage;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkMeshNodeObjectRole;
import systems.zlink.framework.locations.ZLinkObjectCapability;
import systems.zlink.framework.locations.ZLinkObjectMaintenancePolicyKind;
import systems.zlink.framework.locations.ZLinkPlacementCapacity;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.locations.ZLinkSpotTypeCapacity;
import systems.zlink.framework.runtime.InMemoryRelocationStore;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamErrorHandler;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorBindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorUnbindOperation;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.calls.ZLinkOneWayCalls;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthorityReadResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot;
import systems.zlink.framework.runtime.internal.locations.ZLinkCanonicalRelocationObservation;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteIntent;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptorKey;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectCommitResult;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservation;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReservationRequest;
import systems.zlink.framework.runtime.internal.locations.ZLinkObjectReserved;
import systems.zlink.framework.runtime.internal.locations.ZLinkOwnerLeaseClaimed;
import systems.zlink.framework.runtime.internal.locations.ZLinkPlacementCapacityBundle;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations.ZLinkInMemoryLocationStore;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.messaging.ZLinkJsonMessageSerializer;
import systems.zlink.framework.runtime.spots.ZLinkActorRelocationRuntimePortFixture;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamRuntime;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkStoreSpotHandleResolver;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkSession;
import systems.zlink.framework.streams.ZLinkSessionContext;
import systems.zlink.framework.streams.ZLinkSessionDispatchContext;
import systems.zlink.framework.streams.ZLinkStreamError;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;

final class ZLinkActorJoinRelocationBehaviorContractTest {
    private static final String MESH = "relocation-behavior";
    private static final String ACTOR_TYPE = "probe";
    private static final String ROOM_TYPE = "room";
    private static final String ACTOR_ID = "actor-a";
    private static final String TARGET_SPOT_ID = "room-a";
    private static final RoutingId SOURCE_RID = RoutingId.from("source-node");
    private static final RoutingId TARGET_RID = RoutingId.from("target-node");
    private static final RoutingId SESSION_RID = RoutingId.from("session-a");
    private static final Duration REQUEST_TIMEOUT = Duration.ofSeconds(2);
    private static final Duration ORDINARY_SEND_TIMEOUT = Duration.ofMillis(25);
    private final List<ZLinkStreamRuntime> routeRuntimes = new ArrayList<>();

    private enum Verification {
        UNBOUND_AUTHORITY,
        BOUND_OUTBOUND_FIFO,
        SUCCESSOR_TERMINAL_GATE,
        LOCAL_TARGET_STORE_ZERO,
        LOCAL_TARGET_CONFLICT,
        REMOTE_PROOF_MISMATCH_RETRY
    }

    @AfterEach
    void closeRouteRuntimes() {
        routeRuntimes.forEach(runtime ->
            runtime.closeAsync().toCompletableFuture().join());
        routeRuntimes.clear();
        RouteRuntimeSession.current.set(null);
    }

    @Test
    void unboundJoinProjectsTheCommittedTargetAuthority() throws Exception {
        runPublicDeferredJoin(Verification.UNBOUND_AUTHORITY);
    }

    @Test
    void publicDeferredJoinProducesObservableRelocationTraceAndRetainsCallbackPush()
        throws Exception {
        runPublicDeferredJoin(Verification.BOUND_OUTBOUND_FIFO);
    }

    @Test
    void successorWaitsForTheExactPredecessorRouteTerminal() throws Exception {
        runPublicDeferredJoin(Verification.SUCCESSOR_TERMINAL_GATE);
    }

    @Test
    void publicCommand44UsesLocalCommittedTenureWithoutStoreRead()
        throws Exception {
        runPublicDeferredJoin(Verification.LOCAL_TARGET_STORE_ZERO);
    }

    @Test
    void publicCommand44RejectsConflictingLocalFingerprintBeforeMutation()
        throws Exception {
        runPublicDeferredJoin(Verification.LOCAL_TARGET_CONFLICT);
    }

    @Test
    void publicCommand44RetriesAfterMismatchedStoreProofWithoutLosingOutbound()
        throws Exception {
        runPublicDeferredJoin(Verification.REMOTE_PROOF_MISMATCH_RETRY);
    }

    private void runPublicDeferredJoin(Verification verification)
        throws Exception {
        boolean boundSession = verification != Verification.UNBOUND_AUTHORITY;
        JsonNode behaviorFixture = fixture(
            "framework/runtime/conformance/relocation-behavior-v1.json");
        JsonNode boundSessionFixture = fixture(
            "framework/runtime/conformance/bound-session-relocation-v1.json");
        Trace trace = new Trace();
        TargetActorFactory.trace.set(trace);
        var locations = new ZLinkInMemoryLocationStore();
        var relocations = new InMemoryRelocationStore();
        var sourceOwner = owner(locations, "source-owner");
        var targetOwner = owner(locations, "target-owner");
        ZLinkMeshNodeDescriptor sourceDescriptor = descriptor(
            SOURCE_RID, 11, sourceOwner, false);
        ZLinkMeshNodeDescriptor targetDescriptor = descriptor(
            TARGET_RID, 19, targetOwner, true);
        locations.updateMeshNode(
                sourceDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().get();
        locations.updateMeshNode(
                targetDescriptor, ZLinkLocationWriteIntent.NEW_CLAIM)
            .toCompletableFuture().get();
        createAuthority(
            locations,
            sourceDescriptor,
            sourceOwner,
            ZLinkPlacementObjectKind.ACTOR,
            ZLinkAuthorityKeyCodec.actor(ACTOR_ID),
            ACTOR_TYPE,
            new ZLinkActorAuthorityPayloadCodec().encode(
                ZLinkActorAuthorityPayloadCodec.State.READY,
                ACTOR_TYPE,
                ACTOR_ID,
                SOURCE_RID.toString(),
                1,
                1,
                sourceOwner.ownerId(),
                sourceOwner.leaseGeneration(),
                MESH,
                SOURCE_RID,
                sourceDescriptor.lifecycleGeneration()),
            ZLinkPlacementCapacityBundle.actor(1));
        createAuthority(
            locations,
            targetDescriptor,
            targetOwner,
            ZLinkPlacementObjectKind.USER_SPOT,
            ZLinkAuthorityKeyCodec.spot(TARGET_SPOT_ID),
            ROOM_TYPE,
            new ZLinkServiceAuthorityPayloadCodec().encodeUser(
                ZLinkServiceAuthorityPayloadCodec.State.READY,
                ROOM_TYPE,
                TARGET_SPOT_ID,
                targetOwner.ownerId(),
                targetOwner.leaseGeneration(),
                MESH,
                TARGET_RID,
                targetDescriptor.lifecycleGeneration()),
            ZLinkPlacementCapacityBundle.spot(
                ZLinkPlacementObjectKind.USER_SPOT,
                ROOM_TYPE,
                1));

        AtomicReference<TargetActor> targetActor = new AtomicReference<>();
        AtomicReference<ZLinkActorRuntime> targetRuntime = new AtomicReference<>();
        AtomicBoolean countCommittedProofReads = new AtomicBoolean();
        AtomicInteger committedProofReads = new AtomicInteger();
        AtomicReference<CompletableFuture<Void>> committedProofReadGate =
            new AtomicReference<>();
        AtomicBoolean returnMismatchedCommittedProof = new AtomicBoolean(
            verification == Verification.REMOTE_PROOF_MISMATCH_RETRY);
        ZLinkLocationRepository authority = tracingAuthority(
            locations,
            relocations,
            trace,
            targetActor,
            targetRuntime,
            countCommittedProofReads,
            committedProofReads,
            committedProofReadGate,
            returnMismatchedCommittedProof);
        var routes = new ZLinkStoreLocationResolvers(
            ZLinkRegisteredLocationStores.fromUnified(authority),
            new ZLinkLocationOptions());
        var spotResolver = new ZLinkStoreSpotHandleResolver(
            new ZLinkStoreLocationResolvers.AddressResolvers(
                List.of(MESH), routes),
            authority);

        AtomicBoolean routeAvailable = new AtomicBoolean();
        AtomicInteger callbackDeliveries = new AtomicInteger();
        List<String> callbackDeliveryOrder = new CopyOnWriteArrayList<>();
        AtomicInteger callbackCleanups = new AtomicInteger();
        ImmediateAdmission admission = new ImmediateAdmission(
            callbackCleanups, trace);
        RoutePeer routePeer = new RoutePeer(trace);
        AtomicReference<ZLinkBackendActorRef> targetRef = new AtomicReference<>();
        AtomicReference<ActorTenureProjection> targetTenureProjection =
            new AtomicReference<>();
        ZLinkInternalSpotNode sourceNode = spotNode(
            SOURCE_RID,
            sourceOwner.leaseGeneration(),
            routeAvailable,
            callbackDeliveries,
            callbackDeliveryOrder,
            trace,
            null,
            null,
            routePeer);
        ZLinkInternalSpotNode targetNode = spotNode(
            TARGET_RID,
            targetOwner.leaseGeneration(),
            routeAvailable,
            callbackDeliveries,
            callbackDeliveryOrder,
            trace,
            targetRef,
            targetTenureProjection,
            routePeer);

        var source = new ZLinkActorRuntime(
            sourceNode,
            Map.of(ACTOR_TYPE, SourceActorFactory.class),
            Map.of(),
            ORDINARY_SEND_TIMEOUT,
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer(),
            ZLinkHandlerActivator.reflection(),
            ZLinkStreamCodec.JSON,
            admission);
        var target = new ZLinkActorRuntime(
            targetNode,
            Map.of(ACTOR_TYPE, TargetActorFactory.class),
            Map.of(),
            ORDINARY_SEND_TIMEOUT,
            Duration.ofSeconds(1),
            new ZLinkJsonMessageSerializer(),
            ZLinkHandlerActivator.reflection(),
            ZLinkStreamCodec.JSON,
            admission);
        targetRuntime.set(target);
        source.setMeshName(MESH);
        target.setMeshName(MESH);
        source.setDeferredJoinAcceptedRecovery(authority, relocations);
        target.setDeferredJoinAcceptedRecovery(authority, relocations);
        source.setStoreLocationResolvers(routes);
        target.setStoreLocationResolvers(routes);
        source.setRemoteAddressResolver(spotResolver);

        source.setSessionRelocationSealer(
            new ZLinkSessionRelocationPeerClient(routePeer.node()));
        ZLinkSessionRelocationPeerClient targetPeer =
            new ZLinkSessionRelocationPeerClient(routePeer.node());
        target.setSessionRelocationSealer(targetPeer);

        TargetSpot targetSpot = new TargetSpot(
            trace,
            verification == Verification.BOUND_OUTBOUND_FIFO
                || verification == Verification.LOCAL_TARGET_STORE_ZERO
                || verification == Verification.LOCAL_TARGET_CONFLICT
                || verification == Verification.REMOTE_PROOF_MISMATCH_RETRY);
        var port = new ZLinkActorRelocationRuntimePortFixture(
            target,
            targetNode,
            TARGET_SPOT_ID,
            targetSpot,
            actor -> {
                TargetActor current = (TargetActor) actor;
                targetActor.set(current);
                targetRef.set(target.actorRef(current));
                assertEquals(0, current.handlerCount());
                return targetSpot.onJoinedActor(current);
            },
            actorRef -> CompletableFuture.completedFuture(List.of()),
            targetPeer);
        source.setActorJoinTransferTransport((address, parts, timeout, internal) -> {
            ZLinkActorSpotRoutePackets.TransferRequest request =
                ZLinkActorSpotRoutePackets.decodeTransferRequest(parts.get(1));
            if (request.commit()) {
                assertEquals(
                    boundSession,
                    request.sessionRouteCommand44().length > 0,
                    "only a bound Session relocation carries route intent");
            }
            return port.request(parts).thenCompose(replies -> {
                if (!request.commit()) {
                    return CompletableFuture.completedFuture(replies);
                }
                return CompletableFuture.completedFuture(replies);
            });
        });

        SourceActor sourceActor = (SourceActor) source
            .getOrCreateLocalActor(ACTOR_ID, ZLinkActor.class)
            .toCompletableFuture().get().orElseThrow();
        ZLinkBackendActorRef sourceRef = source.actorRef(sourceActor);
        source.markJoinedEntrySpot(sourceActor, sourceRef, SOURCE_RID);
        if (boundSession) {
            boolean localTargetOwner =
                verification == Verification.LOCAL_TARGET_STORE_ZERO
                    || verification == Verification.LOCAL_TARGET_CONFLICT;
            if (localTargetOwner) {
                var sourceBinding = new ZLinkSessionActorsRuntime(
                    targetNode,
                    sessionStream(
                        targetNode,
                        routeAvailable,
                        callbackDeliveries,
                        callbackDeliveryOrder,
                        trace),
                    SESSION_RID,
                    source,
                    new ZLinkJsonMessageSerializer(),
                    ignored -> true,
                    null,
                    true,
                    ZLinkStreamCodec.JSON);
                sourceBinding.bind(
                        new systems.zlink.framework.actors.ActorRef(
                            ACTOR_ID,
                            sourceRef.generation(),
                            MESH,
                            SOURCE_RID))
                    .toCompletableFuture().get();
            }
            RouteRuntimeSession routeSession = startRouteRuntime(
                localTargetOwner ? target : source,
                localTargetOwner ? targetNode : sourceNode,
                routeAvailable,
                callbackDeliveries,
                callbackDeliveryOrder,
                trace);
            routeSession.context().actors().bind(
                    new systems.zlink.framework.actors.ActorRef(
                        ACTOR_ID,
                        sourceRef.generation(),
                        MESH,
                        SOURCE_RID))
                .toCompletableFuture().get();
            routePeer.attach(routeSession);
        }
        assertEquals(
            boundSession,
            source.boundSessionRoute(sourceActor).isPresent());
        CompletableFuture<Void> leaveNotification = new CompletableFuture<>();
        source.setSourceActorLeaver(ignored -> {
            trace.add("sourceMembershipLeaveSubmitted");
            return leaveNotification;
        });

        source.submitActorDispatch(ACTOR_ID, () -> {
            trace.add("relocationRequested");
            sourceActor.context()
                .joinSpot(TARGET_SPOT_ID)
                .timeout(REQUEST_TIMEOUT)
                .defer();
            return CompletableFuture.completedFuture(null);
        }).toCompletableFuture().get();

        if (boundSession) {
            try {
                routePeer.routeRequested.get(2, TimeUnit.SECONDS);
            } catch (java.util.concurrent.TimeoutException timeout) {
                fail("Session route did not start after source cleanup: "
                    + trace.events, timeout);
            }
        }
        trace.await("authorityCompleted", Duration.ofSeconds(2));
        assertNotEquals(
            trace.storeVersion("sourceCleanupCompleted"),
            trace.storeVersion("authorityCompleted"),
            "source cleanup and Completed require distinct authority writes");
        ZLinkAuthoritySnapshot committed = assertInstanceOf(
            ZLinkAuthoritySnapshot.class,
            locations.read(
                    ZLinkAuthorityKeyCodec.actor(ACTOR_ID), () -> false)
                .toCompletableFuture().get());
        if (!boundSession) {
            ActorTenureProjection projection = targetTenureProjection.get();
            assertNotNull(
                projection,
                "unbound target must apply the immutable committed Actor tenure");
            assertEquals(
                new ZLinkBackendActorRef(
                    TARGET_RID, ACTOR_ID, committed.objectGeneration()),
                projection.actor());
            assertEquals(
                committed.authorityOwnerGeneration(),
                projection.authorityOwnerGeneration());
            assertEquals(
                committed.ownerLeaseGeneration(),
                projection.ownerLeaseGeneration());
            assertFalse(leaveNotification.isDone(),
                "one-way source leave completion must not gate target progress");
            assertOrder(trace,
                "relocationRequested",
                "targetStateRestored",
                "ownershipCommitted",
                "targetLifecycleCompleted",
                "sourceMembershipLeaveSubmitted",
                "publicJoinCompleted",
                "targetDispatchActivated",
                "targetReadyPublished",
                "sourceCleanupCompleted",
                "authorityCompleted");
            assertEquals(-1, trace.indexOf("readyWhileMoving"));
            assertFixtureEdge(
                actorJoinProfile(behaviorFixture).path("additionalOrder"),
                trace,
                "targetLifecycleCompleted",
                "sourceMembershipLeaveSubmitted");
            assertFixtureEdge(
                actorJoinProfile(behaviorFixture).path("additionalOrder"),
                trace,
                "sourceMembershipLeaveSubmitted",
                "publicJoinCompleted");
            return;
        }
        dispatch(target, targetActor.get(), "targetApplicationMessageDelivered")
            .toCompletableFuture().get();
        Thread.sleep(ORDINARY_SEND_TIMEOUT.multipliedBy(4).toMillis());
        int deliveryBeforeConvergence = callbackDeliveries.get();
        assertEquals(boundSessionFixture.path("invariants")
                .path("deliveryBeforeRouteApply").asInt(),
            deliveryBeforeConvergence,
            "callback push must not cross RoutePending before atomic route apply");
        assertEquals(
            verification == Verification.BOUND_OUTBOUND_FIFO
                    || verification == Verification.LOCAL_TARGET_STORE_ZERO
                    || verification == Verification.LOCAL_TARGET_CONFLICT
                    || verification
                        == Verification.REMOTE_PROOF_MISMATCH_RETRY
                ? 1
                : 0,
            callbackCleanups.get(),
            "the target producer must release its frame after command 36 admission");
        assertFalse(leaveNotification.isDone(),
            "one-way source leave completion must not gate target progress");

        routeAvailable.set(true);
        ZLinkServiceM6BWireCodec.SessionRelocationRoute command =
            routePeer.command.get();
        assertEquals(committed.objectGeneration(), command.actor().generation());
        assertEquals(
            committed.authorityOwnerGeneration(),
            command.currentAuthorityOwnerGeneration(),
            "command 44 must use the target owner generation committed by Store");
        ZLinkServiceM6BWireCodec.SessionRelocationRoute wrongGeneration =
            new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
                command.relocation(),
                command.coordinator(),
                command.senderRole(),
                command.actor(),
                command.session(),
                command.action(),
                command.previousAuthorityOwnerGeneration(),
                command.currentAuthorityOwnerGeneration() + 1,
                command.targetNodeRid(),
                command.targetNodeGeneration(),
                command.lastAcceptedSessionSequence());
        countCommittedProofReads.set(true);
        ZLinkServiceM6BWireCodec.SessionRelocationRouted ack;
        try {
            if (verification == Verification.LOCAL_TARGET_CONFLICT) {
                CompletionException conflict = assertThrows(
                    CompletionException.class,
                    () -> routePeer.apply(wrongGeneration));
                assertEquals(
                    systems.zlink.framework.errors.ZLinkFrameworkErrorKind
                        .PROTOCOL_ERROR,
                    ((systems.zlink.framework.errors.ZLinkFrameworkException)
                        conflict.getCause()).kind());
                assertEquals(0, committedProofReads.get(),
                    "a local conflicting fingerprint must fail before Store");
                assertEquals(
                    deliveryBeforeConvergence,
                    callbackDeliveries.get(),
                    "a conflicting local command 44 must not mutate the route");
                ack = routePeer.apply(command);
            } else if (verification
                    == Verification.LOCAL_TARGET_STORE_ZERO) {
                ack = routePeer.apply(command);
                assertEquals(0, committedProofReads.get(),
                    "the public local command 44 path must use committed tenure");
            } else if (verification
                    == Verification.REMOTE_PROOF_MISMATCH_RETRY) {
                ZLinkServiceM6BWireCodec.SessionRelocationRouted mismatch =
                    routePeer.apply(command);
                assertEquals(
                    ZLinkServiceM6BWireCodec
                        .SessionRelocationRouteResult.STALE,
                    mismatch.result(),
                    "a Store proof for another target tenure must not apply");
                assertEquals(1, committedProofReads.get());
                assertEquals(
                    deliveryBeforeConvergence,
                    callbackDeliveries.get(),
                    "a mismatched proof must leave pending command 36 queued");
                ack = routePeer.apply(command);
                assertEquals(2, committedProofReads.get(),
                    "a mismatched proof must not poison the accepted proof cache");
            } else {
                CompletableFuture<Void> proofGate = new CompletableFuture<>();
                committedProofReadGate.set(proofGate);
                CompletionStage<ZLinkServiceM6BWireCodec
                    .SessionRelocationRouted> first =
                        routePeer.applyAsync(wrongGeneration);
                awaitValue(committedProofReads, 1, Duration.ofSeconds(2));
                CompletionStage<ZLinkServiceM6BWireCodec
                    .SessionRelocationRouted> duplicate =
                        routePeer.applyAsync(wrongGeneration);
                proofGate.complete(null);
                ZLinkServiceM6BWireCodec.SessionRelocationRouted wrongAck =
                    first.toCompletableFuture().join();
                assertEquals(wrongAck,
                    duplicate.toCompletableFuture().join());
                assertEquals(1, committedProofReads.get(),
                    "identical concurrent command 44 calls must share one read");
                assertEquals(
                    ZLinkServiceM6BWireCodec
                        .SessionRelocationRouteResult.STALE,
                    wrongAck.result(),
                    "Session owner must reject an owner generation not "
                        + "committed by Store");
                assertEquals(
                    deliveryBeforeConvergence,
                    callbackDeliveries.get(),
                    "accepted target proof must not open the outbound route "
                        + "before the exact command 44 applies");
                ack = routePeer.apply(command);
                assertEquals(2, committedProofReads.get(),
                    "a proof that mismatched the first fingerprint must not "
                        + "be reused by the exact command 44");
            }
        } finally {
            CompletableFuture<Void> proofGate =
                committedProofReadGate.getAndSet(null);
            if (proofGate != null) {
                proofGate.complete(null);
            }
            countCommittedProofReads.set(false);
        }
        assertEquals(
            ZLinkServiceM6BWireCodec.SessionRelocationRouteResult.APPLIED,
            ack.result());
        if (verification == Verification.SUCCESSOR_TERMINAL_GATE) {
            verifySuccessorTerminalGate(
                target, targetActor.get(), trace, routePeer, command, ack);
            return;
        }

        dispatchBoundPush(
                target,
                targetActor.get(),
                trace,
                "callback-push-after-apply")
            .toCompletableFuture().get();
        awaitCount(callbackDeliveries, 2, Duration.ofSeconds(2));
        int deliveryAfterRouteApply = callbackDeliveries.get();
        assertEquals(boundSessionFixture.path("applicationBoundary")
                .path("afterAtomicRouteApply")
                .path("eventualPhysicalDeliveryCount").asInt() + 1,
            deliveryAfterRouteApply,
            "route apply must release the pre-apply push and admit the "
                + "post-apply push before command 45");
        assertEquals(
            List.of(
                "callback-push-before-apply",
                "callback-push-after-apply"),
            callbackDeliveryOrder,
            "the Session owner must preserve its outbound admission order");
        trace.add("sessionRouteTerminalDelivered");
        routePeer.routeReply.complete(routePeer.codec
            .encodeSessionRelocationRouted(ack));
        assertEquals(2, callbackCleanups.get());

        target.onDirectJoinSessionRouteTerminal(command, ack);
        target.onDirectJoinSessionRouteTerminal(command, ack);
        assertEquals(boundSessionFixture.path("invariants")
                .path("duplicateRouteTerminalAdditionalDelivery").asInt(),
            callbackDeliveries.get() - deliveryAfterRouteApply,
            "duplicate command 45 must not redeliver the retained push");
        assertEquals(2, callbackCleanups.get());
        awaitAuthorityRelease(locations);

        assertOrder(trace,
            "relocationRequested",
            "targetStateRestored",
            "ownershipCommitted",
            "applicationOperationSubmitted",
            "targetLifecycleCompleted",
            "sourceMembershipLeaveSubmitted",
            "publicJoinCompleted",
            "targetDispatchActivated",
            "targetReadyPublished");
        assertOrder(trace,
            "targetReadyPublished",
            "sourceCleanupCompleted",
            "authorityCompleted",
            "targetApplicationMessageDelivered",
            "sessionRouteConverged",
            "applicationOperationDelivered",
            "callbackPushDelivered",
            "sessionRouteTerminalDelivered");
        assertEquals(-1, trace.indexOf("readyWhileMoving"));
        assertFixtureEdge(
            actorJoinProfile(behaviorFixture).path("additionalOrder"),
            trace,
            "targetLifecycleCompleted",
            "sourceMembershipLeaveSubmitted");
        assertFixtureEdge(
            actorJoinProfile(behaviorFixture).path("additionalOrder"),
            trace,
            "sourceMembershipLeaveSubmitted",
            "publicJoinCompleted");
        JsonNode routeOrder = boundSessionRouteBranch(behaviorFixture)
            .path("requiredOrder");
        assertFixtureEdge(
            routeOrder,
            trace,
            "authorityCompleted",
            "sessionRouteConverged");
        assertFixtureEdge(
            routeOrder,
            trace,
            "sessionRouteConverged",
            "sessionRouteTerminalDelivered");
        assertTrue(boundSessionFixture.path("invariants")
            .path("trafficSubmittedWhileSealedIsRetained").asBoolean());
    }

    private static CompletionStage<Void> dispatch(
        ZLinkActorRuntime target,
        TargetActor actor,
        String payload) {
        return target.submitActorDispatch(
            ACTOR_ID,
            () -> {
                actor.handle(payload);
                return CompletableFuture.completedFuture(null);
            });
    }

    private static CompletionStage<Void> dispatchBoundPush(
        ZLinkActorRuntime target,
        TargetActor actor,
        Trace trace,
        String payload) {
        return target.submitActorDispatch(
            ACTOR_ID,
            () -> {
                trace.add("postApplyOutboundSubmitted");
                return actor.context().boundSession().send(payload).submit();
            });
    }

    private static void verifySuccessorTerminalGate(
        ZLinkActorRuntime target,
        TargetActor actor,
        Trace trace,
        RoutePeer routePeer,
        ZLinkServiceM6BWireCodec.SessionRelocationRoute predecessor,
        ZLinkServiceM6BWireCodec.SessionRelocationRouted exactTerminal)
        throws Exception {
        target.submitActorDispatch(
                ACTOR_ID,
                () -> {
                    trace.add("successorRelocationRequested");
                    actor.context()
                        .joinSpot("successor-room")
                        .timeout(REQUEST_TIMEOUT)
                        .defer();
                    return CompletableFuture.completedFuture(null);
                })
            .toCompletableFuture().get();

        Thread.sleep(100);
        int beforeWrongTerminal =
            trace.count("successorRelocationAdmitted");

        ZLinkServiceM6BWireCodec.SessionRelocationRoute wrongBindingTerminal =
            withBindingGeneration(
                predecessor,
                predecessor.session().bindingGeneration() + 1);
        assertEquals(
            predecessor.relocation(),
            wrongBindingTerminal.relocation());
        assertEquals(
            predecessor.session().sessionRid(),
            wrongBindingTerminal.session().sessionRid());
        assertNotEquals(
            predecessor.session().bindingGeneration(),
            wrongBindingTerminal.session().bindingGeneration());
        target.onDirectJoinSessionRouteTerminal(
            wrongBindingTerminal,
            routed(
                wrongBindingTerminal,
                ZLinkServiceM6BWireCodec.SessionRelocationRouteResult.APPLIED));
        Thread.sleep(100);
        int afterWrongBindingTerminal =
            trace.count("successorRelocationAdmitted");

        routePeer.routeReply.complete(
            routePeer.codec.encodeSessionRelocationRouted(exactTerminal));
        trace.awaitCount(
            "successorRelocationAdmitted", 1, Duration.ofSeconds(2));
        int afterExactTerminal =
            trace.count("successorRelocationAdmitted");

        target.onDirectJoinSessionRouteTerminal(predecessor, exactTerminal);
        target.onDirectJoinSessionRouteTerminal(
            wrongBindingTerminal,
            routed(
                wrongBindingTerminal,
                ZLinkServiceM6BWireCodec.SessionRelocationRouteResult.APPLIED));
        Thread.sleep(100);
        int afterDuplicates = trace.count("successorRelocationAdmitted");

        assertAll(
            "the predecessor terminal owns the exact successor gate",
            () -> assertEquals(
                0,
                beforeWrongTerminal,
                "a successor must remain queued before the predecessor terminal"),
            () -> assertEquals(
                0,
                afterWrongBindingTerminal,
                "a predecessor terminal with the same relocationId and "
                    + "sessionRid but a different bindingGeneration must not "
                    + "admit the successor"),
            () -> assertEquals(
                1,
                afterExactTerminal,
                "the exact predecessor terminal must admit one successor"),
            () -> assertEquals(
                1,
                afterDuplicates,
                "duplicate exact and wrong-binding terminals must not readmit it"));
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRoute
        withBindingGeneration(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute route,
            long bindingGeneration) {
        var session = route.session();
        return new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
            route.relocation(),
            route.coordinator(),
            route.senderRole(),
            route.actor(),
            new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                session.nodeRid(),
                session.nodeGeneration(),
                session.ownerId(),
                session.ownerLeaseGeneration(),
                session.sessionRid(),
                bindingGeneration),
            route.action(),
            route.previousAuthorityOwnerGeneration(),
            route.currentAuthorityOwnerGeneration(),
            route.targetNodeRid(),
            route.targetNodeGeneration(),
            route.lastAcceptedSessionSequence());
    }

    private static ZLinkServiceM6BWireCodec.SessionRelocationRouted routed(
        ZLinkServiceM6BWireCodec.SessionRelocationRoute route,
        ZLinkServiceM6BWireCodec.SessionRelocationRouteResult result) {
        return new ZLinkServiceM6BWireCodec.SessionRelocationRouted(
            route.relocation(),
            route.coordinator(),
            route.actor(),
            route.session(),
            route.action(),
            result,
            route.currentAuthorityOwnerGeneration(),
            route.lastAcceptedSessionSequence());
    }

    private static void awaitCount(
        AtomicInteger value,
        int expected,
        Duration timeout) throws Exception {
        long deadline = System.nanoTime() + timeout.toNanos();
        while (value.get() < expected && System.nanoTime() < deadline) {
            Thread.sleep(2);
        }
        assertEquals(
            expected,
            value.get(),
            "atomic route apply did not release pre-apply traffic before "
                + "the post-apply submission");
    }

    private static void awaitValue(
        AtomicInteger value,
        int expected,
        Duration timeout) throws Exception {
        long deadline = System.nanoTime() + timeout.toNanos();
        while (value.get() < expected && System.nanoTime() < deadline) {
            Thread.sleep(2);
        }
        assertEquals(expected, value.get(),
            "Store proof read did not enter the single-flight window");
    }

    private static ZLinkLocationRepository tracingAuthority(
        ZLinkInMemoryLocationStore store,
        InMemoryRelocationStore relocations,
        Trace trace,
        AtomicReference<TargetActor> targetActor,
        AtomicReference<ZLinkActorRuntime> targetRuntime,
        AtomicBoolean countCommittedProofReads,
        AtomicInteger committedProofReads,
        AtomicReference<CompletableFuture<Void>> committedProofReadGate,
        AtomicBoolean returnMismatchedCommittedProof) {
        return (ZLinkLocationRepository) Proxy.newProxyInstance(
            ZLinkLocationRepository.class.getClassLoader(),
            new Class<?>[] {ZLinkLocationRepository.class},
            (proxy, method, arguments) -> {
                boolean countedProofRead = countCommittedProofReads.get()
                    && method.getName().equals("read")
                    && arguments != null
                    && arguments.length > 0
                    && ZLinkAuthorityKeyCodec.actor(ACTOR_ID).equals(
                        arguments[0]);
                if (countedProofRead) {
                    committedProofReads.incrementAndGet();
                }
                Object result;
                try {
                    result = method.invoke(store, arguments);
                } catch (InvocationTargetException failure) {
                    throw failure.getCause();
                }
                if (!(result instanceof CompletionStage<?> stage)) {
                    return result;
                }
                CompletionStage<?> effective = stage;
                CompletableFuture<Void> proofGate =
                    committedProofReadGate.get();
                if (countedProofRead && proofGate != null) {
                    effective = proofGate.thenCompose(ignored -> stage);
                }
                return effective.thenApply(value -> {
                    observeAuthority(
                        store,
                        relocations,
                        trace,
                        targetActor.get(),
                        targetRuntime.get());
                    if (countedProofRead
                        && returnMismatchedCommittedProof.compareAndSet(
                            true, false)
                        && value instanceof ZLinkAuthoritySnapshot snapshot) {
                        return new ZLinkAuthoritySnapshot(
                            snapshot.storeVersion(),
                            snapshot.payload(),
                            snapshot.objectGeneration(),
                            snapshot.authorityOwnerGeneration() + 1,
                            snapshot.ownerId(),
                            snapshot.ownerLeaseGeneration(),
                            snapshot.allocation(),
                            snapshot.pendingCreation(),
                            snapshot.storeNow());
                    }
                    return value;
                });
            });
    }

    private static void observeAuthority(
        ZLinkInMemoryLocationStore store,
        InMemoryRelocationStore relocations,
        Trace trace,
        TargetActor actor,
        ZLinkActorRuntime runtime) {
        ZLinkAuthorityReadResult read = store.read(
                ZLinkAuthorityKeyCodec.actor(ACTOR_ID),
                () -> false)
            .toCompletableFuture().join();
        if (!(read instanceof ZLinkAuthoritySnapshot snapshot)) {
            return;
        }
        var authority = new ZLinkActorAuthorityPayloadCodec()
            .decode(snapshot.payload()).orElse(null);
        var publication = ZLinkCanonicalRelocationObservation
            .observe(snapshot.payload(), relocations);
        if (authority != null
            && authority.state()
                == ZLinkActorAuthorityPayloadCodec.State.CREATING
            && publication.relocationPublished()) {
            trace.addOnce("ownershipCommitted");
        }
        if (authority != null
            && authority.state() == ZLinkActorAuthorityPayloadCodec.State.READY
            && publication.relocationPublished()
            && trace.indexOf("targetReadyPublished") < 0) {
            if (actor != null && runtime != null && !runtime.isMoving(actor)) {
                trace.addOnce("targetDispatchActivated");
            } else {
                trace.addOnce("readyWhileMoving");
            }
            assertTrue(actor == null || actor.handlerCount() == 0,
                "application handlers must remain closed before Ready");
            trace.addOnce("targetReadyPublished");
        }
        if (publication.sourceCleanupMarked()
            && !publication.authorityCompleted()) {
            trace.addStoreTransition(
                "sourceCleanupCompleted", snapshot.storeVersion());
        }
        if (publication.authorityCompleted()) {
            trace.addStoreTransition(
                "authorityCompleted", snapshot.storeVersion());
        }
    }

    private static void awaitAuthorityRelease(
        ZLinkInMemoryLocationStore locations) throws Exception {
        long deadline = System.nanoTime() + Duration.ofSeconds(2).toNanos();
        while (System.nanoTime() < deadline) {
            ZLinkAuthorityReadResult read = locations.read(
                    ZLinkAuthorityKeyCodec.actor(ACTOR_ID),
                    () -> false)
                .toCompletableFuture().get();
            if (read instanceof ZLinkAuthoritySnapshot snapshot
                && !ZLinkCanonicalRelocationObservation
                    .observe(snapshot.payload()).relocationPublished()) {
                return;
            }
            Thread.sleep(5);
        }
        fail("command 45 did not release the canonical relocation root");
    }

    private static ZLinkLocationOwnerToken owner(
        ZLinkInMemoryLocationStore store,
        String ownerId) throws Exception {
        return assertInstanceOf(
            ZLinkOwnerLeaseClaimed.class,
            store.claimOwnerLease(ownerId, Duration.ofMinutes(1))
                .toCompletableFuture().get()).token();
    }

    private static void createAuthority(
        ZLinkInMemoryLocationStore store,
        ZLinkMeshNodeDescriptor descriptor,
        ZLinkLocationOwnerToken owner,
        ZLinkPlacementObjectKind kind,
        String key,
        String stableType,
        byte[] payload,
        ZLinkPlacementCapacityBundle capacity) throws Exception {
        ZLinkObjectReservation reservation = assertInstanceOf(
            ZLinkObjectReserved.class,
            store.reserve(
                    new ZLinkObjectReservationRequest(
                        kind,
                        key,
                        stableType,
                        "seed-" + key,
                        new byte[32],
                        32,
                        new ZLinkMeshNodeDescriptorKey(
                            descriptor.meshName(), descriptor.rid()),
                        descriptor.lifecycleGeneration(),
                        owner,
                        payload,
                        capacity),
                    () -> false)
                .toCompletableFuture().get()).reservation();
        assertEquals(
            ZLinkObjectCommitResult.COMMITTED,
            store.commit(reservation, payload, () -> false)
                .toCompletableFuture().get());
    }

    private static ZLinkMeshNodeDescriptor descriptor(
        RoutingId rid,
        long lifecycle,
        ZLinkLocationOwnerToken owner,
        boolean target) {
        List<ZLinkObjectCapability> capabilities = new ArrayList<>();
        capabilities.add(new ZLinkObjectCapability(
            ZLinkPlacementObjectKind.ACTOR,
            ACTOR_TYPE,
            ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
            true,
            0));
        if (target) {
            capabilities.add(new ZLinkObjectCapability(
                ZLinkPlacementObjectKind.USER_SPOT,
                ROOM_TYPE,
                ZLinkObjectMaintenancePolicyKind.SNAPSHOT,
                true,
                0));
        }
        return new ZLinkMeshNodeDescriptor(
            MESH,
            rid,
            lifecycle,
            1,
            "inproc://" + rid,
            Map.of(MESH, 100),
            1,
            capabilities,
            ZLinkMeshNodeObjectRole.SERVER,
            Optional.of(rid + "-entry-00000000-0000-4000-8000-000000000001"),
            100,
            new ZLinkPlacementCapacity(
                new ZLinkCapacityUsage(0, 0, 8),
                new ZLinkCapacityUsage(0, 0, target ? 4 : 0),
                target
                    ? List.of(new ZLinkSpotTypeCapacity(
                        ZLinkPlacementObjectKind.USER_SPOT,
                        ROOM_TYPE,
                        new ZLinkCapacityUsage(0, 0, 4)))
                    : List.of()),
            new ZLinkActivationConcurrency(0, 16),
            Optional.empty(),
            ZLinkFrameworkRuntimeState.SERVING,
            "test-security",
            owner.ownerId(),
            owner.leaseGeneration(),
            Instant.now());
    }

    private static ZLinkInternalSpotNode spotNode(
        RoutingId rid,
        long localLease,
        AtomicBoolean routeAvailable,
        AtomicInteger callbackDeliveries,
        List<String> callbackDeliveryOrder,
        Trace trace,
        AtomicReference<ZLinkBackendActorRef> actorRef,
        AtomicReference<ActorTenureProjection> tenureProjection,
        RoutePeer routePeer) {
        Map<ZLinkBackendActorRef, long[]> rememberedAuthorities =
            new ConcurrentHashMap<>();
        AtomicReference<RelocatingBinding> relocatingBinding =
            new AtomicReference<>();
        ZLinkBackendSpot entrySpot = (ZLinkBackendSpot) Proxy.newProxyInstance(
            ZLinkBackendSpot.class.getClassLoader(),
            new Class<?>[] {ZLinkBackendSpot.class},
            (proxy, method, arguments) -> defaultValue(method.getReturnType()));
        return (ZLinkInternalSpotNode) Proxy.newProxyInstance(
            ZLinkInternalSpotNode.class.getClassLoader(),
            new Class<?>[] {ZLinkInternalSpotNode.class},
            (proxy, method, arguments) -> switch (method.getName()) {
                case "routingId" -> rid;
                case "entrySpot" -> entrySpot;
                case "createActor" -> {
                    if (arguments[1] instanceof Message request) {
                        request.close();
                    }
                    ZLinkBackendActorRef created = new ZLinkBackendActorRef(
                        rid, (String) arguments[0], 1);
                    if (actorRef != null) {
                        actorRef.set(created);
                    }
                    yield created;
                }
                case "prepareActorTransfer" ->
                    throw new UnsupportedOperationException();
                case "registerTransferredActor" -> {
                    if (actorRef != null
                        && arguments != null
                        && arguments.length > 0
                        && arguments[0] instanceof ZLinkBackendActorRef current) {
                        actorRef.set(current);
                    }
                    yield null;
                }
                case "rememberActorAuthority" -> {
                    ZLinkBackendActorRef current =
                        (ZLinkBackendActorRef) arguments[0];
                    long ownerGeneration = (long) arguments[1];
                    long leaseGeneration = arguments.length > 2
                        ? (long) arguments[2]
                        : localLease;
                    rememberedAuthorities.put(
                        current,
                        new long[] {ownerGeneration, leaseGeneration});
                    if (tenureProjection != null) {
                        tenureProjection.set(new ActorTenureProjection(
                            current, ownerGeneration, leaseGeneration));
                    }
                    if (actorRef != null) {
                        actorRef.set(current);
                    }
                    yield null;
                }
                case "actorMembershipEpoch" -> 1L;
                case "localAuthorityLeaseGeneration" -> localLease;
                case "localNodeGeneration" -> rid.equals(SOURCE_RID)
                    ? 11L : 19L;
                case "localAuthorityOwnerId" -> rid.equals(SOURCE_RID)
                    ? "source-owner" : "target-owner";
                case "actorNodeGeneration" -> {
                    ZLinkBackendActorRef current =
                        (ZLinkBackendActorRef) arguments[0];
                    yield current.nodeRid().equals(SOURCE_RID) ? 11L : 19L;
                }
                case "actorAuthorityOwnerGeneration" -> {
                    ZLinkBackendActorRef current =
                        (ZLinkBackendActorRef) arguments[0];
                    long[] remembered = rememberedAuthorities.get(current);
                    yield current.nodeRid().equals(rid)
                        ? 1L
                        : remembered == null ? 0L : remembered[0];
                }
                case "actorAuthorityOwnerLeaseGeneration" -> {
                    ZLinkBackendActorRef current =
                        (ZLinkBackendActorRef) arguments[0];
                    long[] remembered = rememberedAuthorities.get(current);
                    yield current.nodeRid().equals(rid)
                        ? localLease
                        : remembered == null ? 0L : remembered[1];
                }
                case "actorLookup" -> actorRef == null ? null : actorRef.get();
                case "installRelocatingActorBoundSession" -> {
                    var actorRoute =
                        (ZLinkServiceM6BWireCodec.ActorRouteFence) arguments[0];
                    var session =
                        (ZLinkServiceM6BWireCodec.SessionOwnerFence) arguments[1];
                    relocatingBinding.set(new RelocatingBinding(
                        actorRoute, session));
                    yield null;
                }
                case "boundSessionRoute" -> {
                    ZLinkBackendActorRef current =
                        (ZLinkBackendActorRef) arguments[0];
                    RelocatingBinding binding = relocatingBinding.get();
                    yield binding != null
                            && binding.actor().actor().equals(current)
                        ? Optional.of(new ZLinkInternalSpotNode.BoundSessionRoute(
                            binding.session().nodeRid(),
                            binding.session().nodeGeneration(),
                            binding.session().sessionRid(),
                            binding.session().bindingGeneration(),
                            0))
                        : Optional.empty();
                }
                case "sendActorBoundSession" -> {
                    RelocatingBinding binding = relocatingBinding.get();
                    if (binding == null) {
                        yield false;
                    }
                    @SuppressWarnings("unchecked")
                    List<Message> parts = (List<Message>) arguments[1];
                    var command = new ZLinkServiceM6BWireCodec.BoundSessionSend(
                        binding.actor(),
                        binding.session().bindingGeneration());
                    yield routePeer.acceptBoundSessionSend(
                        rid,
                        binding.actor().targetNodeGeneration(),
                        command,
                        ZLinkServiceM6AWireCodec
                            .encodeFrameworkMultipart(parts));
                }
                case "joinActor" -> {
                    trace.add("successorRelocationAdmitted");
                    yield CompletableFuture.failedFuture(
                        new IllegalStateException(
                            "successor admission observation completed"));
                }
                case "destroyActor" -> {
                    if (rid.equals(SOURCE_RID)) {
                        trace.addOnce("sourceCleanupStarted");
                    }
                    yield CompletableFuture.completedFuture(null);
                }
                case "close" -> null;
                default -> defaultValue(method.getReturnType());
            });
    }

    @SuppressWarnings("unchecked")
    private static String callbackPayload(Object[] arguments) throws Exception {
        List<Message> parts = (List<Message>) arguments[1];
        byte[] body = ZLinkStreamFrameCodec.tryDecode(
                parts.getFirst().toByteArray())
            .orElseThrow(() -> new AssertionError(
                "bound-Session callback was not a STREAM frame"))
            .body();
        return new ObjectMapper().readValue(body, String.class);
    }

    private record ActorTenureProjection(
        ZLinkBackendActorRef actor,
        long authorityOwnerGeneration,
        long ownerLeaseGeneration) {
    }

    private record RelocatingBinding(
        ZLinkServiceM6BWireCodec.ActorRouteFence actor,
        ZLinkServiceM6BWireCodec.SessionOwnerFence session) {
    }

    private RouteRuntimeSession startRouteRuntime(
        ZLinkActorRuntime actors,
        ZLinkInternalSpotNode sessionOwnerNode,
        AtomicBoolean routeAvailable,
        AtomicInteger callbackDeliveries,
        List<String> callbackDeliveryOrder,
        Trace trace) throws Exception {
        RouteRuntimeSession.current.set(null);
        Queue<ZLinkBackendStreamReceived> received =
            new ConcurrentLinkedQueue<>();
        AtomicBoolean receivePermit = new AtomicBoolean();
        ZLinkBackendStreamSocket delegate = sessionStream(
            sessionOwnerNode,
            routeAvailable,
            callbackDeliveries,
            callbackDeliveryOrder,
            trace);
        try (Message initial = Message.from(routeRuntimeFrame())) {
            Message retained = Message.from(initial.toByteArray());
            received.add(new ZLinkBackendStreamReceived(
                Optional.of(SESSION_RID),
                List.of(retained),
                retained::close));
        }
        ZLinkBackendStreamSocket routeStream =
            (ZLinkBackendStreamSocket) Proxy.newProxyInstance(
                ZLinkBackendStreamSocket.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendStreamSocket.class},
                (proxy, method, arguments) -> {
                    try {
                        return switch (method.getName()) {
                            case "waitForReadable" -> {
                                boolean ready = !received.isEmpty();
                                if (ready) {
                                    receivePermit.set(true);
                                }
                                yield ready;
                            }
                            case "recv" -> receivePermit.compareAndSet(
                                    true, false)
                                ? received.poll()
                                : null;
                            case "close" -> {
                                ZLinkBackendStreamReceived pending;
                                while ((pending = received.poll()) != null) {
                                    pending.close();
                                }
                                delegate.close();
                                yield null;
                            }
                            default -> method.invoke(delegate, arguments);
                        };
                    } catch (InvocationTargetException failure) {
                        throw failure.getCause();
                    }
                });
        DefaultZLinkFrameworkOptions options =
            new DefaultZLinkFrameworkOptions();
        options.addStreamNode("route-stream")
            .bind("tcp://127.0.0.1:18081")
            .registerSession(RouteRuntimeSession.class);
        ZLinkFrameworkRegistration registration = options.registration();
        ZLinkStreamRuntime runtime = new ZLinkStreamRuntime(
            new RouteRuntimeProvider(routeStream),
            new ZLinkBackendAdapterOptions(Duration.ofSeconds(1)),
            registration,
            Map.of("session-owner", sessionOwnerNode),
            Map.of(),
            new ZLinkJsonMessageSerializer(),
            actors,
            ZLinkHandlerActivator.reflection(),
            ignored -> true,
            null,
            null,
            new RouteRuntimeContext(),
            false,
            (ignoredBackend, ignoredKey) ->
                (ignoredReady, ignoredShutdown) ->
                    CompletableFuture.completedFuture(null));
        routeRuntimes.add(runtime);
        long deadline = System.nanoTime() + Duration.ofSeconds(5).toNanos();
        RouteRuntimeSession session;
        while ((session = RouteRuntimeSession.current.get()) == null
            && System.nanoTime() < deadline) {
            Thread.sleep(1);
        }
        if (session == null) {
            throw new AssertionError("public route Session was not created");
        }
        session.runtime = runtime;
        return session;
    }

    private static byte[] routeRuntimeFrame() {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.JSON,
            java.util.EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            "route-runtime-init",
            Map.of());
        return ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header),
            "{}".getBytes(java.nio.charset.StandardCharsets.UTF_8));
    }

    public static final class RouteRuntimeSession implements ZLinkSession {
        private static final AtomicReference<RouteRuntimeSession> current =
            new AtomicReference<>();
        private final ZLinkSessionContext context;
        private volatile ZLinkStreamRuntime runtime;

        public RouteRuntimeSession(ZLinkSessionContext context) {
            this.context = context;
            current.set(this);
        }

        private ZLinkStreamRuntime runtime() {
            return runtime;
        }

        @Override
        public ZLinkSessionContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onConnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDisconnected() {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onError(ZLinkStreamError error) {
            return CompletableFuture.completedFuture(null);
        }

        @Override
        public CompletionStage<Void> onDispatch(
            ZLinkSessionDispatchContext dispatch,
            systems.zlink.framework.messaging.ZLinkMessage payload) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private record RouteRuntimeProvider(
        ZLinkBackendStreamSocket stream)
        implements ZLinkBackendAdapterProvider {
        @Override
        public ZLinkChannelBackendAdapter createChannelAdapter(
            ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkSpotBackendAdapter createSpotAdapter(
            ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }

        @Override
        public ZLinkStreamBackendAdapter createStreamAdapter(
            ZLinkBackendAdapterOptions options) {
            return (context, meshNode) -> stream;
        }

        @Override
        public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
            ZLinkBackendAdapterOptions options) {
            throw new UnsupportedOperationException();
        }
    }

    private static final class RouteRuntimeContext
        implements ZLinkBackendContext {
        @Override public String name() { return "route-runtime"; }
        @Override public void close() { }
        @Override public void shutdown() { }
    }

    private static ZLinkBackendStreamSocket sessionStream(
        ZLinkInternalSpotNode sessionOwnerNode,
        AtomicBoolean routeAvailable,
        AtomicInteger callbackDeliveries,
        List<String> callbackDeliveryOrder,
        Trace trace) {
        return (ZLinkBackendStreamSocket) Proxy.newProxyInstance(
                ZLinkBackendStreamSocket.class.getClassLoader(),
                new Class<?>[] {ZLinkBackendStreamSocket.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "boundActorBindingGeneration" -> 7L;
                    case "bindActor" ->
                        (ZLinkBackendActorBindOperation) timeout ->
                                CompletableFuture.completedFuture(null);
                    case "unbindActor" ->
                        (ZLinkBackendActorUnbindOperation) timeout ->
                                CompletableFuture.completedFuture(null);
                    case "relocateBoundActor" -> {
                        ZLinkBackendActorRef target =
                            (ZLinkBackendActorRef) arguments[3];
                        long ownerGeneration = sessionOwnerNode
                            .actorAuthorityOwnerGeneration(target);
                        long ownerLeaseGeneration = sessionOwnerNode
                            .actorAuthorityOwnerLeaseGeneration(target);
                        yield ownerGeneration > 0 && ownerLeaseGeneration > 0
                            ? CompletableFuture.completedFuture(null)
                            : CompletableFuture.failedFuture(
                                new IllegalStateException(
                                    "canonical target authority was not "
                                        + "propagated to native Session bind: "
                                        + "ownerGeneration=" + ownerGeneration
                                        + " ownerLeaseGeneration="
                                        + ownerLeaseGeneration));
                    }
                    case "requestBoundActor", "requestExactActor" ->
                        CompletableFuture.completedFuture(List.of());
                    case "sendBoundSessionPush" -> {
                        if (!routeAvailable.get()) {
                            yield false;
                        }
                        callbackDeliveryOrder.add(callbackPayload(arguments));
                        callbackDeliveries.incrementAndGet();
                        trace.addOnce("sessionRouteConverged");
                        trace.add("applicationOperationDelivered");
                        trace.add("callbackPushDelivered");
                        yield true;
                    }
                    case "close" -> null;
                    default -> defaultValue(method.getReturnType());
                });
    }

    private static void assertOrder(Trace trace, String... events) {
        int previous = -1;
        for (String event : events) {
            int current = trace.indexOf(event);
            assertTrue(current >= 0, "missing observable event: " + event
                + " in " + trace.events);
            assertTrue(current > previous,
                "observable order differs at " + event + ": " + trace.events);
            previous = current;
        }
    }

    private static void assertFixtureEdge(
        JsonNode edges,
        Trace trace,
        String predecessor,
        String successor) {
        boolean declared = false;
        for (JsonNode edge : edges) {
            if (edge.size() == 2
                && predecessor.equals(edge.get(0).asText())
                && successor.equals(edge.get(1).asText())) {
                declared = true;
                break;
            }
        }
        assertTrue(declared,
            "shared relocation fixture is missing edge "
                + predecessor + " < " + successor);
        assertOrder(trace, predecessor, successor);
    }

    private static JsonNode actorJoinProfile(JsonNode fixture) {
        for (JsonNode profile : fixture.path("profiles")) {
            if ("actorJoin".equals(profile.path("name").asText())) {
                return profile;
            }
        }
        throw new AssertionError(
            "shared relocation fixture has no actorJoin profile");
    }

    private static JsonNode boundSessionRouteBranch(JsonNode fixture) {
        for (JsonNode branch : fixture.path("optionalBranches")) {
            if ("boundSessionRoute".equals(branch.path("name").asText())) {
                return branch;
            }
        }
        throw new AssertionError(
            "shared relocation fixture has no boundSessionRoute branch");
    }

    private static JsonNode fixture(String relativePath) throws Exception {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(relativePath);
            if (Files.isRegularFile(candidate)) {
                return new ObjectMapper().readTree(Files.readString(candidate));
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared relocation fixture was not found: " + relativePath);
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

    public static final class SourceActorFactory implements ZLinkActorFactory {
        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            return CompletableFuture.completedFuture(new SourceActor(context));
        }
    }

    private record SourceActor(ZLinkActorContext context) implements ZLinkActor {
    }

    public static final class TargetActorFactory implements ZLinkActorFactory {
        private static final AtomicReference<Trace> trace =
            new AtomicReference<>();

        @Override
        public CompletionStage<ZLinkActor> create(ZLinkActorContext context) {
            Trace current = trace.get();
            current.add("targetStateRestored");
            return CompletableFuture.completedFuture(
                new TargetActor(context, current));
        }
    }

    private static final class TargetActor implements ZLinkActor {
        private final ZLinkActorContext context;
        private final Trace trace;
        private final AtomicInteger handlers = new AtomicInteger();
        private final AtomicInteger joinCompletions = new AtomicInteger();

        private TargetActor(ZLinkActorContext context, Trace trace) {
            this.context = context;
            this.trace = trace;
        }

        @Override
        public ZLinkActorContext context() {
            return context;
        }

        @Override
        public CompletionStage<Void> onJoinCompleted(
            ZLinkActorJoinCompletion completion) {
            if (joinCompletions.incrementAndGet() == 1) {
                assertInstanceOf(
                    ZLinkActorJoinCompletion.Accepted.class,
                    completion);
                assertEquals(0, handlers.get());
                trace.add("publicJoinCompleted");
            } else {
                trace.add("successorRelocationCompleted");
            }
            return CompletableFuture.completedFuture(null);
        }

        private void handle(String payload) {
            handlers.incrementAndGet();
            trace.add(payload);
        }

        private int handlerCount() {
            return handlers.get();
        }
    }

    private static final class TargetSpot implements ZLinkSpot<TargetActor> {
        private final Trace trace;
        private final boolean sendCallbackPush;

        private TargetSpot(Trace trace, boolean sendCallbackPush) {
            this.trace = trace;
            this.sendCallbackPush = sendCallbackPush;
        }

        @Override
        public ZLinkSpotContext context() {
            return null;
        }

        @Override
        public CompletionStage<Void> onJoinedActor(TargetActor actor) {
            assertEquals(0, actor.handlerCount());
            if (!sendCallbackPush) {
                trace.add("targetLifecycleCompleted");
                return CompletableFuture.completedFuture(null);
            }
            return actor.context().boundSession().send(
                    "callback-push-before-apply")
                .submit()
                .thenRun(() -> trace.add("targetLifecycleCompleted"));
        }

        @Override
        public CompletionStage<Void> onLeaveActor(TargetActor actor) {
            return CompletableFuture.completedFuture(null);
        }
    }

    private static final class ImmediateAdmission
        implements ZLinkOneWayCalls.Admission {
        private final AtomicInteger cleanups;
        private final Trace trace;

        private ImmediateAdmission(AtomicInteger cleanups, Trace trace) {
            this.cleanups = cleanups;
            this.trace = trace;
        }

        @Override
        public CompletionStage<Void> submit(
            ZLinkBackendObject backend,
            ZLinkBackendAdmissionKey key,
            Supplier<Boolean> submission,
            Runnable cleanup,
            Duration timeout) {
            boolean boundSession =
                key.kind() == ZLinkBackendAdmissionKey.Kind.BOUND_SESSION;
            if (boundSession) {
                trace.add("applicationOperationSubmitted");
            }
            try {
                return submission.get()
                    ? CompletableFuture.completedFuture(null)
                    : CompletableFuture.failedFuture(
                        new AssertionError("ordinary admission was backpressured"));
            } finally {
                cleanup.run();
                if (boundSession) {
                    cleanups.incrementAndGet();
                }
            }
        }

        @Override
        public CompletionStage<Void> submitDetached(
            ZLinkBackendObject backend,
            ZLinkBackendAdmissionKey key,
            Supplier<Boolean> submission,
            Runnable cleanup,
            Duration timeout) {
            return CompletableFuture.failedFuture(new AssertionError(
                "the target emitter must not retain bound-Session frames"));
        }

        @Override
        public void releaseDetached(
            ZLinkBackendObject backend,
            ZLinkBackendAdmissionKey key) {
            throw new AssertionError(
                "the target emitter must not own a retained release");
        }

        @Override
        public void terminateDetached(
            ZLinkBackendObject backend,
            ZLinkBackendAdmissionKey key,
            Throwable failure) {
            throw new AssertionError(
                "the target emitter must not own a retained termination");
        }
    }

    private static final class RoutePeer {
        private final Trace trace;
        private final ZLinkServiceM6BWireCodec codec =
            new ZLinkServiceM6BWireCodec();
        private final AtomicReference<ZLinkSessionActorsRuntime> owner =
            new AtomicReference<>();
        private final AtomicReference<ZLinkStreamRuntime> publicRuntime =
            new AtomicReference<>();
        private final CompletableFuture<Void> routeRequested =
            new CompletableFuture<>();
        private final CompletableFuture<byte[]> routeReply =
            new CompletableFuture<>();
        private final AtomicReference<
            ZLinkServiceM6BWireCodec.SessionRelocationRoute> command =
                new AtomicReference<>();

        private RoutePeer(Trace trace) {
            this.trace = trace;
        }

        private void attach(ZLinkSessionActorsRuntime sessionOwner) {
            assertTrue(owner.compareAndSet(null, sessionOwner));
        }

        private void attach(RouteRuntimeSession session) {
            assertTrue(owner.compareAndSet(
                null,
                (ZLinkSessionActorsRuntime) session.context().actors()));
            assertTrue(publicRuntime.compareAndSet(null, session.runtime()));
        }

        private boolean acceptBoundSessionSend(
            RoutingId sourceNodeRid,
            long sourceNodeGeneration,
            ZLinkServiceM6BWireCodec.BoundSessionSend command,
            ZLinkServiceM6AWireCodec.ApplicationPayload payload) {
            ZLinkSessionActorsRuntime aggregate = owner.get();
            return aggregate != null
                && aggregate.matchesBoundSessionSend(
                    sourceNodeRid, sourceNodeGeneration, command)
                && aggregate.acceptBoundSessionSend(
                    sourceNodeRid, sourceNodeGeneration, command, payload);
        }

        private ZLinkServiceM6BWireCodec.SessionRelocationRouted apply(
            ZLinkServiceM6BWireCodec.SessionRelocationRoute route) {
            return applyAsync(route).toCompletableFuture().join();
        }

        private CompletionStage<ZLinkServiceM6BWireCodec
            .SessionRelocationRouted> applyAsync(
                ZLinkServiceM6BWireCodec.SessionRelocationRoute route) {
            ZLinkStreamRuntime runtime = publicRuntime.get();
            if (runtime == null) {
                return owner.get().applyRelocationRouteCommand(route);
            }
            return runtime.handleSessionRelocationRoute(
                    route.targetNodeRid(),
                    codec.encodeSessionRelocationRoute(route))
                .thenApply(codec::decodeSessionRelocationRouted);
        }

        private ZLinkInternalMeshNode node() {
            return (ZLinkInternalMeshNode) Proxy.newProxyInstance(
                ZLinkInternalMeshNode.class.getClassLoader(),
                new Class<?>[] {ZLinkInternalMeshNode.class},
                (proxy, method, arguments) -> switch (method.getName()) {
                    case "requestSessionRelocationSeal" -> {
                        var seal = codec.decodeSessionRelocationSeal(
                            (byte[]) arguments[1]);
                        ZLinkStreamRuntime runtime = publicRuntime.get();
                        if (runtime == null) {
                            var sealed = owner.get()
                                .applyRelocationSealCommand(seal)
                                .toCompletableFuture().join();
                            yield CompletableFuture.completedFuture(
                                codec.encodeSessionRelocationSealed(sealed));
                        }
                        yield runtime.handleSessionRelocationSeal(
                            seal.actor().actor().nodeRid(),
                            codec.encodeSessionRelocationSeal(seal));
                    }
                    case "requestSessionRelocationRoute" -> {
                        command.set(codec.decodeSessionRelocationRoute(
                            (byte[]) arguments[1]));
                        routeRequested.complete(null);
                        yield routeReply;
                    }
                    case "close" -> null;
                    default -> defaultValue(method.getReturnType());
                });
        }
    }

    private static final class Trace {
        private final CopyOnWriteArrayList<String> events =
            new CopyOnWriteArrayList<>();
        private final Map<String, String> storeVersions =
            new ConcurrentHashMap<>();

        private void add(String event) {
            events.add(event);
        }

        private void addOnce(String event) {
            if (!events.contains(event)) {
                events.add(event);
            }
        }

        private void addStoreTransition(String event, String storeVersion) {
            if (storeVersions.putIfAbsent(event, storeVersion) == null) {
                addOnce(event);
            }
        }

        private String storeVersion(String event) {
            return storeVersions.get(event);
        }

        private int indexOf(String event) {
            return events.indexOf(event);
        }

        private int count(String event) {
            return (int) events.stream()
                .filter(event::equals)
                .count();
        }

        private void await(String event, Duration timeout) throws Exception {
            long deadline = System.nanoTime() + timeout.toNanos();
            while (System.nanoTime() < deadline) {
                if (events.contains(event)) {
                    return;
                }
                Thread.sleep(2);
            }
            fail("missing observable event " + event + ": " + events);
        }

        private void awaitCount(
            String event,
            int expected,
            Duration timeout) throws Exception {
            long deadline = System.nanoTime() + timeout.toNanos();
            while (System.nanoTime() < deadline) {
                if (count(event) >= expected) {
                    return;
                }
                Thread.sleep(2);
            }
            fail("missing observable event count " + event + "="
                + expected + ": " + events);
        }
    }
}
