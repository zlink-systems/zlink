package systems.zlink.framework.runtime.spots;
import java.nio.charset.StandardCharsets;
import java.time.Duration;
import java.time.Instant;
import java.util.Collections;
import java.util.concurrent.CancellationException;
import java.util.function.Supplier;
import java.util.stream.Stream;

import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ThreadLocalRandom;
import java.util.concurrent.atomic.AtomicReference;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.actors.ZLinkRelocationCancellation;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.locations.*;
import systems.zlink.framework.runtime.internal.locations.*;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkLocationRepository;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntimeState;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationPermitPool;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationStored;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations
    .ZLinkServiceAuthorityPayloadCodec;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocatableActorFactory;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocatableSpotFactory;
import systems.zlink.framework.runtime.internal.configuration
    .ZLinkObjectFactoryRegistration.RelocationPolicy;
import systems.zlink.framework.spots.ZLinkSpot;

/**
 * Builds the reversible source half of one User Spot Retire transaction from
 * live runtime objects and exact Location authority snapshots.
 */
final class ZLinkUserSpotRetireSourceBuilder {
    private static final long SNAPSHOT_RESERVATION_BYTES =
        64L * 1024 * 1024;
    private static final long ENVELOPE_RESERVATION_BYTES = 64L * 1024;
    private static final int PAGE_SIZE = 1000;
    private static final int MAX_DESCRIPTORS = 65_536;
    private static final byte[] SOURCE_CLEANUP_PENDING =
        "zlink.spot.source.pending.v1".getBytes(
            StandardCharsets.UTF_8);
    private static final Duration SESSION_SEAL_DEADLINE =
        Duration.ofSeconds(5);

    private final String meshName;
    private final RoutingId localNodeRid;
    private final long localNodeGeneration;
    private final ZLinkLocationRepository locations;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkRelocationPermitPool permits;
    private final ZLinkSpotLifecycle spots;
    private final ZLinkSpotRuntime relocationReplies;
    private final ZLinkSessionRelocationPeerClient sessionSealer;
    private final ZLinkActorSessionCoordinator actors;
    private final ZLinkRelocationAdapterRegistry adapters;
    private final Map<String, RelocatableSpotFactory<?>>
        spotFactories;
    private final Map<String, RelocatableActorFactory<?>>
        actorFactories;
    private final ZLinkServiceAuthorityPayloadCodec spotAuthorities =
        new ZLinkServiceAuthorityPayloadCodec();
    private final List<UnresolvedPreparation> unresolved =
        Collections.synchronizedList(new ArrayList<>());

    ZLinkUserSpotRetireSourceBuilder(
        String meshName,
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkRelocationPermitPool permits,
        ZLinkSpotLifecycle spots,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        Map<String, RelocatableSpotFactory<?>>
            spotFactories,
        Map<String, RelocatableActorFactory<?>>
            actorFactories) {
        this(
            meshName,
            localNodeRid,
            localNodeGeneration,
            locations,
            coordinator,
            permits,
            spots,
            actors,
            adapters,
            spotFactories,
            actorFactories,
            null);
    }

    ZLinkUserSpotRetireSourceBuilder(
        String meshName,
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkRelocationPermitPool permits,
        ZLinkSpotLifecycle spots,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        Map<String, RelocatableSpotFactory<?>>
            spotFactories,
        Map<String, RelocatableActorFactory<?>>
            actorFactories,
        ZLinkSpotRuntime relocationReplies) {
        this(
            meshName,
            localNodeRid,
            localNodeGeneration,
            locations,
            coordinator,
            permits,
            spots,
            actors,
            adapters,
            spotFactories,
            actorFactories,
            relocationReplies,
            null);
    }

    ZLinkUserSpotRetireSourceBuilder(
        String meshName,
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkRelocationPermitPool permits,
        ZLinkSpotLifecycle spots,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        Map<String, RelocatableSpotFactory<?>> spotFactories,
        Map<String, RelocatableActorFactory<?>> actorFactories,
        ZLinkSpotRuntime relocationReplies,
        ZLinkSessionRelocationPeerClient sessionSealer) {
        if (meshName == null || meshName.isBlank()) {
            throw new IllegalArgumentException("meshName is required");
        }
        if (localNodeGeneration <= 0) {
            throw new IllegalArgumentException(
                "local node generation must be positive");
        }
        this.meshName = meshName;
        this.localNodeRid = Objects.requireNonNull(
            localNodeRid, "localNodeRid");
        this.localNodeGeneration = localNodeGeneration;
        this.locations = Objects.requireNonNull(locations, "locations");
        this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
        this.permits = Objects.requireNonNull(permits, "permits");
        this.spots = Objects.requireNonNull(spots, "spots");
        this.relocationReplies = relocationReplies;
        this.sessionSealer = sessionSealer;
        this.actors = Objects.requireNonNull(actors, "actors");
        this.adapters = Objects.requireNonNull(adapters, "adapters");
        this.spotFactories = Map.copyOf(
            Objects.requireNonNull(spotFactories, "spotFactories"));
        this.actorFactories = Map.copyOf(
            Objects.requireNonNull(actorFactories, "actorFactories"));
    }

    CompletionStage<PreparedSource> prepare(
        String spotId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(targetPolicy, "targetPolicy");
        Objects.requireNonNull(cancellation, "cancellation");
        return reconcileUnresolvedPreparations().thenCompose(ignored -> {
            ZLinkSpot<?> spot = activeSpot(spotId, cancellation);
            if (spot == null) {
                return cancellation.isCancellationRequested()
                    ? cancelled()
                    : failed(new IllegalStateException(
                        "User Spot is not active locally: " + spotId));
            }
            return admit(spotId, spot, targetPolicy, cancellation)
                .thenCompose(admission -> sealAndCapture(
                    spot,
                    admission,
                    cancellation));
        });
    }

