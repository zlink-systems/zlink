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
import java.util.function.Predicate;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
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
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationPermitPool;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
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
    private static final long SNAPSHOT_RESERVATION_BYTES = 64L * 1024 * 1024;
    private static final long ENVELOPE_RESERVATION_BYTES = 64L * 1024;

    private final String meshName;
    private final RoutingId localNodeRid;
    private final long localNodeGeneration;
    private final ZLinkLocationRepository locations;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkRelocationPermitPool permits;
    private final ZLinkActorSessionCoordinator actors;
    private final ZLinkRelocationAdapterRegistry adapters;
    private final Map<String, RelocatableActorFactory<?>>
        factories;
    private final ZLinkSpotRuntime relocationReplies;
    //  Command 42 sender. A bound Session cannot relocate when the peer does
    //  not support the exact seal/high-water barrier.
    private final ZLinkSessionRelocationPeerClient sessionSealer;
    //  Command 42 loss policy is `retransmit-until-sealed-or-deadline`
    //  (service-wire-v1.schema.json relocationStateMachine.commandRules).
    private static final Duration SESSION_SEAL_DEADLINE =
        Duration.ofSeconds(5);
    private final ZLinkActorAuthorityPayloadCodec authorities =
        new ZLinkActorAuthorityPayloadCodec();

    ZLinkStandaloneActorRelocationSourceBuilder(
        String meshName,
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkRelocationPermitPool permits,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        Map<String, RelocatableActorFactory<?>>
            factories,
        ZLinkSpotRuntime relocationReplies,
        ZLinkSessionRelocationPeerClient sessionSealer) {
        this.sessionSealer = sessionSealer;
        this.meshName = requireText(meshName, "meshName");
        this.localNodeRid = Objects.requireNonNull(
            localNodeRid, "localNodeRid");
        if (localNodeGeneration <= 0) {
            throw new IllegalArgumentException(
                "local node generation must be positive");
        }
        this.localNodeGeneration = localNodeGeneration;
        this.locations = Objects.requireNonNull(locations, "locations");
        this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
        this.permits = Objects.requireNonNull(permits, "permits");
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
                actor, admission, cancellation));
    }

    CompletionStage<Void> preflight(
        String actorId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(targetPolicy, "targetPolicy");
        Objects.requireNonNull(capacityPlan, "capacityPlan");
        Objects.requireNonNull(cancellation, "cancellation");
        ZLinkActor actor = activeActor(actorId, cancellation);
        if (actor == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "Actor is not active locally: " + actorId));
        }
        return admit(
            actorId,
            targetPolicy,
            capacityPlan,
            cancellation).thenApply(admission -> {
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
                capacityPlan.reserveActor(actorId, admission.target());
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
        return admit(actorId, targetPolicy, null, cancellation);
    }

    CompletionStage<PreparedSource> preparePinned(
        String actorId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkMeshNodeDescriptor pinnedTarget,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(pinnedTarget, "pinnedTarget");
        ZLinkActor actor = activeActor(actorId, cancellation);
        if (actor == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "Actor is not active locally: " + actorId));
        }
        return readOwned(actorId, cancellation)
            .thenCompose(owned -> listDescriptors(cancellation)
                .thenApply(descriptors -> new Admission(
                    owned,
                    requirePinnedTarget(
                        pinnedTarget,
                        descriptors,
                        candidate -> hasCapability(owned, candidate)),
                    sessionRoute(owned, descriptors))))
            .thenCompose(admission -> sealAndCapture(
                actor, admission, cancellation));
    }

    private ZLinkMeshNodeDescriptor requirePinnedTarget(
        ZLinkMeshNodeDescriptor pinned,
        List<ZLinkMeshNodeDescriptor> descriptors,
        Predicate<ZLinkMeshNodeDescriptor> capability) {
        return descriptors.stream()
            .filter(candidate -> candidate.rid().equals(pinned.rid()))
            .filter(candidate -> candidate.lifecycleGeneration()
                == pinned.lifecycleGeneration())
            .filter(this::baseEligible)
            .filter(capability)
            .findFirst()
            .orElseThrow(() -> new ZLinkUserSpotRetireRuntime
                .RelocationBlockedException(
                    systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE,
                    "Pinned Actor relocation target is no longer Ready"));
    }

    private CompletionStage<Admission> admit(
        String actorId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan,
        ZLinkStoreCancellation cancellation) {
        return readOwned(actorId, cancellation)
            .thenCompose(owned -> listDescriptors(cancellation)
                .thenApply(descriptors -> new Admission(
                    owned,
                    selectTarget(
                        owned,
                        descriptors,
                        targetPolicy,
                        capacityPlan),
                    sessionRoute(owned, descriptors))));
    }

    private CompletionStage<PreparedSource> sealAndCapture(
        ZLinkActor actor,
        Admission admission,
        ZLinkStoreCancellation cancellation) {
        return permits.acquire(
                ZLinkRelocationPermitPool.Request.outboundAggregate(
                admission.owned().snapshotPolicy()
                    ? SNAPSHOT_RESERVATION_BYTES
                    : ENVELOPE_RESERVATION_BYTES,
                admission.owned().snapshotPolicy() ? 1 : 0,
                    false),
                cancellation)
            .thenCompose(permit -> sealAndCaptureWithPermit(
                actor, admission, cancellation, permit));
    }

    private CompletionStage<PreparedSource> sealAndCaptureWithPermit(
        ZLinkActor actor,
        Admission admission,
        ZLinkStoreCancellation cancellation,
        ZLinkRelocationPermitPool.Lease permit) {
        return sealAtTurnBoundary(
                admission.owned().actorId(), cancellation)
            .thenCompose(sealed -> {
                if (sealed.isEmpty()) {
                    permit.close();
                    return failed(new IllegalStateException(
                        "Actor relocation queue cannot be sealed"));
                }
                ZLinkAsyncSerialQueue.RelocationSeal seal = sealed.orElseThrow();
                byte[] timerEnvelope =
                    relocationReplies.freezeActorTimerRelocationEnvelope(
                        admission.owned().actorId());
                ZLinkRelocationCancellation relocationCancellation =
                    cancellation::isCancellationRequested;
                CompletionStage<byte[]> captured = admission.owned().snapshotPolicy()
                    ? adapters.captureActor(
                        admission.owned().stableType(),
                        actor,
                        relocationCancellation)
                    : CompletableFuture.completedFuture(new byte[0]);
                return captured.thenCompose(state -> {
                    byte[] applicationState = Objects.requireNonNull(
                        state, "Actor Capture returned null").clone();
                    UUID relocationId = UUID.randomUUID();
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
                    var request = request(
                        admission.owned(),
                        admission.target(),
                        relocationId,
                        initialRoot);
                    Optional<ZLinkSpotRetireControl.SessionRouteFence>
                        capturedRoute = admission.sessionRoute().or(() ->
                            capturedSessionRoute(
                                admission.owned(),
                                seal.captured()));
                    return sealSessionRoute(
                            admission.owned(), relocationId, capturedRoute)
                        .thenCompose(sessionRoute ->
                            coordinator.stageRoot(request, cancellation)
                                .thenApply(staged -> new PreparedSource(
                                    coordinator,
                                    actors,
                                    relocationReplies,
                                    sessionSealer,
                                    sessionRoute,
                                    seal,
                                    permit,
                                    admission.owned(),
                                    admission.target(),
                                    relocationId,
                                    applicationState,
                                    timerEnvelope,
                                    staged,
                                    stageRequest(
                                        admission,
                                        relocationId,
                                        staged,
                                        sessionRoute.map(
                                            SealedSessionRoute::route))))
                                .exceptionallyCompose(failure ->
                                    abortSessionRoute(sessionSealer, sessionRoute)
                                        .handle((ignored, abortFailure) -> {
                                            Throwable cause = unwrap(failure);
                                            if (abortFailure != null) {
                                                cause.addSuppressed(
                                                    unwrap(abortFailure));
                                            }
                                            throw new CompletionException(cause);
                                        })));
                }).exceptionallyCompose(failure -> {
                    relocationReplies.resumeActorTimersAfterRelocationAbort(
                        admission.owned().actorId());
                    actors.abortActorRelocation(
                        admission.owned().actorId(), seal);
                    permit.close();
                    return failed(unwrap(failure));
                });
            });
    }

    private CompletionStage<Optional<ZLinkAsyncSerialQueue.RelocationSeal>>
        sealAtTurnBoundary(
            String actorId,
            ZLinkStoreCancellation cancellation) {
        ZLinkAsyncSerialQueue queue = actors.actorRelocationLane(actorId);
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
        ZLinkStoreCancellation cancellation) {
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
            if (factory == null
                || factory.relocationPolicy()
                    instanceof RelocationPolicy.Disabled) {
                return failed(new IllegalStateException(
                    "Actor relocation policy is unavailable: " + stableType));
            }
            if (authority.state() != ZLinkActorAuthorityPayloadCodec.State.READY
                || authority.currentSpotKind() != 1
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
                factory.relocationPolicy()
                    instanceof RelocationPolicy.PreserveState));
        });
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
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan) {
        return ZLinkRelocationTargetSelector.select(
            descriptors,
            targetPolicy,
            this::baseEligible,
            candidate -> hasCapability(actor, candidate),
            candidate -> hasCapacity(candidate)
                && (capacityPlan == null
                    || capacityPlan.canReserveActor(candidate)),
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

    private ZLinkAggregateRelocationCoordinator.Request request(
        Owned actor,
        ZLinkMeshNodeDescriptor target,
        UUID relocationId,
        byte[] root) {
        String targetSpotId = target.entrySpotId().orElseThrow();
        byte[] targetAuthority = authorities.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            actor.stableType(),
            actor.actorId(),
            targetSpotId,
            target.lifecycleGeneration(),
            1,
            target.ownerId(),
            target.leaseGeneration(),
            meshName,
            target.rid(),
            target.lifecycleGeneration());
        var participant = new ZLinkAggregateRelocationCoordinator.Participant(
            actor.authorityKey(),
            ZLinkPlacementObjectKind.ACTOR,
            actor.snapshot().objectGeneration(),
            actor.snapshot().authorityOwnerGeneration(),
            actor.snapshot().storeVersion(),
            ZLinkAuthorityGenerationTransition.NEW_OWNER,
            targetAuthority,
            new byte[0]);
        return new ZLinkAggregateRelocationCoordinator.Request(
            relocationId,
            1,
            List.of(participant),
            root,
            new ZLinkMeshNodeDescriptorKey(meshName, target.rid()),
            target.lifecycleGeneration(),
            ZLinkPlacementCapacityBundle.actor(1),
            new ZLinkLocationOwnerToken(
                target.ownerId(), target.leaseGeneration()));
    }

    private ZLinkSpotRetireControl.StageRequest stageRequest(
        Admission admission,
        UUID relocationId,
        ZLinkAggregateRelocationCoordinator.StagedRoot staged,
        Optional<ZLinkSpotRetireControl.SessionRouteFence> sessionRoute) {
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
            target.entrySpotId().orElseThrow(),
            actor.stableType(),
            false,
            actor.snapshotPolicy(),
            staged.stored().reference(),
            staged.stored().checksumCrc32c(),
            List.of(new ZLinkSpotRetireControl.ParticipantFence(
                actor.authorityKey(),
                1,
                actor.actorId(),
                actor.stableType(),
                actor.snapshotPolicy(),
                actor.snapshot().objectGeneration(),
                actor.snapshot().authorityOwnerGeneration())),
            sessionRoute.stream().toList());
    }

    /**
     * Spec 20 §5 step 1: once the Actor lane is sealed the relocation source
     * asks each bound Session owner to seal its ingress boundary (command 42)
     * and answer with the accepted high-water it recorded there (command 43).
     * That number replaces the source's own captured sequence in the Session
     * route fence, so the value the target replays in command 44 is the one
     * the owner itself reported and the owner's step 7 check is an equality.
     *
     * <p>Ported from the C++ source-side seal
     * (`runtime/stateful/public_host_runtime.cpp:1430`
     * `seal_session_remote`, which journals
     * `sealed.last_accepted_session_sequence` into the durable relocation
     * record). A missing or failed seal has no durable high-water evidence and
     * therefore fails the relocation instead of guessing from the source
     * journal.</p>
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
        return sessionSealer.sealRouteUntilAck(seal, SESSION_SEAL_DEADLINE)
            .<Optional<SealedSessionRoute>>thenApplyAsync(
                sealed -> {
                    ZLinkSpotRetireControl.SessionRouteFence sealedRoute =
                        new ZLinkSpotRetireControl.SessionRouteFence(
                        route.actorId(),
                        route.actorObjectGeneration(),
                        route.sourceAuthorityOwnerGeneration(),
                        route.sourceAuthorityStoreVersion(),
                        route.sessionOwnerNodeRid(),
                        route.sessionOwnerNodeGeneration(),
                        route.sessionOwnerId(),
                        route.sessionOwnerLeaseGeneration(),
                        route.sessionRid(),
                        route.bindingGeneration(),
                        sealed.lastAcceptedSessionSequence());
                    return Optional.of(new SealedSessionRoute(
                        sealedRoute,
                        seal,
                        sealed.lastAcceptedSessionSequence()));
                })
            .exceptionallyCompose(failure -> CompletableFuture.failedFuture(
                new ZLinkUserSpotRetireRuntime.RelocationBlockedException(
                    systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRelocationReason.RELOCATION_FAILED,
                    "Bound-Session relocation seal did not produce durable "
                        + "high-water evidence")));
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
                0,
                0);
        return sessionSealer.abortRouteUntilAck(
                abort, sealed.lastAcceptedSessionSequence(),
                SESSION_SEAL_DEADLINE)
            .thenApply(ignored -> null);
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
                    record.sourceBindingGeneration(),
                    record.sourceSessionSequence()));
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
                route.bindingGeneration(),
                route.lastAcceptedSessionSequence());
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
        private final ZLinkAggregateRelocationCoordinator coordinator;
        private final ZLinkActorSessionCoordinator actors;
        private final ZLinkSpotRuntime relocationReplies;
        private final ZLinkSessionRelocationPeerClient sessionSealer;
        private final Optional<SealedSessionRoute> sealedSessionRoute;
        private final ZLinkAsyncSerialQueue.RelocationSeal seal;
        private final ZLinkRelocationPermitPool.Lease permit;
        private final Owned owned;
        private final ZLinkMeshNodeDescriptor target;
        private final UUID relocationId;
        private final byte[] state;
        private final byte[] timerEnvelope;
        private final ZLinkAggregateRelocationCoordinator.StagedRoot initial;
        private final ZLinkSpotRetireControl.StageRequest stageRequest;
        private ZLinkAggregateRelocationCoordinator.Prepared prepared;
        private List<ZLinkAsyncSerialQueue.QueuedRecord> finalJournal =
            List.of();
        private systems.zlink.framework.runtime.internal.relocation
            .ZLinkRetainedSerialQueueCommit.Commit relocationCommit;
        private byte[] committedRoot;
        private ZLinkAggregateRelocationCoordinator.Published
            activatedPublication;
        private CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published> sourceCommitFlight;
        private boolean captureFinished;
        private boolean committed;
        private boolean terminal;

        private PreparedSource(
            ZLinkAggregateRelocationCoordinator coordinator,
            ZLinkActorSessionCoordinator actors,
            ZLinkSpotRuntime relocationReplies,
            ZLinkSessionRelocationPeerClient sessionSealer,
            Optional<SealedSessionRoute> sealedSessionRoute,
            ZLinkAsyncSerialQueue.RelocationSeal seal,
            ZLinkRelocationPermitPool.Lease permit,
            Owned owned,
            ZLinkMeshNodeDescriptor target,
            UUID relocationId,
            byte[] state,
            byte[] timerEnvelope,
            ZLinkAggregateRelocationCoordinator.StagedRoot initial,
            ZLinkSpotRetireControl.StageRequest stageRequest) {
            this.coordinator = coordinator;
            this.actors = actors;
            this.relocationReplies = relocationReplies;
            this.sessionSealer = sessionSealer;
            this.sealedSessionRoute = Objects.requireNonNull(
                sealedSessionRoute, "sealedSessionRoute");
            this.seal = seal;
            this.permit = permit;
            this.owned = owned;
            this.target = target;
            this.relocationId = relocationId;
            this.state = state.clone();
            this.timerEnvelope = timerEnvelope.clone();
            this.initial = initial;
            this.stageRequest = stageRequest;
        }

        ZLinkStandaloneActorRelocationStagingOwner.Request targetRequest() {
            return new ZLinkStandaloneActorRelocationStagingOwner.Request(
                relocationId,
                owned.actorId(),
                owned.stableType(),
                owned.snapshot().objectGeneration(),
                owned.snapshot().authorityOwnerGeneration(),
                owned.snapshotPolicy(),
                target.entrySpotId().orElseThrow());
        }

        ZLinkAggregateRelocationCoordinator.StagedRoot initialRoot() {
            return initial;
        }

        ZLinkSpotRetireControl.StageRequest stageRequest() {
            return stageRequest;
        }

        synchronized CompletionStage<
            ZLinkAggregateRelocationCoordinator.Prepared> freezeAndPrepare(
                ZLinkStoreCancellation cancellation) {
            if (terminal || committed || prepared != null) {
                return failed(new IllegalStateException(
                    "Actor relocation source is already terminal"));
            }
            List<ZLinkAsyncSerialQueue.QueuedRecord> held =
                actors.freezeActorRelocationIngress(
                    owned.actorId(), seal).orElseThrow(() ->
                        new IllegalStateException(
                            "Actor relocation ingress freeze was lost"));
            List<ZLinkAsyncSerialQueue.QueuedRecord> journal =
                new ArrayList<>(seal.captured());
            journal.addAll(held);
            byte[] finalRoot =
                ZLinkCanonicalActorRelocationEnvelope.encode(
                    relocationId,
                    owned.actorId(),
                    owned.snapshot().objectGeneration(),
                    owned.snapshot().authorityOwnerGeneration(),
                    owned.snapshotPolicy(),
                    state,
                    journal,
                    timerEnvelope);
            if (!permit.tryShrinkPayload(finalRoot.length)) {
                return failed(new IllegalStateException(
                    "Actor relocation root exceeded its permit"));
            }
            var request = new ZLinkAggregateRelocationCoordinator.Request(
                initial.request().aggregateId(),
                initial.request().aggregateGeneration(),
                initial.request().participants(),
                finalRoot,
                initial.request().targetDescriptor(),
                initial.request().targetDescriptorLifecycleGeneration(),
                initial.request().capacityBundle(),
                initial.request().targetOwner());
            return coordinator.prepareReplayPending(request, cancellation)
                .thenApply(value -> {
                    synchronized (PreparedSource.this) {
                        prepared = value;
                        finalJournal = List.copyOf(journal);
                    }
                    return value;
                });
        }

        synchronized CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published> commitSourceQueue(
            ZLinkAggregateRelocationCoordinator.Published published,
            ZLinkStoreCancellation cancellation) {
            Objects.requireNonNull(published, "published");
            Objects.requireNonNull(cancellation, "cancellation");
            if (terminal || prepared == null) {
                return failed(new IllegalStateException(
                    "Actor relocation source queue cannot be committed"));
            }
            if (committed) {
                return CompletableFuture.completedFuture(
                    activatedPublication);
            }
            if (sourceCommitFlight != null) {
                return sourceCommitFlight;
            }
            if (relocationCommit == null) {
                relocationCommit = actors.retainActorRelocationCommit(
                        owned.actorId(), seal)
                    .orElseThrow(() -> new IllegalStateException(
                        "Actor relocation source queue cannot be committed"));
            }
            CompletableFuture<ZLinkAggregateRelocationCoordinator.Published>
                flight = new CompletableFuture<>();
            sourceCommitFlight = flight;
            commitNextSourceQueueCut(published, cancellation)
                .whenComplete((activated, failure) -> {
                Throwable terminal = failure;
                if (terminal == null) {
                    try {
                        bindCommittedReplies(
                            activated.targetOwnerGenerations());
                    } catch (RuntimeException bindFailure) {
                        terminal = bindFailure;
                    }
                }
                synchronized (PreparedSource.this) {
                    if (terminal == null) {
                        committed = true;
                        activatedPublication = activated;
                    } else {
                        sourceCommitFlight = null;
                    }
                }
                if (terminal == null) {
                    flight.complete(activated);
                } else {
                    flight.completeExceptionally(unwrap(terminal));
                }
            });
            return flight;
        }

        private CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published>
            commitNextSourceQueueCut(
                ZLinkAggregateRelocationCoordinator.Published published,
                ZLinkStoreCancellation cancellation) {
            if (cancellation.isCancellationRequested()) {
                return cancelled();
            }
            ZLinkAggregateRelocationCoordinator.Prepared authority;
            systems.zlink.framework.runtime.internal.relocation
                .ZLinkRetainedSerialQueueCommit.Commit retained;
            boolean finished;
            synchronized (this) {
                authority = prepared;
                retained = relocationCommit;
                finished = captureFinished;
            }
            if (finished) {
                return coordinator.activateCanonicalReplay(
                    authority, cancellation);
            }
            var cut = retained.cut();
            List<ZLinkAsyncSerialQueue.QueuedRecord> journal =
                new ArrayList<>(seal.captured());
            journal.addAll(cut.records());
            journal.sort((left, right) -> Long.compareUnsigned(
                left.sequence(), right.sequence()));
            byte[] root = ZLinkCanonicalActorRelocationEnvelope.encode(
                relocationId,
                owned.actorId(),
                owned.snapshot().objectGeneration(),
                owned.snapshot().authorityOwnerGeneration(),
                owned.snapshotPolicy(),
                state,
                journal,
                timerEnvelope);
            synchronized (this) {
                finalJournal = List.copyOf(journal);
                committedRoot = root.clone();
            }
            return coordinator.updateCanonicalReplay(
                    authority.request().participants().stream()
                        .map(value -> new ZLinkAggregateRelocationCoordinator
                            .ExpectedParticipant(
                                value.authorityKey(),
                                value.objectGeneration(),
                                value.authorityOwnerGeneration()))
                        .toList(),
                    authority.fence(),
                    authority.request().targetOwner(),
                    ignored -> ZLinkServiceRelocationEnvelopeCodec.decode(root),
                    cancellation)
                .thenCompose(ignored -> {
                    if (cancellation.isCancellationRequested()) {
                        return cancelled();
                    }
                    installRelocationForward(published);
                    if (!retained.tryEstablishDurableCut(cut)) {
                        return commitNextSourceQueueCut(
                            published, cancellation);
                    }
                    if (!retained.tryFinishCapture(cut)) {
                        return commitNextSourceQueueCut(
                            published, cancellation);
                    }
                    synchronized (PreparedSource.this) {
                        captureFinished = true;
                    }
                    return coordinator.activateCanonicalReplay(
                        authority, cancellation);
                });
        }

        private void installRelocationForward(
            ZLinkAggregateRelocationCoordinator.Published published) {
            long targetAuthorityGeneration = published.targetOwnerGeneration(
                owned.authorityKey());
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
            if (!committed || relocationCommit == null) {
                throw new IllegalStateException(
                    "Actor relocation source queue is not durably committed");
            }
            relocationCommit.complete();
        }

        CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published> commitAuthority(
            ZLinkStoreCancellation cancellation) {
            ZLinkAggregateRelocationCoordinator.Prepared authority;
            synchronized (this) {
                if (terminal || committed || prepared == null) {
                    return failed(new IllegalStateException(
                        "Actor relocation source is not ready for authority commit"));
                }
                authority = prepared;
            }
            return coordinator.commit(authority, cancellation);
        }

        CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published>
                completeSourceCleanup(
                    ZLinkAggregateRelocationCoordinator.Published published,
                    ZLinkStoreCancellation cancellation) {
            ZLinkAggregateRelocationCoordinator.Prepared authority;
            synchronized (this) {
                if (!committed || terminal || prepared == null) {
                    return failed(new IllegalStateException(
                        "Actor relocation source is not committed"));
                }
                authority = prepared;
            }
            return coordinator.completeSourceCleanup(
                published,
                committedRoot == null
                    ? authority.request().root()
                    : committedRoot,
                cancellation);
        }

        CompletionStage<Void> discardInitialAfterCommit() {
            synchronized (this) {
                if (!committed || terminal) {
                    return failed(new IllegalStateException(
                        "Actor relocation source is not committed"));
                }
            }
            return coordinator.discardStagedRoot(initial);
        }

        synchronized void releasePermitAfterCompletion() {
            if (!committed) {
                throw new IllegalStateException(
                    "Actor relocation source is not committed");
            }
            finish();
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
            CompletionStage<Void> authority = prepared == null
                ? CompletableFuture.completedFuture(null)
                : coordinator.abort(prepared);
            return authority
                .thenCompose(ignored -> abortSessionRoute(
                    sessionSealer, sealedSessionRoute))
                .thenCompose(ignored ->
                    coordinator.discardStagedRoot(initial))
                .thenRun(() -> {
                    if (!actors.abortActorRelocation(
                        owned.actorId(), seal)) {
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
                permit.close();
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
        long lastAcceptedSessionSequence) {
        private SealedSessionRoute {
            Objects.requireNonNull(route, "route");
            Objects.requireNonNull(seal, "seal");
            if (lastAcceptedSessionSequence < 0) {
                throw new IllegalArgumentException(
                    "sealed Session sequence must be nonnegative");
            }
        }
    }

    private record Owned(
        String authorityKey,
        String actorId,
        String stableType,
        ZLinkAuthoritySnapshot snapshot,
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
