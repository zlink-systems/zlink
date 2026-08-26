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
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.locations.ZLinkAuthorityKeyCodec;
import systems.zlink.framework.runtime.locations
    .ZLinkServiceAuthorityPayloadCodec;
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
    private static final int PAGE_SIZE = 1000;
    private static final int MAX_DESCRIPTORS = 65_536;
    private final String meshName;
    private final RoutingId localNodeRid;
    private final long localNodeGeneration;
    private final ZLinkLocationRepository locations;
    private final ZLinkAggregateRelocationCoordinator coordinator;
    private final ZLinkSpotLifecycle spots;
    private final ZLinkSpotRuntime relocationReplies;
    private final ZLinkSessionRelocationPeerClient sessionSealer;
    private final Duration sessionRelocationSealTimeout;
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
            spots,
            actors,
            adapters,
            spotFactories,
            actorFactories,
            relocationReplies,
            null,
            Duration.ofSeconds(3));
    }

    ZLinkUserSpotRetireSourceBuilder(
        String meshName,
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkSpotLifecycle spots,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        Map<String, RelocatableSpotFactory<?>> spotFactories,
        Map<String, RelocatableActorFactory<?>> actorFactories,
        ZLinkSpotRuntime relocationReplies,
        ZLinkSessionRelocationPeerClient sessionSealer) {
        this(
            meshName,
            localNodeRid,
            localNodeGeneration,
            locations,
            coordinator,
            spots,
            actors,
            adapters,
            spotFactories,
            actorFactories,
            relocationReplies,
            sessionSealer,
            Duration.ofSeconds(3));
    }

    ZLinkUserSpotRetireSourceBuilder(
        String meshName,
        RoutingId localNodeRid,
        long localNodeGeneration,
        ZLinkLocationRepository locations,
        ZLinkAggregateRelocationCoordinator coordinator,
        ZLinkSpotLifecycle spots,
        ZLinkActorSessionCoordinator actors,
        ZLinkRelocationAdapterRegistry adapters,
        Map<String, RelocatableSpotFactory<?>> spotFactories,
        Map<String, RelocatableActorFactory<?>> actorFactories,
        ZLinkSpotRuntime relocationReplies,
        ZLinkSessionRelocationPeerClient sessionSealer,
        Duration sessionRelocationSealTimeout) {
        if (meshName == null || meshName.isBlank()) {
            throw new IllegalArgumentException("meshName is required");
        }
        if (localNodeGeneration == 0) {
            throw new IllegalArgumentException(
                "local node generation must be positive");
        }
        this.meshName = meshName;
        this.localNodeRid = Objects.requireNonNull(
            localNodeRid, "localNodeRid");
        this.localNodeGeneration = localNodeGeneration;
        this.locations = Objects.requireNonNull(locations, "locations");
        this.coordinator = Objects.requireNonNull(coordinator, "coordinator");
        this.spots = Objects.requireNonNull(spots, "spots");
        this.relocationReplies = relocationReplies;
        this.sessionSealer = sessionSealer;
        if (sessionRelocationSealTimeout == null
            || sessionRelocationSealTimeout.isZero()
            || sessionRelocationSealTimeout.isNegative()) {
            throw new IllegalArgumentException(
                "session relocation seal timeout must be positive");
        }
        this.sessionRelocationSealTimeout = sessionRelocationSealTimeout;
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
        ZLinkStoreCancellation cancellation) {
        Objects.requireNonNull(targetPolicy, "targetPolicy");
        Objects.requireNonNull(cancellation, "cancellation");
        ZLinkSpot<?> spot = activeSpot(spotId, cancellation);
        if (spot == null) {
            return cancellation.isCancellationRequested()
                ? cancelled()
                : failed(new IllegalStateException(
                    "User Spot is not active locally: " + spotId));
        }
        return admit(spotId, spot, targetPolicy, cancellation)
            .thenApply(ignored -> null);
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
                            targetPolicy),
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
        ZLinkUserSpotRelocationBarrier barrier = spots.relocationBarrier(
            admission.inventory().spot().id(), actors);
        return barrier.sealForRelocation(preview -> {
                return preview.participantActorIds().equals(
                        admission.inventory().actorIds())
                    && !cancellation.isCancellationRequested();
            }, cancellation::isCancellationRequested)
            .thenCompose(sealed -> {
                if (sealed.isEmpty()) {
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
                            "User Spot relocation seal was unavailable"));
                }
                return captureSealed(
                    spot,
                    admission,
                    cancellation,
                    barrier,
                    sealed.orElseThrow());
            });
    }

    private CompletionStage<PreparedSource> captureSealed(
        ZLinkSpot<?> spot,
        Admission admission,
        ZLinkStoreCancellation cancellation,
        ZLinkUserSpotRelocationBarrier barrier,
        ZLinkUserSpotRelocationBarrier.Seal seal) {
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
                        //  The aggregate payload lives only in source
                        //  memory and travels directly with the stage
                        //  request (spec 28 §4.2).
                        if (cancellation.isCancellationRequested()) {
                            return cancelled();
                        }
                        return CompletableFuture.completedFuture(
                            new PreparedSource(
                                barrier,
                                seal,
                                exact,
                                spots,
                                actors,
                                relocationReplies,
                                sessionSealer,
                                sealed,
                                unresolved,
                                stageRequest(
                                    exact,
                                    aggregateId,
                                    root,
                                    admission.target())));
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
                        sealedSessions.get(),
                        false));
                    return failed(cause);
                }
                CompletionStage<Void> release = abortSessionRoutes(
                        sessionSealer,
                        sealedSessions.get())
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
                                    sealedSessions.get(),
                                    true));
                            return failed(cause);
                        }
                        if (relocationReplies != null) {
                            admission.inventory().actorIds().forEach(
                                relocationReplies
                                    ::resumeActorTimersAfterRelocationAbort);
                        }
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
                    route.bindingGeneration()));
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
                record.sourceBindingGeneration()));
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
                        command, sessionRelocationSealTimeout)
                    .thenAcceptAsync(acknowledgement -> {
                        sealed.add(new SealedSessionRoute(
                            route,
                            command));
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
                0);
        return sessionSealer.sendRoute(abort);
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
        ZLinkRelocationTargetPolicy targetPolicy) {
        return ZLinkRelocationTargetSelector.select(
            descriptors,
            targetPolicy,
            this::baseEligible,
            candidate -> hasCapabilities(inventory, candidate),
            candidate -> hasCapacity(inventory, candidate),
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
        UUID aggregateId,
        byte[] relocationPayload,
        ZLinkMeshNodeDescriptor target) {
        ZLinkAuthoritySnapshot source =
            captured.inventory().spot().snapshot();
        return new ZLinkSpotRetireControl.StageRequest(
            new ZLinkSpotRetireControl.Fence(aggregateId, 1),
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
            relocationPayload,
            participantFences(captured),
            captured.sessionRoutes());
    }

    static final class PreparedSource {
        private final ZLinkUserSpotRelocationBarrier barrier;
        private final ZLinkUserSpotRelocationBarrier.Seal seal;
        private final Captured captured;
        private final ZLinkSpotLifecycle spots;
        private final ZLinkActorSessionCoordinator actors;
        private final ZLinkSpotRuntime relocationReplies;
        private final ZLinkSessionRelocationPeerClient sessionSealer;
        private final List<SealedSessionRoute> sealedSessionRoutes;
        private final List<UnresolvedPreparation> unresolved;
        private final ZLinkSpotRetireControl.StageRequest stageRequest;
        private final ZLinkStateLane stateLane = new ZLinkStateLane();
        private Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>>
            finalJournal = Map.of();
        private ZLinkUserSpotRelocationBarrier.RelocationCommit
            relocationCommit;
        private CompletableFuture<ZLinkUserSpotRelocationBarrier.RelocationCommit>
            relocationCommitClaim;
        private boolean captureFinished;
        private boolean sourceCommitted;
        private boolean terminal;

        private record RetainClaim(
            ZLinkUserSpotRelocationBarrier.RelocationCommit retained,
            CompletableFuture<ZLinkUserSpotRelocationBarrier.RelocationCommit>
                completion,
            boolean owner) {
        }

        private <T> T inStateLane(Supplier<T> work) {
            try {
                return stateLane.runAsync(work).toCompletableFuture().join();
            } catch (CompletionException failure) {
                Throwable cause = failure.getCause();
                if (cause instanceof RuntimeException runtimeFailure) {
                    throw runtimeFailure;
                }
                if (cause instanceof Error error) {
                    throw error;
                }
                throw failure;
            }
        }

        private PreparedSource(
            ZLinkUserSpotRelocationBarrier barrier,
            ZLinkUserSpotRelocationBarrier.Seal seal,
            Captured captured,
            ZLinkSpotLifecycle spots,
            ZLinkActorSessionCoordinator actors,
            ZLinkSpotRuntime relocationReplies,
            ZLinkSessionRelocationPeerClient sessionSealer,
            List<SealedSessionRoute> sealedSessionRoutes,
            List<UnresolvedPreparation> unresolved,
            ZLinkSpotRetireControl.StageRequest stageRequest) {
            this.barrier = barrier;
            this.seal = seal;
            this.captured = captured;
            this.spots = Objects.requireNonNull(spots, "spots");
            this.actors = Objects.requireNonNull(actors, "actors");
            this.relocationReplies = relocationReplies;
            this.sessionSealer = sessionSealer;
            this.sealedSessionRoutes = List.copyOf(sealedSessionRoutes);
            this.unresolved = Objects.requireNonNull(unresolved, "unresolved");
            this.stageRequest = stageRequest;
        }

        ZLinkSpotRetireControl.StageRequest stageRequest() {
            return stageRequest;
        }

        ZLinkUserSpotAggregateStagingOwner.Request stagingRequest() {
            return captured.staging();
        }

        CompletionStage<Void> relayCapturedIngress(
            ZLinkRelocationTransitionClient client,
            Duration timeout) {
            Objects.requireNonNull(client, "client");
            Objects.requireNonNull(timeout, "timeout");
            RetainClaim claim;
            try {
                claim = inStateLane(() -> {
                    if (terminal || sourceCommitted) {
                        throw new IllegalStateException(
                            "source relocation relay boundary is terminal");
                    }
                    if (relocationCommit != null) {
                        return new RetainClaim(relocationCommit, null, false);
                    }
                    if (relocationCommitClaim != null) {
                        return new RetainClaim(
                            null, relocationCommitClaim, false);
                    }
                    var completion =
                        new CompletableFuture<ZLinkUserSpotRelocationBarrier
                            .RelocationCommit>();
                    relocationCommitClaim = completion;
                    return new RetainClaim(null, completion, true);
                });
            } catch (RuntimeException failure) {
                return failed(failure);
            }
            if (claim.retained() == null && !claim.owner()) {
                return claim.completion().thenCompose(ignored ->
                    relayCapturedIngress(client, timeout));
            }
            ZLinkUserSpotRelocationBarrier.RelocationCommit retained =
                claim.retained();
            if (claim.owner()) {
                try {
                    retained = barrier.retainCommit(seal)
                        .orElseThrow(() -> new IllegalStateException(
                            "source relocation barrier was lost"));
                    installExpectedRelocationForwards();
                    ZLinkUserSpotRelocationBarrier.RelocationCommit established =
                        retained;
                    retained = inStateLane(() -> {
                        relocationCommit = established;
                        relocationCommitClaim = null;
                        return relocationCommit;
                    });
                    ZLinkUserSpotRelocationBarrier.RelocationCommit published =
                        retained;
                    claim.completion().completeAsync(() -> published);
                } catch (RuntimeException failure) {
                    inStateLane(() -> {
                        if (relocationCommitClaim == claim.completion()) {
                            relocationCommitClaim = null;
                        }
                        return null;
                    });
                    claim.completion().completeAsync(() -> {
                        throw failure;
                    });
                    throw failure;
                }
            }
            ZLinkUserSpotRelocationBarrier.RelocationCommit.Cut cut;
            do {
                cut = retained.cut();
            } while (!retained.tryEstablishAndFinishCapture(cut));
            Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> relayed =
                cut.committed().heldIngress();
            inStateLane(() -> {
                finalJournal = relayed;
                captureFinished = true;
                return null;
            });
            CompletionStage<Void> chain =
                CompletableFuture.completedFuture(null);
            for (List<ZLinkAsyncSerialQueue.QueuedRecord> lane
                    : relayed.values()) {
                for (ZLinkAsyncSerialQueue.QueuedRecord record : lane) {
                    chain = chain.thenCompose(ignored -> client.relay(
                        stageRequest.targetNodeRid(),
                        stageRequest.fence(),
                        record.payload(),
                        timeout));
                }
            }
            return chain;
        }

        private void installExpectedRelocationForwards() {
            if (relocationReplies == null) {
                throw new IllegalStateException(
                    "relocation reply runtime is unavailable");
            }
            Map<String, Long> targetOwnerGenerations =
                new LinkedHashMap<>();
            for (var participant : stageRequest.participants()) {
                targetOwnerGenerations.put(
                    participant.authorityKey(),
                    Math.addExact(
                        participant.sourceAuthorityOwnerGeneration(), 1));
            }
            installRelocationForwards(targetOwnerGenerations);
            bindCommittedReplies(targetOwnerGenerations);
        }

        private void installRelocationForwards(
            Map<String, Long> targetOwnerGenerations) {
            var node = relocationReplies.nodeByRid(
                stageRequest.sourceNodeRid());
            Duration retention = relocationReplies.relocationForwardRetention();
            Owned spot = captured.inventory().spot();
            node.installRelocationSpotForward(
                spotRoute(spot, false, targetOwnerGenerations),
                spotRoute(spot, true, targetOwnerGenerations),
                retention);
            for (Owned actor : captured.inventory().actors()) {
                node.installRelocationActorForward(
                    actorRoute(actor, false, targetOwnerGenerations),
                    actorRoute(actor, true, targetOwnerGenerations),
                    retention);
            }
        }

        private ZLinkServiceM6BWireCodec.SpotRouteFence spotRoute(
            Owned spot,
            boolean target,
            Map<String, Long> targetOwnerGenerations) {
            return new ZLinkServiceM6BWireCodec.SpotRouteFence(
                spot.id(),
                spot.snapshot().objectGeneration(),
                target ? stageRequest.targetNodeRid()
                    : stageRequest.sourceNodeRid(),
                target ? stageRequest.targetNodeGeneration()
                    : stageRequest.sourceNodeGeneration(),
                target ? targetOwnerGeneration(
                    targetOwnerGenerations, spot.key())
                    : spot.snapshot().authorityOwnerGeneration(),
                target ? stageRequest.targetOwnerLeaseGeneration()
                    : spot.snapshot().ownerLeaseGeneration());
        }

        private ZLinkServiceM6BWireCodec.ActorRouteFence actorRoute(
            Owned actor,
            boolean target,
            Map<String, Long> targetOwnerGenerations) {
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
                target ? targetOwnerGeneration(
                    targetOwnerGenerations, actor.key())
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
                        stageRequest.fence().aggregateGeneration()));
            }
            if (relocationReplies == null) {
                throw new IllegalStateException(
                    "relocation reply runtime is unavailable");
            }
            Map<String, List<ZLinkAsyncSerialQueue.QueuedRecord>> journal =
                inStateLane(() -> finalJournal);
            relocationReplies.bindCanonicalRelocationReplies(
                journal,
                stageRequest.targetNodeRid(),
                stageRequest.targetNodeGeneration(),
                Map.copyOf(replyFences));
        }

        void completeSourceBarrierCommit() {
            ZLinkUserSpotRelocationBarrier.RelocationCommit retained =
                inStateLane(() -> {
                if (relocationCommit == null
                || !sourceCommitted && !captureFinished) {
                    throw new IllegalStateException(
                        "source relocation barrier is not durably committed");
                }
                sourceCommitted = true;
                return relocationCommit;
            });
            // CompletableFuture dependents may run inline, so complete only
            // after the lane turn has released its ownership marker.
            retained.complete();
        }

        boolean relayBoundaryCommitted() {
            return inStateLane(() -> sourceCommitted);
        }

        CompletionStage<Void> abortPrecommit() {
            try {
                inStateLane(() -> {
                    if (terminal || sourceCommitted) {
                        throw new IllegalStateException(
                            "committed source relocation cannot be aborted");
                    }
                    return null;
                });
            } catch (RuntimeException failure) {
                return failed(failure);
            }
            //  Contract order: Aborted record, staged payload discard, queue
            //  restore, then terminal progress removal while the lanes are
            //  still paused. Lane resume is the last observable step.
            return abortSessionRoutes(
                        sessionSealer, sealedSessionRoutes)
                    .exceptionallyCompose(failure -> {
                        rememberUnresolved(
                            unresolved,
                            new UnresolvedPreparation(
                                captured.inventory().spot().id(),
                                barrier,
                                seal,
                                sealedSessionRoutes,
                                true));
                        return failed(unwrap(failure));
                    })
                .thenCompose(ignored -> {
                    ZLinkUserSpotRelocationBarrier.RelocationCommit retained;
                    retained = inStateLane(() -> relocationCommit);
                    if (retained != null) {
                        if (!retained.abort()) {
                            return failed(new IllegalStateException(
                                "source relocation retained queue cannot be restored"));
                        }
                        inStateLane(() -> {
                            terminal = true;
                            return null;
                        });
                        forgetUnresolved(unresolved, seal);
                        if (relocationReplies != null) {
                            captured.inventory().actorIds().forEach(
                                relocationReplies
                                    ::resumeActorTimersAfterRelocationAbort);
                        }
                        return CompletableFuture.completedFuture(null);
                    }
                    return barrier.abortAsync(seal, () -> {
                            inStateLane(() -> {
                                terminal = true;
                                return null;
                            });
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
                        });
                });
        }

        CompletionStage<Void> discardInitialAfterCommit() {
            try {
                inStateLane(() -> {
                if (!sourceCommitted || terminal) {
                    throw new IllegalStateException(
                        "source relocation is not committed");
                }
                terminal = true;
                    return null;
                });
            } catch (RuntimeException failure) {
                return failed(failure);
            }
            return CompletableFuture.completedFuture(null);
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
        List<SealedSessionRoute> sealedSessionRoutes,
        boolean authorityAborted) {
        private UnresolvedPreparation {
            sealedSessionRoutes = List.copyOf(sealedSessionRoutes);
        }
    }

    private record SealedSessionRoute(
        ZLinkSpotRetireControl.SessionRouteFence route,
        ZLinkServiceM6BWireCodec.SessionRelocationSeal seal) {
        private SealedSessionRoute {
            Objects.requireNonNull(route, "route");
            Objects.requireNonNull(seal, "seal");
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