    CompletionStage<Void> preflight(
        String spotId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(targetPolicy, "targetPolicy");
        Objects.requireNonNull(capacityPlan, "capacityPlan");
        Objects.requireNonNull(cancellation, "cancellation");
        ZLinkSpot<?> spot = activeSpot(spotId, cancellation);
        if (spot == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "User Spot is not active locally: " + spotId));
        }
        return admit(
            spotId,
            spot,
            targetPolicy,
            capacityPlan,
            cancellation).thenApply(admission -> {
                capacityPlan.reserveUserSpot(
                    spotId,
                    admission.target(),
                    admission.inventory().spot().stableType(),
                    admission.inventory().actors().size());
                return null;
            });
    }

    CompletionStage<PreparedSource> preparePinned(
        String spotId,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkMeshNodeDescriptor pinnedTarget,
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(pinnedTarget, "pinnedTarget");
        return reconcileUnresolvedPreparations().thenCompose(ignored -> {
            ZLinkSpot<?> spot = activeSpot(spotId, cancellation);
            if (spot == null) {
                return cancellation.isCancellationRequested()
                    ? cancelled()
                    : failed(new IllegalStateException(
                        "User Spot is not active locally: " + spotId));
            }
            List<String> actorIds = actors.actorIdsInSpot(spotId);
            return readInventory(spotId, actorIds, cancellation)
                .thenCompose(inventory -> {
                    validateLiveInventory(spot, inventory);
                    return listDescriptors(cancellation)
                        .thenApply(descriptors -> new Admission(
                            inventory,
                            descriptors.stream()
                                .filter(candidate -> candidate.rid().equals(
                                    pinnedTarget.rid()))
                                .filter(candidate -> candidate
                                    .lifecycleGeneration()
                                    == pinnedTarget.lifecycleGeneration())
                                .filter(this::baseEligible)
                                .filter(candidate -> hasCapabilities(
                                    inventory, candidate))
                                .findFirst()
                                .orElseThrow(() ->
                                    new ZLinkUserSpotRetireRuntime
                                        .RelocationBlockedException(
                                        systems.zlink.framework.runtime.host
                                            .ZLinkFrameworkRelocationReason
                                            .TARGET_UNAVAILABLE,
                                        "Pinned User Spot relocation target "
                                            + "is no longer Ready")),
                            descriptors));
                })
                .thenCompose(admission -> sealAndCapture(
                    spot, admission, cancellation));
        });
    }

    private ZLinkSpot<?> activeSpot(
        String spotId,
        ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested()) {
            return null;
        }
        return spots.spotFor(spotId);
    }

    private CompletionStage<Admission> admit(
        String spotId,
        ZLinkSpot<?> spot,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkStoreCancellation cancellation) {
        return admit(
            spotId, spot, targetPolicy, null, cancellation);
    }

    private CompletionStage<Admission> admit(
        String spotId,
        ZLinkSpot<?> spot,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan,
        ZLinkStoreCancellation cancellation) {
        List<String> actorIds = actors.actorIdsInSpot(spotId);
        return readInventory(spotId, actorIds, cancellation)
            .thenCompose(inventory -> {
                validateLiveInventory(spot, inventory);
                return listDescriptors(cancellation)
                    .thenApply(descriptors -> new Admission(
                        inventory,
                        selectTarget(
                            inventory,
                            descriptors,
                            targetPolicy,
                            capacityPlan),
                        descriptors));
            });
    }

    int unresolvedPreparationCount() {
        return unresolved.size();
    }

    CompletionStage<Void> reconcileUnresolvedPreparations() {
        List<UnresolvedPreparation> pending;
        synchronized (unresolved) {
            pending = List.copyOf(unresolved);
        }
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (UnresolvedPreparation context : pending) {
            chain = chain.thenCompose(ignored -> {
                if (!context.authorityAborted()) {
                    return failed(new IllegalStateException(
                        "User Spot relocation authority outcome is unresolved: "
                            + context.spotId()));
                }
                return abortSessionRoutes(
                        sessionSealer,
                        context.sealedSessionRoutes())
                    .thenCompose(aborted -> context.stagedRoot() == null
                        ? CompletableFuture.completedFuture(null)
                        : coordinator.discardStagedRoot(
                            context.stagedRoot()))
                    .thenCompose(discarded -> context.barrier().abortAsync(
                        context.seal()))
                    .thenAccept(aborted -> {
                        if (!aborted) {
                            throw new IllegalStateException(
                                "source relocation barrier was lost during "
                                    + "Session abort recovery");
                        }
                        if (relocationReplies != null) {
                            context.seal().participantActorIds().forEach(
                                relocationReplies
                                    ::resumeActorTimersAfterRelocationAbort);
                        }
                        context.permit().close();
                        unresolved.remove(context);
                    });
            });
        }
        return chain;
    }

    private CompletionStage<PreparedSource> sealAndCapture(
        ZLinkSpot<?> spot,
        Admission admission,
        ZLinkStoreCancellation cancellation) {
        AtomicReference<ZLinkRelocationPermitPool.Lease> acquired =
            new AtomicReference<>();
        int captures = snapshotCount(admission.inventory());
        ZLinkUserSpotRelocationBarrier barrier = spots.relocationBarrier(
            admission.inventory().spot().id(), actors);
        return barrier.sealForRelocation(preview -> {
                if (!preview.participantActorIds().equals(
                        admission.inventory().actorIds())
                    || cancellation.isCancellationRequested()) {
                    return false;
                }
                long estimate = estimatePayload(preview, captures);
                ZLinkRelocationPermitPool.Lease lease = permits.tryAcquire(
                    ZLinkRelocationPermitPool.Request.outboundAggregate(
                        estimate,
                        captures,
                        true));
                acquired.set(lease);
                return lease != null;
            }, cancellation::isCancellationRequested)
            .thenCompose(sealed -> {
                if (sealed.isEmpty()) {
                    close(acquired.get());
                    //  A deadline that elapses while draining toward the seal
                    //  is a Blocked outcome, not a store failure. Diagnostics
                    //  only; nothing here reaches the wire.
                    return cancellation.isCancellationRequested()
                        ? failed(new ZLinkUserSpotRetireRuntime
                            .RelocationBlockedException(
                                systems.zlink.framework.runtime.host
                                    .ZLinkFrameworkRelocationReason
                                    .DEADLINE_EXCEEDED,
                                "User Spot drain and seal deadline elapsed "
                                    + "before Capture"))
                        : failed(new IllegalStateException(
                            "User Spot relocation seal or permit was unavailable"));
                }
                ZLinkRelocationPermitPool.Lease lease = acquired.get();
                if (lease == null) {
                    return barrier.abortAsync(sealed.orElseThrow())
                        .thenCompose(ignored -> failed(
                            new IllegalStateException(
                                "User Spot relocation permit was not acquired")));
                }
                return captureSealed(
                    spot,
                    admission,
                    cancellation,
                    barrier,
                    sealed.orElseThrow(),
                    lease);
            });
    }

    private CompletionStage<PreparedSource> captureSealed(
        ZLinkSpot<?> spot,
        Admission admission,
        ZLinkStoreCancellation cancellation,
        ZLinkUserSpotRelocationBarrier barrier,
        ZLinkUserSpotRelocationBarrier.Seal seal,
        ZLinkRelocationPermitPool.Lease lease) {
        AtomicReference<ZLinkAggregateRelocationCoordinator.StagedRoot>
            staged = new AtomicReference<>();
        AtomicReference<List<SealedSessionRoute>> sealedSessions =
            new AtomicReference<>(List.of());
        return readInventory(
                admission.inventory().spot().id(),
                seal.participantActorIds(),
                cancellation)
            .thenCompose(current -> {
                if (!sameInventory(admission.inventory(), current)) {
                    return failed(new IllegalStateException(
                        "User Spot authority changed before Capture"));
                }
                return barrier.runCapture(
                    seal,
                    () -> capture(
                        spot,
                        current,
                        seal,
                        admission.target(),
                        admission.descriptors(),
                        cancellation));
            })
            .thenCompose(captured -> {
                UUID aggregateId = UUID.randomUUID();
                return sealSessionRoutes(
                        captured,
                        aggregateId,
                        sealedSessions)
                    .thenCompose(sealed -> {
                        Captured exact = captured.withSessionRoutes(
                            sealed.stream()
                                .map(SealedSessionRoute::route)
                                .toList());
                        List<ZLinkSpotRetireControl.ParticipantFence> inventory =
                            participantFences(exact);
                        byte[] root =
                            ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                                exact.staging(),
                                aggregateId,
                                exact.inventory().spot().snapshot()
                                    .authorityOwnerGeneration(),
                                inventory);
                        if (!lease.tryShrinkPayload(root.length)) {
                            return failed(new IllegalStateException(
                                "captured relocation root exceeded its permit"));
                        }
                        var authorityRequest = relocationRequest(
                            exact,
                            root,
                            admission.target(),
                            aggregateId);
                        return coordinator.stageRoot(
                                authorityRequest, cancellation)
                            .thenCompose(stagedRoot -> {
                                staged.set(stagedRoot);
                                if (cancellation.isCancellationRequested()) {
                                    return cancelled();
                                }
                                return CompletableFuture.completedFuture(
                                    new PreparedSource(
                                        coordinator,
                                        barrier,
                                        seal,
                                        lease,
                                        exact,
                                        authorityRequest,
                                        stagedRoot,
                                        spots,
                                        actors,
                                        relocationReplies,
                                        sessionSealer,
                                        sealed,
                                        unresolved,
                                        stageRequest(
                                            exact,
                                            authorityRequest,
                                            stagedRoot.stored(),
                                            admission.target())));
                            });
                    });
            })
            .exceptionallyCompose(failure -> {
                Throwable cause = unwrap(failure);
                if (cause instanceof ZLinkAggregateRelocationCoordinator
                    .PreparationOutcomeUnknownException) {
                    unresolved.add(new UnresolvedPreparation(
                        admission.inventory().spot().id(),
                        barrier,
                        seal,
                        lease,
                        sealedSessions.get(),
                        staged.get(),
                        false));
                    return failed(cause);
                }
                CompletionStage<Void> release = abortSessionRoutes(
                        sessionSealer,
                        sealedSessions.get())
                    .thenCompose(ignored -> staged.get() == null
                        ? CompletableFuture.completedFuture(null)
                        : coordinator.discardStagedRoot(staged.get()))
                    .thenCompose(ignored -> barrier.abortAsync(seal))
                    .thenAccept(aborted -> {
                        if (!aborted) {
                            throw new IllegalStateException(
                                "source relocation barrier was lost during abort");
                        }
                    });
                return release.handle((ignored, releaseFailure) -> releaseFailure)
                    .thenCompose(releaseFailure -> {
                        if (releaseFailure != null) {
                            cause.addSuppressed(unwrap(releaseFailure));
                            rememberUnresolved(
                                unresolved,
                                new UnresolvedPreparation(
                                    admission.inventory().spot().id(),
                                    barrier,
                                    seal,
                                    lease,
                                    sealedSessions.get(),
                                    staged.get(),
                                    true));
                            return failed(cause);
                        }
                        if (relocationReplies != null) {
                            admission.inventory().actorIds().forEach(
                                relocationReplies
                                    ::resumeActorTimersAfterRelocationAbort);
                        }
                        lease.close();
                        return failed(cause);
                    });
            });
    }

    private CompletionStage<Captured> capture(
        ZLinkSpot<?> spot,
        Inventory inventory,
        ZLinkUserSpotRelocationBarrier.Seal seal,
        ZLinkMeshNodeDescriptor target,
        List<ZLinkMeshNodeDescriptor> descriptors,
        ZLinkStoreCancellation cancellation) {
        ZLinkRelocationCancellation relocationCancellation =
            cancellation::isCancellationRequested;
        CompletionStage<byte[]> spotState = captureState(
            inventory.spot().stableType(),
            inventory.spot().policy(),
            () -> adapters.captureSpot(
                inventory.spot().stableType(),
                spot,
                relocationCancellation));
        return spotState.thenCompose(state -> {
            List<ZLinkUserSpotAggregateStagingOwner.ActorParticipant>
                participants = new ArrayList<>();
            CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
            for (Owned actor : inventory.actors()) {
                chain = chain.thenCompose(ignored -> {
                    ZLinkActor live = actors.localActor(actor.id())
                        .orElseThrow(() -> new IllegalStateException(
                            "Actor disappeared during Capture: " + actor.id()));
                    return captureState(
                            actor.stableType(),
                            actor.policy(),
                            () -> adapters.captureActor(
                                actor.stableType(),
                                live,
                                relocationCancellation))
                        .thenAccept(actorState -> participants.add(
                            new ZLinkUserSpotAggregateStagingOwner.ActorParticipant(
                                actor.id(),
                                actor.stableType(),
                                actorState,
                                isSnapshot(actor.policy()),
                                actors.actorRef(actor.id()),
                                relocationReplies == null
                                    ? ZLinkSpotTimerRelocationEnvelope
                                        .encodeCanonical(List.of())
                                    : relocationReplies
                                        .freezeActorTimerRelocationEnvelope(
                                            actor.id()))));
                });
            }
            return chain.thenApply(ignored -> new Captured(
                inventory,
                new ZLinkUserSpotAggregateStagingOwner.Request(
                    spotFactories.get(inventory.spot().stableType()).spotType(),
                    inventory.spot().stableType(),
                    inventory.spot().id(),
                    inventory.spot().snapshot().objectGeneration(),
                    state,
                    isSnapshot(inventory.spot().policy()),
                    seal.timerEnvelope(),
                    participants,
                    seal.capturedRecords()),
                sessionRoutes(
                    inventory,
                    descriptors,
                    seal.capturedRecords())));
        });
    }

    private List<ZLinkSpotRetireControl.SessionRouteFence> sessionRoutes(
        Inventory inventory,
        List<ZLinkMeshNodeDescriptor> descriptors,
        Map<String, List<systems.zlink.framework.execution
            .ZLinkAsyncSerialQueue.QueuedRecord>> acceptedRecords) {
        List<ZLinkSpotRetireControl.SessionRouteFence> routes =
            new ArrayList<>();
        for (Owned actor : inventory.actors()) {
            var knownRoute = actors.boundSessionRoute(actor.id());
            knownRoute.ifPresent(route -> {
                ZLinkMeshNodeDescriptor owner = descriptors.stream()
                    .filter(value -> value.rid().equals(
                        route.sessionOwnerNodeRid()))
                    .findFirst()
                    .orElseThrow(() -> new IllegalStateException(
                        "bound Session owner descriptor is unavailable: "
                            + route.sessionOwnerNodeRid()));
                routes.add(new ZLinkSpotRetireControl.SessionRouteFence(
                    actor.id(),
                    actor.snapshot().objectGeneration(),
                    actor.snapshot().authorityOwnerGeneration(),
                    actor.snapshot().storeVersion(),
                    route.sessionOwnerNodeRid(),
                    owner.lifecycleGeneration(),
                    owner.ownerId(),
                    owner.leaseGeneration(),
                    route.sessionRid(),
                    route.bindingGeneration(),
                    route.lastAcceptedSessionSequence()));
            });
        }
        for (byte[] encoded : acceptedRecords.values().stream()
            .flatMap(List::stream)
            .map(systems.zlink.framework.execution
                .ZLinkAsyncSerialQueue.QueuedRecord::payload)
            .toList()) {
            ZLinkActorAcceptedJournal.Record record;
            try {
                record = ZLinkActorAcceptedJournal.decode(encoded);
            } catch (IllegalArgumentException notActorRecord) {
                continue;
            }
            if (record.sourceSessionRid() == null
                || routes.stream().anyMatch(route ->
                    route.actorId().equals(record.actorId()))) {
                continue;
            }
            Owned actor = inventory.actors().stream()
                .filter(value -> value.id().equals(record.actorId()))
                .findFirst()
                .orElseThrow(() -> new IllegalStateException(
                    "bound Session record is outside Actor inventory: "
                        + record.actorId()));
            routes.add(new ZLinkSpotRetireControl.SessionRouteFence(
                actor.id(),
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
        routes.sort((left, right) -> Arrays.compareUnsigned(
            left.actorId().getBytes(StandardCharsets.UTF_8),
            right.actorId().getBytes(StandardCharsets.UTF_8)));
        return List.copyOf(routes);
    }

    private CompletionStage<List<SealedSessionRoute>> sealSessionRoutes(
        Captured captured,
        UUID aggregateId,
        AtomicReference<List<SealedSessionRoute>> observed) {
        if (captured.sessionRoutes().isEmpty()) {
            return CompletableFuture.completedFuture(List.of());
        }
        if (sessionSealer == null) {
            return failed(new ZLinkUserSpotRetireRuntime
                .RelocationBlockedException(
                    systems.zlink.framework.runtime.host
                        .ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE,
                    "Bound-Session User Spot relocation requires the "
                        + "command 42/43 seal barrier"));
        }
        List<SealedSessionRoute> sealed = new ArrayList<>();
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (ZLinkSpotRetireControl.SessionRouteFence route
            : captured.sessionRoutes()) {
            Owned actor = captured.inventory().actors().stream()
                .filter(candidate -> candidate.id().equals(route.actorId()))
                .findFirst()
                .orElseThrow(() -> new IllegalStateException(
                    "bound Session route Actor is outside the inventory: "
                        + route.actorId()));
            chain = chain.thenCompose(ignored -> {
                ZLinkServiceM6BWireCodec.SessionRelocationSeal command =
                    sessionSeal(captured, actor, route, aggregateId);
                return sessionSealer.sealRouteUntilAck(
                        command, SESSION_SEAL_DEADLINE)
                    .thenAcceptAsync(acknowledgement -> {
                        ZLinkSpotRetireControl.SessionRouteFence exact =
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
                                acknowledgement
                                    .lastAcceptedSessionSequence());
                        sealed.add(new SealedSessionRoute(
                            exact,
                            command,
                            acknowledgement.lastAcceptedSessionSequence()));
                        observed.set(List.copyOf(sealed));
                    });
            });
        }
        return chain.thenApply(ignored -> List.copyOf(sealed));
    }

    private ZLinkServiceM6BWireCodec.SessionRelocationSeal sessionSeal(
        Captured captured,
        Owned actor,
        ZLinkSpotRetireControl.SessionRouteFence route,
        UUID aggregateId) {
        ZLinkAuthoritySnapshot source =
            captured.inventory().spot().snapshot();
        return new ZLinkServiceM6BWireCodec.SessionRelocationSeal(
            new ZLinkServiceM6BWireCodec.RelocationIdentity(
                aggregateId.getMostSignificantBits(),
                aggregateId.getLeastSignificantBits()),
            new ZLinkServiceM6BWireCodec.RelocationCoordinatorFence(
                source.ownerId(),
                source.ownerLeaseGeneration(),
                localNodeRid,
                localNodeGeneration,
                route.sourceAuthorityStoreVersion()),
            ZLinkServiceM6BWireCodec.RelocationRole.SOURCE,
            new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef(
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
    }

    private static CompletionStage<Void> abortSessionRoutes(
        ZLinkSessionRelocationPeerClient sessionSealer,
        List<SealedSessionRoute> contexts) {
        if (contexts.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        if (sessionSealer == null) {
            return failed(new IllegalStateException(
                "Session relocation abort sender is unavailable"));
        }
        AtomicReference<Throwable> firstFailure = new AtomicReference<>();
        CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
        for (SealedSessionRoute context : List.copyOf(contexts)) {
            chain = chain.thenCompose(ignored ->
                abortSessionRoute(sessionSealer, context)
                    .handle((aborted, failure) -> {
                        if (failure != null) {
                            Throwable cause = unwrap(failure);
                            Throwable first = firstFailure.get();
                            if (first == null
                                && firstFailure.compareAndSet(null, cause)) {
                                return null;
                            }
                            firstFailure.get().addSuppressed(cause);
                        }
                        return null;
                    }));
        }
        return chain.thenCompose(ignored -> firstFailure.get() == null
            ? CompletableFuture.completedFuture(null)
            : failed(firstFailure.get()));
    }

    private static CompletionStage<Void> abortSessionRoute(
        ZLinkSessionRelocationPeerClient sessionSealer,
        SealedSessionRoute context) {
        ZLinkServiceM6BWireCodec.SessionRelocationSeal command42 =
            context.seal();
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
                abort,
                context.lastAcceptedSessionSequence(),
                SESSION_SEAL_DEADLINE)
            .thenApply(ignored -> null);
    }

    private static CompletionStage<byte[]> captureState(
        String stableType,
        RelocationPolicy policy,
        Supplier<CompletionStage<byte[]>> capture) {
        if (policy instanceof RelocationPolicy.Recreate) {
            return CompletableFuture.completedFuture(new byte[0]);
        }
        if (policy
            instanceof RelocationPolicy.PreserveState) {
            return Objects.requireNonNull(
                    capture.get(),
                    "Capture returned null")
                .thenApply(value -> Objects.requireNonNull(
                    value,
                    "Capture returned null state").clone());
        }
        return failed(new IllegalStateException(
            "relocation is disabled for stable type: " + stableType));
    }

    private CompletionStage<Inventory> readInventory(
        String spotId,
        List<String> actorIds,
        ZLinkStoreCancellation cancellation) {
        return readOwned(
                ZLinkAuthorityKeyCodec.spot(spotId),
                spotId,
                ZLinkPlacementObjectKind.USER_SPOT,
                cancellation)
            .thenCompose(spot -> {
                List<Owned> actorRows = new ArrayList<>();
                CompletionStage<Void> chain = CompletableFuture.completedFuture(null);
                for (String actorId : actorIds) {
                    chain = chain.thenCompose(ignored -> readOwned(
                            ZLinkAuthorityKeyCodec.actor(actorId),
                            actorId,
                            ZLinkPlacementObjectKind.ACTOR,
                            cancellation)
                        .thenAccept(actorRows::add));
                }
                return chain.thenApply(ignored -> new Inventory(
                    withPolicy(spot),
                    actorRows.stream().map(this::withPolicy).toList()));
            });
    }

    private CompletionStage<Owned> readOwned(
        String key,
        String id,
        ZLinkPlacementObjectKind kind,
        ZLinkStoreCancellation cancellation) {
        if (cancellation.isCancellationRequested()) {
            return cancelled();
        }
        return locations.read(key, cancellation).thenCompose(read -> {
            if (!(read instanceof ZLinkAuthoritySnapshot snapshot)
                || snapshot.allocation().state()
                    != ZLinkPlacementAllocationState.ACTIVE
                || snapshot.allocation().objectKind() != kind
                || !snapshot.allocation().descriptor().meshName()
                    .equals(meshName)
                || !snapshot.allocation().descriptor().rid()
                    .equals(localNodeRid)
                || snapshot.allocation().descriptorLifecycleGeneration()
                    != localNodeGeneration) {
                return failed(new IllegalStateException(
                    "relocation authority is not Ready on the source: " + key));
            }
            return CompletableFuture.completedFuture(new Owned(
                key,
                id,
                snapshot.allocation().stableType(),
                snapshot,
                null));
        });
    }

    private Owned withPolicy(Owned owned) {
        RelocationPolicy policy;
        if (owned.snapshot().allocation().objectKind()
            == ZLinkPlacementObjectKind.USER_SPOT) {
            var factory = spotFactories.get(owned.stableType());
            policy = factory == null ? null : factory.relocationPolicy();
        } else {
            var factory = actorFactories.get(owned.stableType());
            policy = factory == null ? null : factory.relocationPolicy();
        }
        if (policy == null
            || policy
                instanceof RelocationPolicy.Disabled) {
            throw new IllegalStateException(
                "relocation policy is unavailable: " + owned.stableType());
        }
        return new Owned(
            owned.key(),
            owned.id(),
            owned.stableType(),
            owned.snapshot(),
            policy);
    }

    private void validateLiveInventory(ZLinkSpot<?> spot, Inventory inventory) {
        var authority = spotAuthorities.decode(inventory.spot().snapshot().payload())
            .orElseThrow(() -> new IllegalStateException(
                "User Spot authority payload is invalid"));
        if (authority.user().isEmpty()
            || authority.state()
                != ZLinkServiceAuthorityPayloadCodec.State.READY
            || !authority.spotId().equals(inventory.spot().id())
            || !authority.stableType().equals(inventory.spot().stableType())
            || !authority.meshName().equals(meshName)
            || !authority.nodeRid().equals(localNodeRid)
            || authority.nodeGeneration() != localNodeGeneration
            || !spotFactories.get(inventory.spot().stableType())
                .spotType().isInstance(spot)) {
            throw new IllegalStateException(
                "live User Spot does not match Location authority");
        }
        for (Owned actor : inventory.actors()) {
            if (!actors.actorType(actor.id()).equals(actor.stableType())
                || actors.actorRef(actor.id()).generation()
                    != actor.snapshot().objectGeneration()
                || !actor.snapshot().ownerId().equals(
                    inventory.spot().snapshot().ownerId())
                || actor.snapshot().ownerLeaseGeneration()
                    != inventory.spot().snapshot().ownerLeaseGeneration()) {
                throw new IllegalStateException(
                    "live Actor does not match aggregate authority: "
                        + actor.id());
            }
        }
    }

    private CompletionStage<List<ZLinkMeshNodeDescriptor>> listDescriptors(
        ZLinkStoreCancellation cancellation) {
        List<ZLinkMeshNodeDescriptor> result = new ArrayList<>();
        return listDescriptorPage(null, result, cancellation);
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
                    return CompletableFuture.completedFuture(List.copyOf(result));
                }
                if (next.equals(cursor)) {
                    return failed(new IllegalStateException(
                        "MeshNode descriptor cursor did not advance"));
                }
                return listDescriptorPage(next, result, cancellation);
            });
    }

    private ZLinkMeshNodeDescriptor selectTarget(
        Inventory inventory,
        List<ZLinkMeshNodeDescriptor> descriptors,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan) {
        return ZLinkRelocationTargetSelector.select(
            descriptors,
            targetPolicy,
            this::baseEligible,
            candidate -> hasCapabilities(inventory, candidate),
            candidate -> hasCapacity(inventory, candidate)
                && (capacityPlan == null
                    || capacityPlan.canReserveUserSpot(
                        candidate,
                        inventory.spot().stableType(),
                        inventory.actors().size())),
            "No eligible User Spot relocation target is Ready");
    }

    private boolean baseEligible(ZLinkMeshNodeDescriptor candidate) {
        if (!candidate.meshName().equals(meshName)
            || candidate.rid().equals(localNodeRid)
            || candidate.state() != ZLinkFrameworkRuntimeState.SERVING
            || candidate.objectRole() != ZLinkMeshNodeObjectRole.SERVER) {
            return false;
        }
        return true;
    }

    private boolean hasCapabilities(
        Inventory inventory,
        ZLinkMeshNodeDescriptor candidate) {
        if (!hasCapability(candidate, inventory.spot())) {
            return false;
        }
        for (Owned actor : inventory.actors()) {
            if (!hasCapability(candidate, actor)) {
                return false;
            }
        }
        return true;
    }

    private static boolean hasCapacity(
        Inventory inventory,
        ZLinkMeshNodeDescriptor candidate) {
        return hasCapacity(candidate.capacity().spots(), 1)
            && hasCapacity(
                candidate.capacity().actors(),
                inventory.actors().size())
            && hasTypeCapacity(
                candidate,
                ZLinkPlacementObjectKind.USER_SPOT,
                inventory.spot().stableType(),
                1)
            && (candidate.activationConcurrency().limit() == 0
                || candidate.activationConcurrency().active()
                    < candidate.activationConcurrency().limit());
    }

    private static boolean hasCapability(
        ZLinkMeshNodeDescriptor target,
        Owned participant) {
        ZLinkPlacementObjectKind kind =
            participant.snapshot().allocation().objectKind();
        ZLinkObjectMaintenancePolicyKind policy = policyKind(
            participant.policy());
        return target.objectCapabilities().stream().anyMatch(capability ->
            capability.objectKind() == kind
                && capability.stableType().equals(participant.stableType())
                && capability.policy() == policy
                && capability.hasSnapshotAdapter() == isSnapshot(
                    participant.policy()));
    }

    private static boolean hasCapacity(ZLinkCapacityUsage usage, int required) {
        return usage.limit() == 0
            || (long) usage.active() + usage.reserved() + required
                <= usage.limit();
    }

    private static boolean hasTypeCapacity(
        ZLinkMeshNodeDescriptor target,
        ZLinkPlacementObjectKind kind,
        String stableType,
        int required) {
        return target.capacity().spotTypes().stream()
            .filter(type -> type.objectKind() == kind
                && type.stableType().equals(stableType))
            .findFirst()
            .map(type -> hasCapacity(type.usage(), required))
            .orElse(true);
    }

    private static boolean sameInventory(Inventory expected, Inventory actual) {
        if (!sameOwned(expected.spot(), actual.spot())
            || !expected.actorIds().equals(actual.actorIds())) {
            return false;
        }
        for (int index = 0; index < expected.actors().size(); index++) {
            if (!sameOwned(expected.actors().get(index), actual.actors().get(index))) {
                return false;
            }
        }
        return true;
    }

    private static boolean sameOwned(Owned expected, Owned actual) {
        return expected.key().equals(actual.key())
            && expected.stableType().equals(actual.stableType())
            && expected.snapshot().storeVersion().equals(
                actual.snapshot().storeVersion())
            && expected.snapshot().objectGeneration()
                == actual.snapshot().objectGeneration()
            && expected.snapshot().authorityOwnerGeneration()
                == actual.snapshot().authorityOwnerGeneration()
            && expected.snapshot().ownerId().equals(actual.snapshot().ownerId())
            && expected.snapshot().ownerLeaseGeneration()
                == actual.snapshot().ownerLeaseGeneration()
            && Arrays.equals(
                expected.snapshot().payload(),
                actual.snapshot().payload());
    }

    private static int snapshotCount(Inventory inventory) {
        int count = isSnapshot(inventory.spot().policy()) ? 1 : 0;
        for (Owned actor : inventory.actors()) {
            if (isSnapshot(actor.policy())) {
                count++;
            }
        }
        return count;
    }

    private static long estimatePayload(
        ZLinkUserSpotRelocationBarrier.Preview preview,
        int snapshotCount) {
        long bytes = ENVELOPE_RESERVATION_BYTES;
        bytes = Math.addExact(bytes, preview.timerEnvelope().length);
        for (var records : preview.capturedRecords().values()) {
            for (var record : records) {
                bytes = Math.addExact(bytes, 16L + record.payload().length);
            }
        }
        return Math.addExact(
            bytes,
            Math.multiplyExact(SNAPSHOT_RESERVATION_BYTES, snapshotCount));
    }

    private static ZLinkObjectMaintenancePolicyKind policyKind(
        RelocationPolicy policy) {
        if (policy
            instanceof RelocationPolicy.PreserveState) {
            return ZLinkObjectMaintenancePolicyKind.SNAPSHOT;
        }
        if (policy instanceof RelocationPolicy.Recreate) {
            return ZLinkObjectMaintenancePolicyKind.RECREATE;
        }
        return ZLinkObjectMaintenancePolicyKind.DISABLED;
    }

    private static boolean isSnapshot(
        RelocationPolicy policy) {
        return policy
            instanceof RelocationPolicy.PreserveState;
    }

    private static ZLinkAggregateRelocationCoordinator.Request
        relocationRequest(
            Captured captured,
            byte[] root,
            ZLinkMeshNodeDescriptor target,
            UUID aggregateId) {
        List<ZLinkAggregateRelocationCoordinator.Participant> participants =
            new ArrayList<>();
        participants.add(participant(
            captured.inventory().spot(),
            SOURCE_CLEANUP_PENDING));
        for (Owned actor : captured.inventory().actors()) {
            participants.add(participant(actor, new byte[0]));
        }
        return new ZLinkAggregateRelocationCoordinator.Request(
            aggregateId,
            1,
            participants,
            root,
            new ZLinkMeshNodeDescriptorKey(target.meshName(), target.rid()),
            target.lifecycleGeneration(),
            new ZLinkPlacementCapacityBundle(
                captured.inventory().actors().size(),
                1,
                Optional.of(new ZLinkSpotTypeCapacityDelta(
                    ZLinkPlacementObjectKind.USER_SPOT,
                    captured.inventory().spot().stableType(),
                    1))),
            new ZLinkLocationOwnerToken(
                target.ownerId(),
                target.leaseGeneration()));
    }

    private static ZLinkAggregateRelocationCoordinator.Participant participant(
        Owned owned,
        byte[] completion) {
        return new ZLinkAggregateRelocationCoordinator.Participant(
            owned.key(),
            owned.snapshot().allocation().objectKind(),
            owned.snapshot().objectGeneration(),
            owned.snapshot().authorityOwnerGeneration(),
            owned.snapshot().storeVersion(),
            ZLinkAuthorityGenerationTransition.NEW_OWNER,
            owned.snapshot().payload(),
            completion);
    }

    private static List<ZLinkSpotRetireControl.ParticipantFence>
        participantFences(Captured captured) {
        return Stream.concat(
                    Stream.of(captured.inventory().spot()),
                    captured.inventory().actors().stream())
                .map(owned -> new ZLinkSpotRetireControl.ParticipantFence(
                    owned.key(),
                    owned.snapshot().allocation().objectKind()
                        == ZLinkPlacementObjectKind.ACTOR ? 1 : 2,
                    owned.id(),
                    owned.stableType(),
                    isSnapshot(owned.policy()),
                    owned.snapshot().objectGeneration(),
                    owned.snapshot().authorityOwnerGeneration()))
                .sorted((left, right) -> Arrays.compareUnsigned(
                    left.authorityKey().getBytes(
                        StandardCharsets.UTF_8),
                    right.authorityKey().getBytes(
                        StandardCharsets.UTF_8)))
                .toList();
    }

    private ZLinkSpotRetireControl.StageRequest stageRequest(
        Captured captured,
        ZLinkAggregateRelocationCoordinator.Request authorityRequest,
        ZLinkRelocationStored stored,
        ZLinkMeshNodeDescriptor target) {
        ZLinkAuthoritySnapshot source =
            captured.inventory().spot().snapshot();
        return new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(
                authorityRequest.aggregateId(),
                authorityRequest.aggregateGeneration()),
            localNodeRid,
            localNodeGeneration,
            source.ownerId(),
            source.ownerLeaseGeneration(),
            target.rid(),
            target.lifecycleGeneration(),
            target.ownerId(),
            target.leaseGeneration(),
            meshName,
            captured.inventory().spot().id(),
            captured.inventory().spot().stableType(),
            false,
            captured.staging().restoreSpotSnapshot(),
            stored.reference(),
            stored.checksumCrc32c(),
            participantFences(captured),
            captured.sessionRoutes());
    }

    static final class PreparedSource {
        private final ZLinkAggregateRelocationCoordinator coordinator;
        private final ZLinkUserSpotRelocationBarrier barrier;
        private final ZLinkUserSpotRelocationBarrier.Seal seal;
        private final ZLinkRelocationPermitPool.Lease permit;
        private final Captured captured;
        private final ZLinkAggregateRelocationCoordinator.Request
            authorityRequest;
        private final ZLinkAggregateRelocationCoordinator.StagedRoot
            stagedRoot;
        private final ZLinkSpotLifecycle spots;
        private final ZLinkActorSessionCoordinator actors;
        private final ZLinkSpotRuntime relocationReplies;
        private final ZLinkSessionRelocationPeerClient sessionSealer;
        private final List<SealedSessionRoute> sealedSessionRoutes;
        private final List<UnresolvedPreparation> unresolved;
        private final ZLinkSpotRetireControl.StageRequest stageRequest;
        private ZLinkAggregateRelocationCoordinator.Prepared finalPrepared;
        private ZLinkAggregateRelocationCoordinator.Prepared
            uncertainPrepared;
        private boolean finalJournalEmpty;
        private Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            finalJournal = Map.of();
        private ZLinkUserSpotRelocationBarrier.RelocationCommit
            relocationCommit;
        private byte[] committedRoot;
        private CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published> sourceCommitFlight;
        private ZLinkAggregateRelocationCoordinator.Published
            activatedPublication;
        private boolean captureFinished;
        private boolean sourceCommitted;
        private boolean terminal;

        private PreparedSource(
            ZLinkAggregateRelocationCoordinator coordinator,
            ZLinkUserSpotRelocationBarrier barrier,
            ZLinkUserSpotRelocationBarrier.Seal seal,
            ZLinkRelocationPermitPool.Lease permit,
            Captured captured,
            ZLinkAggregateRelocationCoordinator.Request authorityRequest,
            ZLinkAggregateRelocationCoordinator.StagedRoot stagedRoot,
            ZLinkSpotLifecycle spots,
            ZLinkActorSessionCoordinator actors,
            ZLinkSpotRuntime relocationReplies,
            ZLinkSessionRelocationPeerClient sessionSealer,
            List<SealedSessionRoute> sealedSessionRoutes,
            List<UnresolvedPreparation> unresolved,
            ZLinkSpotRetireControl.StageRequest stageRequest) {
            this.coordinator = coordinator;
            this.barrier = barrier;
            this.seal = seal;
            this.permit = permit;
            this.captured = captured;
            this.authorityRequest = authorityRequest;
            this.stagedRoot = stagedRoot;
            this.spots = Objects.requireNonNull(spots, "spots");
            this.actors = Objects.requireNonNull(actors, "actors");
            this.relocationReplies = relocationReplies;
            this.sessionSealer = sessionSealer;
            this.sealedSessionRoutes = List.copyOf(sealedSessionRoutes);
            this.unresolved = Objects.requireNonNull(unresolved, "unresolved");
            this.stageRequest = stageRequest;
        }

        ZLinkAggregateRelocationCoordinator.StagedRoot stagedRoot() {
            return stagedRoot;
        }

        ZLinkSpotRetireControl.StageRequest stageRequest() {
            return stageRequest;
        }

        ZLinkUserSpotAggregateStagingOwner.Request stagingRequest() {
            return captured.staging();
        }

        synchronized CompletionStage<ZLinkAggregateRelocationCoordinator.Prepared>
            freezeAndPrepareFinal(ZLinkStoreCancellation cancellation) {
            if (terminal || sourceCommitted || finalPrepared != null) {
                return failed(new IllegalStateException(
                    "source relocation final root is already terminal"));
            }
            Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> held =
                barrier.freezeIngress(seal).orElseThrow(() ->
                    new IllegalStateException(
                        "source relocation ingress freeze was lost"));
            var finalStaging = withHeldIngress(captured.staging(), held);
            byte[] finalRoot = ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                finalStaging,
                authorityRequest.aggregateId(),
                captured.inventory().spot().snapshot()
                    .authorityOwnerGeneration(),
                participantFences(captured));
            var request = new ZLinkAggregateRelocationCoordinator.Request(
                authorityRequest.aggregateId(),
                authorityRequest.aggregateGeneration(),
                authorityRequest.participants(),
                finalRoot,
                authorityRequest.targetDescriptor(),
                authorityRequest.targetDescriptorLifecycleGeneration(),
                authorityRequest.capacityBundle(),
                authorityRequest.targetOwner());
            return coordinator.prepareReplayPending(request, cancellation)
                .thenApply(prepared -> {
                    synchronized (PreparedSource.this) {
                        finalPrepared = prepared;
                        uncertainPrepared = null;
                        finalJournal = finalStaging.acceptedJournal();
                        finalJournalEmpty = finalStaging.acceptedJournal()
                            .values().stream().allMatch(List::isEmpty);
                    }
                    return prepared;
                })
                .exceptionallyCompose(failure -> {
                    Throwable cause = unwrap(failure);
                    if (cause instanceof ZLinkAggregateRelocationCoordinator
                        .PreparationOutcomeUnknownException unknown) {
                        synchronized (PreparedSource.this) {
                            uncertainPrepared = unknown.prepared();
                        }
                    }
                    return failed(cause);
                });
        }

        synchronized CompletionStage<Void> requireReplaySupport() {
            if (finalPrepared == null) {
                return failed(new IllegalStateException(
                    "source relocation final root is not prepared"));
            }
            return CompletableFuture.completedFuture(null);
        }

        synchronized CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published> commitSourceBarrier(
            ZLinkAggregateRelocationCoordinator.Published published,
            ZLinkStoreCancellation cancellation) {
            if (terminal || finalPrepared == null) {
                return failed(new IllegalStateException(
                    "source relocation final root is not prepared or is terminal"));
            }
            Objects.requireNonNull(published, "published");
            Objects.requireNonNull(cancellation, "cancellation");
            if (sourceCommitted) {
                return CompletableFuture.completedFuture(
                    activatedPublication);
            }
            if (sourceCommitFlight != null) {
                return sourceCommitFlight;
            }
            if (relocationCommit == null) {
                relocationCommit = barrier.retainCommit(seal).orElseThrow(() ->
                    new IllegalStateException(
                        "source relocation barrier was lost"));
            }
            CompletableFuture<ZLinkAggregateRelocationCoordinator.Published>
                flight = new CompletableFuture<>();
            sourceCommitFlight = flight;
            commitNextSourceBarrierCut(published, cancellation)
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
                        sourceCommitted = true;
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
            commitNextSourceBarrierCut(
                ZLinkAggregateRelocationCoordinator.Published published,
                ZLinkStoreCancellation cancellation) {
            if (cancellation.isCancellationRequested()) {
                return cancelled();
            }
            ZLinkAggregateRelocationCoordinator.Prepared authority;
            ZLinkUserSpotRelocationBarrier.RelocationCommit retained;
            boolean finished;
            synchronized (this) {
                authority = finalPrepared;
                retained = relocationCommit;
                finished = captureFinished;
            }
            if (finished) {
                return coordinator.activateCanonicalReplay(
                    authority, cancellation);
            }
            var cut = retained.cut();
            var finalStaging = withHeldIngress(
                captured.staging(),
                cut.committed().heldIngress());
            Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> journal =
                finalStaging.acceptedJournal();
            byte[] root = ZLinkCanonicalUserSpotRelocationEnvelope.encode(
                finalStaging,
                authorityRequest.aggregateId(),
                captured.inventory().spot().snapshot()
                    .authorityOwnerGeneration(),
                participantFences(captured));
            synchronized (this) {
                finalJournal = journal;
                finalJournalEmpty = journal.values().stream()
                    .allMatch(List::isEmpty);
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
                    installRelocationForwards(published);
                    if (!retained.tryEstablishDurableCut(cut)) {
                        return commitNextSourceBarrierCut(
                            published, cancellation);
                    }
                    if (!retained.tryFinishCapture(cut)) {
                        return commitNextSourceBarrierCut(
                            published, cancellation);
                    }
                    synchronized (PreparedSource.this) {
                        captureFinished = true;
                    }
                    return coordinator.activateCanonicalReplay(
                        authority, cancellation);
                });
        }

        private void installRelocationForwards(
            ZLinkAggregateRelocationCoordinator.Published published) {
            var node = relocationReplies.nodeByRid(
                stageRequest.sourceNodeRid());
            Duration retention = relocationReplies.relocationForwardRetention();
            Owned spot = captured.inventory().spot();
            node.installRelocationSpotForward(
                spotRoute(spot, false, published),
                spotRoute(spot, true, published),
                retention);
            for (Owned actor : captured.inventory().actors()) {
                node.installRelocationActorForward(
                    actorRoute(actor, false, published),
                    actorRoute(actor, true, published),
                    retention);
            }
        }

        private ZLinkServiceM6BWireCodec.SpotRouteFence spotRoute(
            Owned spot,
            boolean target,
            ZLinkAggregateRelocationCoordinator.Published published) {
            return new ZLinkServiceM6BWireCodec.SpotRouteFence(
                spot.id(),
                spot.snapshot().objectGeneration(),
                target ? stageRequest.targetNodeRid()
                    : stageRequest.sourceNodeRid(),
                target ? stageRequest.targetNodeGeneration()
                    : stageRequest.sourceNodeGeneration(),
                target ? published.targetOwnerGeneration(spot.key())
                    : spot.snapshot().authorityOwnerGeneration(),
                target ? stageRequest.targetOwnerLeaseGeneration()
                    : spot.snapshot().ownerLeaseGeneration());
        }

        private ZLinkServiceM6BWireCodec.ActorRouteFence actorRoute(
            Owned actor,
            boolean target,
            ZLinkAggregateRelocationCoordinator.Published published) {
            RoutingId nodeRid = target
                ? stageRequest.targetNodeRid()
                : stageRequest.sourceNodeRid();
            return new ZLinkServiceM6BWireCodec.ActorRouteFence(
                new systems.zlink.framework.runtime.internal.backend
                    .ZLinkBackendActorRef(
                        nodeRid,
                        actor.id(),
                        actor.snapshot().objectGeneration()),
                target ? stageRequest.targetNodeGeneration()
                    : stageRequest.sourceNodeGeneration(),
                target ? published.targetOwnerGeneration(actor.key())
                    : actor.snapshot().authorityOwnerGeneration(),
                target ? stageRequest.targetOwnerLeaseGeneration()
                    : actor.snapshot().ownerLeaseGeneration());
        }

        private void bindCommittedReplies(
            Map<String, Long> targetOwnerGenerations) {
            Map<String, ZLinkSpotRelocationReplyRoutes.CommittedFence>
                replyFences = new LinkedHashMap<>();
            for (int index = 0;
                index < stageRequest.participants().size();
                index++) {
                var participant = stageRequest.participants().get(index);
                replyFences.put(
                    participant.objectId().equals(stageRequest.spotId())
                        ? "spot"
                        : participant.objectId(),
                    new ZLinkSpotRelocationReplyRoutes.CommittedFence(
                        participant.authorityKey(),
                        index + 1L,
                        targetOwnerGeneration(
                            targetOwnerGenerations,
                            participant.authorityKey())));
            }
            if (relocationReplies == null) {
                throw new IllegalStateException(
                    "relocation reply runtime is unavailable");
            }
            relocationReplies.bindCanonicalRelocationReplies(
                finalJournal,
                stageRequest.targetNodeRid(),
                stageRequest.targetNodeGeneration(),
                Map.copyOf(replyFences));
        }

        synchronized void completeSourceBarrierCommit() {
            if (!sourceCommitted || relocationCommit == null) {
                throw new IllegalStateException(
                    "source relocation barrier is not durably committed");
            }
            relocationCommit.complete();
        }

        CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published> commitAuthority(
            ZLinkStoreCancellation cancellation) {
            ZLinkAggregateRelocationCoordinator.Prepared authority;
            synchronized (this) {
                if (sourceCommitted || terminal || finalPrepared == null) {
                    return failed(new IllegalStateException(
                        "source relocation is not ready for authority commit"));
                }
                authority = finalPrepared;
            }
            return coordinator.commit(authority, cancellation);
        }

        CompletionStage<
            ZLinkAggregateRelocationCoordinator.Published>
                completeSourceCleanup(
                    ZLinkAggregateRelocationCoordinator.Published published,
                    byte[] completedRoot,
                    ZLinkStoreCancellation cancellation) {
            synchronized (this) {
                if (!sourceCommitted || terminal) {
                    return failed(new IllegalStateException(
                        "source relocation is not committed"));
                }
            }
            return coordinator.completeSourceCleanup(
                published,
                completedRoot,
                cancellation);
        }

        synchronized CompletionStage<Void> abortPrecommit() {
            if (terminal || sourceCommitted) {
                return failed(new IllegalStateException(
                    "committed source relocation cannot be aborted"));
            }
            CompletionStage<Void> abortAuthority = finalPrepared == null
                ? uncertainPrepared != null
                    ? coordinator.abort(uncertainPrepared)
                    : CompletableFuture.completedFuture(null)
                : coordinator.abort(finalPrepared);
            //  Contract order: Aborted record, staged payload discard, queue
            //  restore, then terminal progress removal while the lanes are
            //  still paused. Lane resume is the last observable step.
            return abortAuthority
                .thenCompose(ignored -> abortSessionRoutes(
                        sessionSealer, sealedSessionRoutes)
                    .exceptionallyCompose(failure -> {
                        rememberUnresolved(
                            unresolved,
                            new UnresolvedPreparation(
                                captured.inventory().spot().id(),
                                barrier,
                                seal,
                                permit,
                                sealedSessionRoutes,
                                stagedRoot,
                                true));
                        return failed(unwrap(failure));
                    }))
                .thenCompose(ignored -> coordinator.discardStagedRoot(
                    stagedRoot))
                .thenCompose(ignored -> barrier.abortAsync(seal, () -> {
                        synchronized (PreparedSource.this) {
                            terminal = true;
                            permit.close();
                        }
                        forgetUnresolved(unresolved, seal);
                    })
                    .thenAccept(aborted -> {
                    if (!aborted) {
                        throw new IllegalStateException(
                            "source relocation barrier was lost during abort");
                    }
                    if (relocationReplies != null) {
                        captured.inventory().actorIds().forEach(
                            relocationReplies
                                ::resumeActorTimersAfterRelocationAbort);
                    }
                }));
        }

        CompletionStage<Void> discardInitialAfterCommit() {
            synchronized (this) {
                if (!sourceCommitted || terminal) {
                    return failed(new IllegalStateException(
                        "source relocation is not committed"));
                }
            }
            return coordinator.discardStagedRoot(stagedRoot);
        }

        CompletionStage<Void> cleanupLocal(Instant deadline) {
            Objects.requireNonNull(deadline, "deadline");
            List<String> actorIds = captured.inventory().actorIds();
            String spotId = captured.inventory().spot().id();
            long generation = captured.inventory().spot().snapshot()
                .objectGeneration();
            if (relocationReplies != null) {
                actorIds.forEach(
                    relocationReplies::closeActorTimersAfterRelocation);
            }
            return actors.completeRelocationSource(actorIds)
                .thenCompose(ignored -> spots.completeRelocationSource(
                    spotId,
                    generation,
                    deadline));
        }

        synchronized void releasePermitAfterCompletion() {
            if (!sourceCommitted || terminal) {
                throw new IllegalStateException(
                    "source relocation is not ready to release its permit");
            }
            terminal = true;
            permit.close();
        }
    }

    private static ZLinkUserSpotAggregateStagingOwner.Request withHeldIngress(
        ZLinkUserSpotAggregateStagingOwner.Request initial,
        Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> held) {
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> journal =
            new LinkedHashMap<>();
        initial.acceptedJournal().forEach((lane, records) ->
            journal.put(lane, new ArrayList<>(records)));
        held.forEach((lane, records) -> journal
            .computeIfAbsent(lane, ignored -> new ArrayList<>())
            .addAll(records));
        LinkedHashMap<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> frozen =
            new LinkedHashMap<>();
        journal.forEach((lane, records) -> frozen.put(
            lane,
            List.copyOf(records)));
        return new ZLinkUserSpotAggregateStagingOwner.Request(
            initial.spotType(),
            initial.spotStableType(),
            initial.spotId(),
            initial.objectGeneration(),
            initial.spotState(),
            initial.restoreSpotSnapshot(),
            initial.timerEnvelope(),
            initial.actors(),
            frozen);
    }

    private record Admission(
        Inventory inventory,
        ZLinkMeshNodeDescriptor target,
        List<ZLinkMeshNodeDescriptor> descriptors) {
        private Admission {
            descriptors = List.copyOf(descriptors);
        }
    }

    private record Inventory(Owned spot, List<Owned> actors) {
        private Inventory {
            actors = List.copyOf(actors);
        }

        List<String> actorIds() {
            return actors.stream().map(Owned::id).toList();
        }
    }

    private record Owned(
        String key,
        String id,
        String stableType,
        ZLinkAuthoritySnapshot snapshot,
        RelocationPolicy policy) {
    }

    private record Captured(
        Inventory inventory,
        ZLinkUserSpotAggregateStagingOwner.Request staging,
        List<ZLinkSpotRetireControl.SessionRouteFence> sessionRoutes) {
        private Captured {
            sessionRoutes = List.copyOf(sessionRoutes);
        }

        private Captured withSessionRoutes(
            List<ZLinkSpotRetireControl.SessionRouteFence> exact) {
            return new Captured(inventory, staging, exact);
        }
    }

    private record UnresolvedPreparation(
        String spotId,
        ZLinkUserSpotRelocationBarrier barrier,
        ZLinkUserSpotRelocationBarrier.Seal seal,
        ZLinkRelocationPermitPool.Lease permit,
        List<SealedSessionRoute> sealedSessionRoutes,
        ZLinkAggregateRelocationCoordinator.StagedRoot stagedRoot,
        boolean authorityAborted) {
        private UnresolvedPreparation {
            sealedSessionRoutes = List.copyOf(sealedSessionRoutes);
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

    private static void rememberUnresolved(
        List<UnresolvedPreparation> unresolved,
        UnresolvedPreparation context) {
        synchronized (unresolved) {
            boolean known = unresolved.stream().anyMatch(candidate ->
                candidate.seal().equals(context.seal()));
            if (!known) {
                unresolved.add(context);
            }
        }
    }

    private static void forgetUnresolved(
        List<UnresolvedPreparation> unresolved,
        ZLinkUserSpotRelocationBarrier.Seal seal) {
        synchronized (unresolved) {
            unresolved.removeIf(candidate -> candidate.seal().equals(seal));
        }
    }

    private static void close(AutoCloseable closeable) {
        if (closeable == null) {
            return;
        }
        try {
            closeable.close();
        } catch (Exception ignored) {
        }
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
