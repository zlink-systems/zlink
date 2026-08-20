package systems.zlink.framework.runtime.spots;

import java.util.ArrayList;
import java.util.List;
import java.time.Duration;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CancellationException;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ThreadLocalRandom;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkActorJoinRelocationPort;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocatableActorFactory;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocationPolicy;

/**
 * Builds the reversible source half of one Entry Spot Actor relocation from
 * the live Actor and its exact authority row.
 */
final class ZLinkStandaloneActorRelocationSourceBuilder {
    private static final int PAGE_SIZE = 1000;
    private static final int MAX_DESCRIPTORS = 65_536;

    private final String meshName;
    private final RoutingId localNodeRid;
    private final long localNodeGeneration;
    private final ZLinkLocationRepository locations;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkActorSessionCoordinator actors;
    private final ZLinkRelocationAdapterRegistry adapters;
    private final Map<String, RelocatableActorFactory<?>>
        factories;
    private final ZLinkSpotRuntime relocationReplies;
    //  Command 42 sender. A bound Session cannot relocate when the peer does
    //  not support the exact seal/high-water barrier.
    private final ZLinkSessionRelocationPeerClient sessionSealer;
    private final Duration sessionRelocationSealTimeout;
    private final ZLinkActorAuthorityPayloadCodec authorities =
        new ZLinkActorAuthorityPayloadCodec();

    ZLinkStandaloneActorRelocationSourceBuilder(
        String meshName,
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        Map<String, RelocatableActorFactory<?>>
            factories,
        ZLinkSpotRuntime relocationReplies,
        ZLinkSessionRelocationPeerClient sessionSealer) {
        this(
            meshName,
            localNodeRid,
            localNodeGeneration,
            locations,
            coordinator,
            actors,
            adapters,
            factories,
            relocationReplies,
            sessionSealer,
            Duration.ofSeconds(3));
    }

    ZLinkStandaloneActorRelocationSourceBuilder(
        String meshName,
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        Map<String, RelocatableActorFactory<?>> factories,
        ZLinkSpotRuntime relocationReplies,
        ZLinkSessionRelocationPeerClient sessionSealer,
        Duration sessionRelocationSealTimeout) {
        this.sessionSealer = sessionSealer;
        if (sessionRelocationSealTimeout == null
            || sessionRelocationSealTimeout.isZero()
            || sessionRelocationSealTimeout.isNegative()) {
            throw new IllegalArgumentException(
                "session relocation seal timeout must be positive");
        }
        this.sessionRelocationSealTimeout = sessionRelocationSealTimeout;
        this.meshName = requireText(meshName, "meshName");
        this.localNodeRid = Objects.requireNonNull(
            localNodeRid, "localNodeRid");
        // localNodeGeneration is a node lifecycle-generation opaque equality
        // token (.NET ulong, spec 01-glossary "Lifecycle generation"): full
        // range, only zero is unassigned. A signed `<= 0` sentinel wrongly
        // rejects a legitimate negative-as-long value.
        if (localNodeGeneration == 0) {
            throw new IllegalArgumentException(
                "local node generation must be nonzero");
        }
        this.localNodeGeneration = localNodeGeneration;
        this.locations = Objects.requireNonNull(locations, "locations");
        this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
        this.actors = Objects.requireNonNull(actors, "actors");
        this.adapters = Objects.requireNonNull(adapters, "adapters");
        this.factories = Map.copyOf(
            Objects.requireNonNull(factories, "factories"));
        this.relocationReplies = Objects.requireNonNull(
            relocationReplies, "relocationReplies");
    }

    CompletionStage<PreparedSource> prepare(
        String actorId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(targetPolicy, "targetPolicy");
        Objects.requireNonNull(cancellation, "cancellation");
        ZLinkActor actor = activeActor(actorId, cancellation);
        if (actor == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "Actor is not active locally: " + actorId));
        }
        return admit(actorId, targetPolicy, cancellation)
            .thenCompose(admission -> sealAndCapture(
                actor, admission, null, cancellation));
    }

    CompletionStage<PreparedSource> prepareDirectJoin(
        ZLinkActorJoinRelocationPort.Goal goal,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(goal, "goal");
        Objects.requireNonNull(cancellation, "cancellation");
        String actorId = goal.sourceActor().actorId();
        ZLinkActor actor = activeActor(actorId, cancellation);
        if (actor == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "Actor is not active locally: " + actorId));
        }
        return readOwned(actorId, cancellation, false)
            .thenCompose(owned -> listDescriptors(cancellation)
                .thenApply(descriptors -> new Admission(
                    validateDirectSourceAndTarget(owned, descriptors, goal),
                    directTarget(descriptors, goal),
                    sessionRoute(owned, descriptors))))
            .thenCompose(admission -> validateDirectTargetSpot(
                    goal, cancellation)
                .thenCompose(ignored -> sealAndCapture(
                    actor, admission, goal, cancellation)));
    }

    CompletionStage<Void> preflight(
        String actorId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(targetPolicy, "targetPolicy");
        Objects.requireNonNull(cancellation, "cancellation");
        ZLinkActor actor = activeActor(actorId, cancellation);
        if (actor == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "Actor is not active locally: " + actorId));
        }
        return admit(actorId, targetPolicy, cancellation)
            .thenApply(admission -> {
                if (admission.sessionRoute().isPresent()
                    && sessionSealer == null) {
                    throw new ZLinkUserSpotRetireRuntime
                        .RelocationBlockedException(
                            systems.zlink.framework.runtime.host
                                .ZLinkFrameworkRelocationReason
                                .STATE_INCOMPATIBLE,
                            "Bound-Session relocation requires command 42/43 "
                                + "seal support");
                }
                return null;
            });
    }

    private ZLinkActor activeActor(
        String actorId,
        ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested()) {
            return null;
        }
        return actors.localActor(actorId).orElse(null);
    }

    private CompletionStage<Admission> admit(
        String actorId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkStoreCancellation cancellation) {
        return readOwned(actorId, cancellation, true)
            .thenCompose(owned -> listDescriptors(cancellation)
                .thenApply(descriptors -> new Admission(
                    owned,
                    selectTarget(
                        owned,
                        descriptors,
                        targetPolicy),
                    sessionRoute(owned, descriptors))));
    }

    private CompletionStage<PreparedSource> sealAndCapture(
        ZLinkActor actor,
        Admission admission,
        ZLinkActorJoinRelocationPort.Goal directJoin,
        ZLinkStoreCancellation cancellation) {
        return sealAtTurnBoundary(
                admission.owned().actorId(),
                directJoin == null ? null : directJoin.activeTurnSeal(),
                cancellation)
            .thenCompose(sealed -> {
                if (sealed.isEmpty()) {
                    return failed(new IllegalStateException(
                        "Actor relocation queue cannot be sealed"));
                }
                ZLinkAsyncSerialQueue.RelocationSeal seal = sealed.orElseThrow();
                byte[] timerEnvelope =
                    relocationReplies.freezeActorTimerRelocationEnvelope(
                        admission.owned().actorId());
                ZLinkRelocationCancellation relocationCancellation =
                    cancellation::isCancellationRequested;
                CompletionStage<byte[]> captured =
                    admission.owned().snapshotPolicy()
                        ? adapters.captureActor(
                            admission.owned().stableType(),
                            actor,
                            relocationCancellation)
                        : CompletableFuture.completedFuture(new byte[0]);
                return captured.thenCompose(state -> {
                    byte[] applicationState = Objects.requireNonNull(
                        state, "Actor Capture returned null").clone();
                    UUID relocationId = directJoin == null
                        ? UUID.randomUUID()
                        : directJoin.relocationId();
                    String targetSpotId = directJoin == null
                        ? admission.target().entrySpotId().orElseThrow()
                        : directJoin.targetSpotId();
                    long targetSpotGeneration = directJoin == null
                        ? admission.target().lifecycleGeneration()
                        : directJoin.targetSpotGeneration();
                    int targetSpotKind = directJoin == null ? 1 : 2;
                    byte[] initialRoot =
                        ZLinkCanonicalActorRelocationEnvelope.encode(
                            relocationId,
                            admission.owned().actorId(),
                            admission.owned().snapshot().objectGeneration(),
                            admission.owned().snapshot()
                                .authorityOwnerGeneration(),
                            admission.owned().snapshotPolicy(),
                            applicationState,
                            seal.captured(),
                            timerEnvelope);
                    Optional<ZLinkSpotRetireControl.SessionRouteFence>
                        capturedRoute = admission.sessionRoute().or(() ->
                            capturedSessionRoute(
                                admission.owned(),
                                seal.captured()));
                    //  The captured payload lives only in source memory and
                    //  travels directly with the stage request; nothing is
                    //  written to a relocation store (spec 28 §4.2).
                    long advertisedReceiveChunkLimitBytes = directJoin == null
                        ? 0L
                        : directJoin.advertisedReceiveChunkLimitBytes();
                    return sealSessionRoute(
                            admission.owned(), relocationId, capturedRoute)
                        .thenApply(sessionRoute -> new PreparedSource(
                            locations,
                            actors,
                            relocationReplies,
                            sessionSealer,
                            sessionRoute,
                            seal,
                            admission.owned(),
                            admission.target(),
                            relocationId,
                            applicationState,
                            timerEnvelope,
                            stageRequest(
                                admission,
                                relocationId,
                                initialRoot,
                                sessionRoute.map(
                                    SealedSessionRoute::route),
                                targetSpotId,
                                advertisedReceiveChunkLimitBytes),
                            targetSpotId));
                }).exceptionallyCompose(failure -> {
                    relocationReplies.resumeActorTimersAfterRelocationAbort(
                        admission.owned().actorId());
                    actors.abortActorRelocation(
                        admission.owned().actorId(), seal);
                    return failed(unwrap(failure));
                });
            });
    }

    private CompletionStage<Optional<ZLinkAsyncSerialQueue.RelocationSeal>>
        sealAtTurnBoundary(
            String actorId,
            ZLinkAsyncSerialQueue.ActiveTurnSealHandle activeTurnSeal,
            ZLinkStoreCancellation cancellation) {
        ZLinkAsyncSerialQueue queue = actors.actorRelocationLane(actorId);
        if (activeTurnSeal != null) {
            //  A deferred Join holds this queue's active turn (its mailbox
            //  barrier). Reserving a lifecycle boundary here would queue it
            //  behind that turn and never reach it; seal the captured active
            //  turn directly instead (spec 28 §2 — the source finishes the
            //  current application turn and stops new dispatch; the barrier
            //  IS that current turn).
            return CompletableFuture.completedFuture(
                cancellation.isCancellationRequested()
                    ? Optional.empty()
                    : queue.trySealRelocation(activeTurnSeal));
        }
        Optional<ZLinkAsyncSerialQueue.RelocationBoundary> reserved =
            queue.reserveRelocationTurnBoundary();
        if (reserved.isEmpty()) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        ZLinkAsyncSerialQueue.RelocationBoundary boundary =
            reserved.orElseThrow();
        return boundary.reached().thenCompose(ignored -> {
            Optional<ZLinkAsyncSerialQueue.RelocationSeal> sealed =
                cancellation.isCancellationRequested()
                    ? Optional.empty()
                    : queue.trySealRelocation(boundary);
            boundary.release();
            return boundary.finished().thenApply(ignoredFinished -> sealed);
        });
    }

    private CompletionStage<Owned> readOwned(
        String actorId,
        ZLinkStoreCancellation cancellation,
        boolean requireEntryMembership) {
        String key = ZLinkAuthorityKeyCodec.actor(actorId);
        return locations.read(key, cancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                || snapshot.allocation().state()
                    != ZLinkPlacementAllocationState.ACTIVE
                || snapshot.allocation().objectKind()
                    != ZLinkPlacementObjectKind.ACTOR
                || !snapshot.allocation().descriptor().meshName()
                    .equals(meshName)
                || !snapshot.allocation().descriptor().rid()
                    .equals(localNodeRid)
                || snapshot.allocation().descriptorLifecycleGeneration()
                    != localNodeGeneration) {
                return failed(new IllegalStateException(
                    "Actor authority is not Ready on the source: " + key));
            }
            var authority = authorities.decode(snapshot.payload())
                .orElseThrow(() -> new IllegalStateException(
                    "Actor authority payload is invalid"));
            String stableType = actors.actorType(actorId);
            var factory = factories.get(stableType);
            if (factory == null) {
                return failed(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.NOT_CONFIGURED,
                    "Actor relocation factory is not registered: "
                        + stableType));
            }
            if (factory.relocationPolicy()
                    instanceof RelocationPolicy.Disabled) {
                //  Spec 15 failure table: the Actor's relocation policy
                //  forbids cross-node movement — Rejected, not a generic
                //  failure. This is the sole, exhaustive gate for this
                //  condition in this builder: both prepare() (via
                //  selectTarget/hasCapability) and prepareDirectJoin() (via
                //  directTarget/hasCapabilityForType) always run this check
                //  first through readOwned(), so neither candidacy filter
                //  can ever see a Disabled-policy Actor to dissolve this
                //  fact into a generic Unavailable.
                return failed(new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.REJECTED,
                    "Actor relocation policy forbids cross-node "
                        + "relocation: " + stableType));
            }
            if (authority.state() != ZLinkActorAuthorityPayloadCodec.State.READY
                || requireEntryMembership && authority.currentSpotKind() != 1
                || !authority.actorId().equals(actorId)
                || !authority.stableType().equals(stableType)
                || !authority.nodeRid().equals(localNodeRid)
                || authority.nodeGeneration() != localNodeGeneration
                || actors.actorRef(actorId).generation()
                    != snapshot.objectGeneration()) {
                return failed(new IllegalStateException(
                    "live Entry Spot Actor differs from Location authority"));
            }
            return CompletableFuture.completedFuture(new Owned(
                key,
                actorId,
                stableType,
                snapshot,
                authority,
                factory.relocationPolicy()
                    instanceof RelocationPolicy.PreserveState));
        });
    }

    private Owned validateDirectSourceAndTarget(
        Owned owned,
        List<ZLinkMeshNodeDescriptor> descriptors,
        ZLinkActorJoinRelocationPort.Goal goal) {
        if (!actors.actorRef(owned.actorId()).equals(goal.sourceActor())
            || !owned.stableType().equals(goal.actorType())) {
            throw new IllegalArgumentException(
                "direct Join source identity differs from the live Actor");
        }
        directTarget(descriptors, goal);
        return owned;
    }

    private ZLinkMeshNodeDescriptor directTarget(
        List<ZLinkMeshNodeDescriptor> descriptors,
        ZLinkActorJoinRelocationPort.Goal goal) {
        return descriptors.stream()
            .filter(value -> value.meshName().equals(meshName)
                && value.rid().equals(goal.targetNodeRid())
                && value.lifecycleGeneration()
                    == goal.targetNodeGeneration()
                && value.leaseGeneration()
                    == goal.targetOwnerLeaseGeneration()
                && value.state() == ZLinkFrameworkRuntimeState.SERVING
                && value.objectRole() == ZLinkMeshNodeObjectRole.SERVER
                && hasCapabilityForType(goal.actorType(), value)
                && hasCapacity(value))
            .findFirst()
            .orElseThrow(() -> new IllegalArgumentException(
                "direct Join target descriptor fence is stale"));
    }

    private CompletionStage<Void> validateDirectTargetSpot(
        ZLinkActorJoinRelocationPort.Goal goal,
        ZLinkStoreCancellation cancellation) {
        return locations.read(
                ZLinkAuthorityKeyCodec.spot(goal.targetSpotId()),
                cancellation)
            .thenApply(read -> {
                if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                    || snapshot.allocation().state()
                        != ZLinkPlacementAllocationState.ACTIVE
                    || snapshot.allocation().objectKind()
                        != ZLinkPlacementObjectKind.USER_SPOT
                    || !snapshot.allocation().descriptor().equals(
                        new ZLinkMeshNodeDescriptorKey(
                            meshName, goal.targetNodeRid()))
                    || snapshot.allocation()
                        .descriptorLifecycleGeneration()
                        != goal.targetNodeGeneration()
                    || snapshot.objectGeneration()
                        != goal.targetSpotGeneration()
                    || snapshot.authorityOwnerGeneration()
                        != goal.targetSpotAuthorityOwnerGeneration()
                    || snapshot.ownerLeaseGeneration()
                        != goal.targetOwnerLeaseGeneration()) {
                    throw new IllegalArgumentException(
                        "direct Join target Spot authority fence is stale");
                }
                return null;
            });
    }

    private boolean hasCapabilityForType(
        String stableType,
        ZLinkMeshNodeDescriptor candidate) {
        var factory = factories.get(stableType);
        if (factory == null
            || factory.relocationPolicy() instanceof RelocationPolicy.Disabled) {
            return false;
        }
        ZLinkObjectMaintenancePolicyKind policy = factory.relocationPolicy()
            instanceof RelocationPolicy.PreserveState
                ? ZLinkObjectMaintenancePolicyKind.SNAPSHOT
                : ZLinkObjectMaintenancePolicyKind.RECREATE;
        boolean snapshot = factory.relocationPolicy()
            instanceof RelocationPolicy.PreserveState;
        return candidate.objectCapabilities().stream().anyMatch(capability ->
            capability.objectKind() == ZLinkPlacementObjectKind.ACTOR
                && capability.stableType().equals(stableType)
                && capability.policy() == policy
                && capability.hasSnapshotAdapter() == snapshot);
    }

    private CompletionStage<List<ZLinkMeshNodeDescriptor>> listDescriptors(
        ZLinkStoreCancellation cancellation) {
        return listDescriptorPage(null, new ArrayList<>(), cancellation);
    }

    private CompletionStage<List<ZLinkMeshNodeDescriptor>> listDescriptorPage(
        String cursor,
        List<ZLinkMeshNodeDescriptor> result,
        ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested()) {
            return cancelled();
        }
        return locations.listMeshNodes(
                meshName,
                new ZLinkPageRequest(PAGE_SIZE, cursor))
            .thenCompose(page -> {
                result.addAll(page.items());
                if (result.size() > MAX_DESCRIPTORS) {
                    return failed(new IllegalStateException(
                        "MeshNode descriptor inventory exceeds its bound"));
                }
                String next = page.continuationToken();
                if (next == null || next.isBlank()) {
                    return CompletableFuture.completedFuture(
                        List.copyOf(result));
                }
                if (next.equals(cursor)) {
                    return failed(new IllegalStateException(
                        "MeshNode descriptor cursor did not advance"));
                }
                return listDescriptorPage(next, result, cancellation);
            });
    }

    private ZLinkMeshNodeDescriptor selectTarget(
        Owned actor,
        List<ZLinkMeshNodeDescriptor> descriptors,
        ZLinkRelocationTargetPolicy targetPolicy) {
        return ZLinkRelocationTargetSelector.select(
            descriptors,
            targetPolicy,
            this::baseEligible,
            candidate -> hasCapability(actor, candidate),
            this::hasCapacity,
            "No eligible Entry Spot Actor relocation target is Ready");
    }

    private boolean baseEligible(ZLinkMeshNodeDescriptor candidate) {
        if (!candidate.meshName().equals(meshName)
            || candidate.rid().equals(localNodeRid)
            || candidate.state() != ZLinkFrameworkRuntimeState.SERVING
            || candidate.objectRole() != ZLinkMeshNodeObjectRole.SERVER
            || candidate.entrySpotId().isEmpty()) {
            return false;
        }
        return true;
    }

    private boolean hasCapability(
        Owned actor,
        ZLinkMeshNodeDescriptor candidate) {
        var factory = factories.get(actor.stableType());
        ZLinkObjectMaintenancePolicyKind policy =
            factory.relocationPolicy()
                instanceof RelocationPolicy.PreserveState
                ? ZLinkObjectMaintenancePolicyKind.SNAPSHOT
                : ZLinkObjectMaintenancePolicyKind.RECREATE;
        return candidate.objectCapabilities().stream().anyMatch(capability ->
            capability.objectKind() == ZLinkPlacementObjectKind.ACTOR
                && capability.stableType().equals(actor.stableType())
                && capability.policy() == policy
                && capability.hasSnapshotAdapter()
                    == actor.snapshotPolicy());
    }

    private boolean hasCapacity(ZLinkMeshNodeDescriptor candidate) {
        return hasCapacity(candidate.capacity().actors(), 1)
            && (candidate.activationConcurrency().limit() == 0
                || candidate.activationConcurrency().active()
                    < candidate.activationConcurrency().limit());
    }

    private ZLinkSpotRetireControl.StageRequest stageRequest(
        Admission admission,
        UUID relocationId,
        byte[] relocationPayload,
        Optional<ZLinkSpotRetireControl.SessionRouteFence> sessionRoute,
        String targetSpotId,
        long advertisedReceiveChunkLimitBytes) {
        Owned actor = admission.owned();
        ZLinkMeshNodeDescriptor target = admission.target();
        return new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(relocationId, 1),
            localNodeRid,
            localNodeGeneration,
            actor.snapshot().ownerId(),
            actor.snapshot().ownerLeaseGeneration(),
            target.rid(),
            target.lifecycleGeneration(),
            target.ownerId(),
            target.leaseGeneration(),
            meshName,
            targetSpotId,
            actor.stableType(),
            false,
            actor.snapshotPolicy(),
            relocationPayload,
            List.of(new ZLinkSpotRetireControl.ParticipantFence(
                actor.authorityKey(),
                1,
                actor.actorId(),
                actor.stableType(),
                actor.snapshotPolicy(),
                actor.snapshot().objectGeneration(),
                actor.snapshot().authorityOwnerGeneration())),
            sessionRoute.stream().toList(),
            advertisedReceiveChunkLimitBytes);
    }

    /**
     * Spec 20 §5 step 1: once the Actor lane is sealed the relocation source
     * asks each bound Session owner to seal its ingress boundary (command 42)
     * and answer with the accepted high-water it recorded there (command 43).
     * That number replaces the source's own captured sequence in the Session
     * route fence, so the value the target replays in command 44 is the one
     * the owner itself reported and the owner's step 7 check is an equality.
     *
     * Command 43 must echo command 42 exactly. Session message ordering stays
     * owned by the Session runtime and is not copied into relocation state.
     */
    private CompletionStage<Optional<SealedSessionRoute>> sealSessionRoute(
            Owned actor,
            UUID relocationId,
            Optional<ZLinkSpotRetireControl.SessionRouteFence> captured) {
        if (captured.isEmpty()) {
            return CompletableFuture.completedFuture(Optional.empty());
        }
        if (sessionSealer == null) {
            return CompletableFuture.failedFuture(
                new ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                    systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE,
                    "Bound-Session relocation peer does not support the "
                        + "command 42/43 seal barrier"));
        }
        ZLinkSpotRetireControl.SessionRouteFence route = captured.orElseThrow();
        var seal = new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
                new ZLinkServiceM6BWireCodec.RelocationIdentity(
                        relocationId.getMostSignificantBits(),
                        relocationId.getLeastSignificantBits()),
                new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                        actor.snapshot().ownerId(),
                        actor.snapshot().ownerLeaseGeneration(),
                        localNodeRid,
                        localNodeGeneration,
                        route.sourceAuthorityStoreVersion()),
                ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                        new systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef(
                                localNodeRid,
                                route.actorId(),
                                route.actorObjectGeneration()),
                        localNodeGeneration,
                        route.sourceAuthorityOwnerGeneration(),
                        actor.snapshot().ownerLeaseGeneration()),
                new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                        route.sessionOwnerNodeRid(),
                        route.sessionOwnerNodeGeneration(),
                        route.sessionOwnerId(),
                        route.sessionOwnerLeaseGeneration(),
                        route.sessionRid(),
                        route.bindingGeneration()));
        //  The command 43 ACK completes on the mesh transport dispatch thread;
        //  staging the relocation root must not run there.
        return sessionSealer.sealRouteUntilAck(
                seal, sessionRelocationSealTimeout)
            .<Optional<SealedSessionRoute>>thenApplyAsync(
                sealed -> {
                    return Optional.of(new SealedSessionRoute(
                        route,
                        seal,
                        new ZLinkServiceM6BWireCodec()
                            .encodeSessionRelocationSealed(sealed)));
                })
            .exceptionallyCompose(failure -> CompletableFuture.failedFuture(
                new ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                    systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRelocationReason.RELOCATION_FAILED,
                    "Bound-Session relocation seal did not complete")));
    }

    private static CompletionStage<Void> abortSessionRoute(
        ZLinkSessionRelocationPeerClient sessionSealer,
        Optional<SealedSessionRoute> context) {
        if (context.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        SealedSessionRoute sealed = context.orElseThrow();
        ZLinkServiceM6BWireCodec.SessionRelocationSeal command42 =
            sealed.seal();
        ZLinkServiceM6BWireCodec.SessionRelocationRoute abort =
            new ZLinkServiceM6BWireCodec.SessionRelocationRoute(
                command42.relocation(),
                command42.coordinator(),
                ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
                new ZLinkServiceM6BWireCodec.ActorIdentity(
                    command42.actor().actor().actorId(),
                    command42.actor().actor().generation()),
                command42.session(),
                ZLinkServiceM6BWireCodec.SessionRelocationRouteAction.ABORT,
                0,
                command42.actor().authorityOwnerGeneration(),
                null,
                0);
        return sessionSealer.sendRoute(abort);
    }

    private Optional<ZLinkSpotRetireControl.SessionRouteFence>
        capturedSessionRoute(
            Owned actor,
            List<ZLinkAsyncSerialQueue.QueuedRecord> captured) {
        for (ZLinkAsyncSerialQueue.QueuedRecord queued : captured) {
            ZLinkActorAcceptedJournal.Record record;
            try {
                record = ZLinkActorAcceptedJournal.decode(queued.payload());
            } catch (IllegalArgumentException notActorRecord) {
                continue;
            }
            if (!record.actorId().equals(actor.actorId())
                || record.sourceSessionRid() == null) {
                continue;
            }
            return Optional.of(
                new ZLinkSpotRetireControl.SessionRouteFence(
                    actor.actorId(),
                    actor.snapshot().objectGeneration(),
                    actor.snapshot().authorityOwnerGeneration(),
                    actor.snapshot().storeVersion(),
                    record.sourceNodeRid(),
                    record.sourceNodeGeneration(),
                    record.sourceOwnerId(),
                    record.sourceOwnerLeaseGeneration(),
                    record.sourceSessionRid(),
                    record.sourceBindingGeneration()));
        }
        return Optional.empty();
    }

    private Optional<ZLinkSpotRetireControl.SessionRouteFence> sessionRoute(
        Owned actor,
        List<ZLinkMeshNodeDescriptor> descriptors) {
        var knownRoute = actors.boundSessionRoute(actor.actorId());
        return knownRoute.map(route -> {
            ZLinkMeshNodeDescriptor owner = descriptors.stream()
                .filter(value -> value.rid().equals(
                    route.sessionOwnerNodeRid()))
                .findFirst()
                .orElseThrow(() -> new IllegalStateException(
                    "bound Session owner descriptor is unavailable: "
                        + route.sessionOwnerNodeRid()));
            return new ZLinkSpotRetireControl.SessionRouteFence(
                actor.actorId(),
                actor.snapshot().objectGeneration(),
                actor.snapshot().authorityOwnerGeneration(),
                actor.snapshot().storeVersion(),
                route.sessionOwnerNodeRid(),
                owner.lifecycleGeneration(),
                owner.ownerId(),
                owner.leaseGeneration(),
                route.sessionRid(),
                route.bindingGeneration());
        });
    }

    private static boolean hasCapacity(
        ZLinkCapacityUsage usage,
        int required) {
        return usage.limit() == 0
            || (long) usage.active() + usage.reserved() + required
                <= usage.limit();
    }

    static final class PreparedSource {
        private final ZLinkLocationRepository locations;
        private final ZLinkActorSessionCoordinator actors;
        private final ZLinkSpotRuntime relocationReplies;
        private final ZLinkSessionRelocationPeerClient sessionSealer;
        private final Optional<SealedSessionRoute> sealedSessionRoute;
        private final ZLinkAsyncSerialQueue.RelocationSeal seal;
        private final Owned owned;
        private final ZLinkMeshNodeDescriptor target;
        private final UUID relocationId;
        private final byte[] state;
        private final byte[] timerEnvelope;
        private final ZLinkSpotRetireControl.StageRequest stageRequest;
        private final String targetSpotId;
        private List<ZLinkAsyncSerialQueue.QueuedRecord> finalJournal =
            List.of();
        private systems.zlink.framework.runtime.internal.relocation
            .ZLinkRetainedSerialQueueCommit.Commit relocationCommit;
        private boolean captureFinished;
        private boolean committed;
        private boolean terminal;

        private PreparedSource(
            ZLinkLocationRepository locations,
            ZLinkActorSessionCoordinator actors,
            ZLinkSpotRuntime relocationReplies,
            ZLinkSessionRelocationPeerClient sessionSealer,
            Optional<SealedSessionRoute> sealedSessionRoute,
            ZLinkAsyncSerialQueue.RelocationSeal seal,
            Owned owned,
            ZLinkMeshNodeDescriptor target,
            UUID relocationId,
            byte[] state,
            byte[] timerEnvelope,
            ZLinkSpotRetireControl.StageRequest stageRequest,
            String targetSpotId) {
            this.locations = locations;
            this.actors = actors;
            this.relocationReplies = relocationReplies;
            this.sessionSealer = sessionSealer;
            this.sealedSessionRoute = Objects.requireNonNull(
                sealedSessionRoute, "sealedSessionRoute");
            this.seal = seal;
            this.owned = owned;
            this.target = target;
            this.relocationId = relocationId;
            this.state = state.clone();
            this.timerEnvelope = timerEnvelope.clone();
            this.stageRequest = stageRequest;
            this.targetSpotId = targetSpotId;
        }

        ZLinkStandaloneActorRelocationStagingOwner.Request targetRequest() {
            return new ZLinkStandaloneActorRelocationStagingOwner.Request(
                relocationId,
                owned.actorId(),
                owned.stableType(),
                owned.snapshot().objectGeneration(),
                owned.snapshot().authorityOwnerGeneration(),
                owned.snapshotPolicy(),
                targetSpotId);
        }

        ZLinkSpotRetireControl.StageRequest stageRequest() {
            return stageRequest;
        }

        ZLinkActor actor() {
            return actors.localActor(owned.actorId()).orElse(null);
        }

        String sourceSpotId() {
            return owned.authority().currentSpotId();
        }

        long sourceSpotGeneration() {
            return owned.authority().currentSpotGeneration();
        }

        long expectedTargetAuthorityOwnerGeneration() {
            return Math.addExact(
                owned.snapshot().authorityOwnerGeneration(), 1L);
        }

        CompletionStage<Boolean> authorityMovedToTarget() {
            return locations.read(owned.authorityKey(), () -> false)
                .thenApply(read -> {
                    if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                        || snapshot.authorityOwnerGeneration()
                            != expectedTargetAuthorityOwnerGeneration()
                        || !snapshot.allocation().descriptor().equals(
                            new ZLinkMeshNodeDescriptorKey(
                                target.meshName(), target.rid()))
                        || snapshot.allocation()
                            .descriptorLifecycleGeneration()
                            != target.lifecycleGeneration()) {
                        return false;
                    }
                    return new ZLinkActorAuthorityPayloadCodec()
                        .decode(snapshot.payload())
                        .filter(value -> value.nodeRid().equals(target.rid())
                            && value.nodeGeneration()
                                == target.lifecycleGeneration()
                            && value.currentSpotId().equals(targetSpotId))
                        .isPresent();
                });
        }

        CompletionStage<Void> relayCapturedIngress(
            ZLinkRelocationTransitionClient client,
            Duration timeout) {
            Objects.requireNonNull(client, "client");
            Objects.requireNonNull(timeout, "timeout");
            systems.zlink.framework.runtime.internal.relocation
                .ZLinkRetainedSerialQueueCommit.Commit retained;
            synchronized (this) {
                if (terminal || committed) {
                    return failed(new IllegalStateException(
                        "Actor relocation relay boundary is terminal"));
                }
                if (relocationCommit == null) {
                    relocationCommit = actors.retainActorRelocationCommit(
                            owned.actorId(), seal)
                        .orElseThrow(() -> new IllegalStateException(
                            "Actor relocation source queue was lost"));
                    installExpectedRelocationForward();
                }
                retained = relocationCommit;
            }
            systems.zlink.framework.runtime.internal.relocation
                .ZLinkRetainedSerialQueueCommit.Cut cut;
            do {
                cut = retained.cut();
            } while (!retained.tryEstablishAndFinishCapture(cut));
            List<ZLinkAsyncSerialQueue.QueuedRecord> relayed =
                cut.records().stream()
                    .sorted((left, right) -> Long.compareUnsigned(
                        left.sequence(), right.sequence()))
                    .toList();
            synchronized (this) {
                finalJournal = relayed;
                captureFinished = true;
            }
            CompletionStage<Void> chain =
                CompletableFuture.completedFuture(null);
            if (sealedSessionRoute.isPresent()) {
                byte[] sealed = sealedSessionRoute.orElseThrow().sealedAck();
                chain = chain.thenCompose(ignored -> client.relay(
                    stageRequest.targetNodeRid(),
                    stageRequest.fence(),
                    sealed,
                    timeout));
            }
            for (ZLinkAsyncSerialQueue.QueuedRecord record : relayed) {
                chain = chain.thenCompose(ignored -> client.relay(
                    stageRequest.targetNodeRid(),
                    stageRequest.fence(),
                    record.payload(),
                    timeout));
            }
            return chain;
        }

        private void installExpectedRelocationForward() {
            long targetOwnerGeneration = Math.addExact(
                owned.snapshot().authorityOwnerGeneration(), 1);
            installRelocationForward(targetOwnerGeneration);
            bindCommittedReplies(Map.of(
                owned.authorityKey(), targetOwnerGeneration));
        }

        private void installRelocationForward(
            long targetAuthorityGeneration) {
            relocationReplies.nodeByRid(stageRequest.sourceNodeRid())
                .installRelocationActorForward(
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    new systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendActorRef(
                            stageRequest.sourceNodeRid(),
                            owned.actorId(),
                            owned.snapshot().objectGeneration()),
                    stageRequest.sourceNodeGeneration(),
                    owned.snapshot().authorityOwnerGeneration(),
                    owned.snapshot().ownerLeaseGeneration()),
                new ZLinkServiceM6BWireCodec.ActorRouteFence(
                    new systems.zlink.framework.runtime.internal.backend
                        .ZLinkBackendActorRef(
                            stageRequest.targetNodeRid(),
                            owned.actorId(),
                            owned.snapshot().objectGeneration()),
                    stageRequest.targetNodeGeneration(),
                    targetAuthorityGeneration,
                    stageRequest.targetOwnerLeaseGeneration()),
                relocationReplies.relocationForwardRetention());
        }

        private void bindCommittedReplies(
            Map<String, Long> targetOwnerGenerations) {
            ZLinkSpotRetireControl.ParticipantFence participant =
                stageRequest.participants().stream()
                    .filter(value -> value.objectId().equals(owned.actorId()))
                    .findFirst()
                    .orElseThrow(() -> new IllegalStateException(
                        "Actor relocation participant is missing"));
            relocationReplies.bindCanonicalRelocationReplies(
                Map.of("actor:" + owned.actorId(), finalJournal),
                stageRequest.targetNodeRid(),
                stageRequest.targetNodeGeneration(),
                Map.of(
                    owned.actorId(),
                    new ZLinkSpotRelocationReplyRoutes.CommittedFence(
                        participant.authorityKey(),
                        1L,
                        targetOwnerGeneration(
                            targetOwnerGenerations,
                            participant.authorityKey()))));
        }

        synchronized void completeSourceQueueCommit() {
            if (relocationCommit == null
                || !committed && !captureFinished) {
                throw new IllegalStateException(
                    "Actor relocation source queue is not durably committed");
            }
            committed = true;
            relocationCommit.complete();
        }

        synchronized boolean relayBoundaryCommitted() {
            return committed;
        }

        CompletionStage<Void> discardInitialAfterCommit() {
            synchronized (this) {
                if (!committed || terminal) {
                    return failed(new IllegalStateException(
                        "Actor relocation source is not committed"));
                }
            }
            finish();
            return CompletableFuture.completedFuture(null);
        }

        CompletionStage<Void> cleanupLocal() {
            if (!committed || terminal) {
                return failed(new IllegalStateException(
                    "Actor relocation source is not committed"));
            }
            relocationReplies.closeActorTimersAfterRelocation(
                owned.actorId());
            return actors.completeRelocationSource(
                List.of(owned.actorId()));
        }

        synchronized CompletionStage<Void> abort() {
            if (terminal || committed) {
                return failed(new IllegalStateException(
                    "committed Actor relocation cannot be aborted"));
            }
            return abortSessionRoute(sessionSealer, sealedSessionRoute)
                .thenRun(() -> {
                    boolean restored = relocationCommit == null
                        ? actors.abortActorRelocation(owned.actorId(), seal)
                        : relocationCommit.abort();
                    if (!restored) {
                        throw new IllegalStateException(
                            "Actor relocation source queue was lost");
                    }
                    relocationReplies.resumeActorTimersAfterRelocationAbort(
                        owned.actorId());
                    finish();
                });
        }

        private synchronized void finish() {
            if (!terminal) {
                terminal = true;
            }
        }
    }

    private record Admission(
        Owned owned,
        ZLinkMeshNodeDescriptor target,
        Optional<ZLinkSpotRetireControl.SessionRouteFence> sessionRoute) {
        private Admission {
            sessionRoute = Objects.requireNonNull(
                sessionRoute, "sessionRoute");
        }
    }

    private record SealedSessionRoute(
        ZLinkSpotRetireControl.SessionRouteFence route,
        ZLinkServiceM6BWireCodec.SessionRelocationSeal seal,
        byte[] sealedAck) {
        private SealedSessionRoute {
            Objects.requireNonNull(route, "route");
            Objects.requireNonNull(seal, "seal");
            sealedAck = Objects.requireNonNull(
                sealedAck, "sealedAck").clone();
        }

        @Override public byte[] sealedAck() {
            return sealedAck.clone();
        }
    }

    private record Owned(
        String authorityKey,
        String actorId,
        String stableType,
        ZLinkAuthoritySnapshot snapshot,
        ZLinkActorAuthorityPayloadCodec.ActorAuthority authority,
        boolean snapshotPolicy) {
    }

    private static String requireText(String value, String name) {
        if (value == null || value.isBlank() || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(name + " is invalid");
        }
        return value;
    }

    private static Throwable unwrap(Throwable failure) {
        Throwable current = failure;
        while (current instanceof CompletionException
            && current.getCause() != null) {
            current = current.getCause();
        }
        return current;
    }

    private static <T> CompletionStage<T> cancelled() {
        return failed(new CancellationException());
    }

    private static <T> CompletionStage<T> failed(Throwable failure) {
        return CompletableFuture.failedFuture(failure);
    }

    private static long targetOwnerGeneration(
        Map<String, Long> generations,
        String authorityKey) {
        Long generation = generations.get(authorityKey);
        if (generation == null || generation <= 0) {
            throw new IllegalArgumentException(
                "committed owner generation is absent: " + authorityKey);
        }
        return generation;
    }
}
