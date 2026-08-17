package systems.zlink.framework.runtime.spots;
import java.util.ArrayList;
import java.util.HashSet;
import java.util.Optional;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Supplier;
import java.util.stream.Collectors;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;

import java.time.Duration;
import java.time.Instant;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BooleanSupplier;
import java.util.function.Function;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.internal.locations.ZLinkRelocationStore;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.relocation
    .ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason;

/** Process-wide production bridge from host Retire to per-Mesh Spot units. */
public final class ZLinkUserSpotRetireRuntime {
    private static final Duration CONTROL_TIMEOUT = Duration.ofSeconds(5);

    private final ZLinkSpotRuntime spots;
    private final ZLinkActorRuntime actors;
    private final long applicationVersion;
    private final Optional<String> maintenanceWave;
    private final Duration sessionRelocationSealTimeout;
    private final Map<String, Lane> lanes;
    private final Map<String, ActorLane> actorLanes;

    public static final class RelocationBlockedException
        extends IllegalStateException {
        private final ZLinkFrameworkRelocationReason reason;

        public RelocationBlockedException(
            ZLinkFrameworkRelocationReason reason,
            String message) {
            super(message);
            this.reason = Objects.requireNonNull(reason, "reason");
        }

        public ZLinkFrameworkRelocationReason reason() {
            return reason;
        }
    }

    public ZLinkUserSpotRetireRuntime(
        ZLinkSpotRuntime spots,
        ZLinkActorRuntime actors,
        List<MeshNodeRegistration> registrations,
        Map<String, ZLinkInternalMeshNode> nodes,
        ZLinkLocationRepository locations,
        ZLinkLocationRepository authorities,
        ZLinkRelocationStore relocationStore,
        ZLinkLocationOptions options,
        ZLinkRelocationAdapterRegistry adapters,
        long applicationVersion,
        Optional<String> maintenanceWave,
        Duration sessionRelocationSealTimeout) {
        this.spots = Objects.requireNonNull(spots, "spots");
        this.actors = Objects.requireNonNull(actors, "actors");
        if (applicationVersion < 0) {
            throw new IllegalArgumentException(
                "applicationVersion must not be negative");
        }
        this.applicationVersion = applicationVersion;
        this.maintenanceWave = Objects.requireNonNull(
            maintenanceWave, "maintenanceWave");
        if (sessionRelocationSealTimeout == null
            || sessionRelocationSealTimeout.isZero()
            || sessionRelocationSealTimeout.isNegative()) {
            throw new IllegalArgumentException(
                "session relocation seal timeout must be positive");
        }
        this.sessionRelocationSealTimeout = sessionRelocationSealTimeout;
        Objects.requireNonNull(registrations, "registrations");
        Objects.requireNonNull(nodes, "nodes");
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            Objects.requireNonNull(authorities, "authorities"),
            Objects.requireNonNull(relocationStore, "relocationStore"));
        Objects.requireNonNull(options, "options");
        LinkedHashMap<String, Lane> configured = new LinkedHashMap<>();
        LinkedHashMap<String, ActorLane> configuredActors =
            new LinkedHashMap<>();
        var actorJoin = new ZLinkActorJoinCanonicalAdapter(actors, spots);
        for (MeshNodeRegistration registration : registrations) {
            ZLinkInternalMeshNode node = nodes.get(registration.meshName());
            Map<String, systems.zlink.framework.runtime.internal.configuration
                .ZLinkObjectFactoryRegistration.RelocatableSpotFactory<?>>
                relocatableSpots = registration.relocatableSpotFactories()
                    .entrySet().stream()
                    .filter(entry -> !(entry.getValue().relocationPolicy()
                        instanceof systems.zlink.framework.runtime.internal
                            .configuration.ZLinkObjectFactoryRegistration
                            .RelocationPolicy.Disabled))
                    .collect(Collectors.toUnmodifiableMap(
                        Map.Entry::getKey,
                        Map.Entry::getValue));
            Map<String, systems.zlink.framework.runtime.internal.configuration
                .ZLinkObjectFactoryRegistration.RelocatableActorFactory<?>>
                relocatableActors = registration.relocatableActorFactories()
                    .entrySet().stream()
                    .filter(entry -> !(entry.getValue().relocationPolicy()
                        instanceof systems.zlink.framework.runtime.internal
                            .configuration.ZLinkObjectFactoryRegistration
                            .RelocationPolicy.Disabled))
                    .collect(Collectors.toUnmodifiableMap(
                        Map.Entry::getKey,
                        Map.Entry::getValue));
            if (node == null
                || relocatableSpots.isEmpty()
                    && relocatableActors.isEmpty()) {
                continue;
            }
            var staging = new ZLinkUserSpotAggregateStagingOwner(
                spots,
                adapters);
            var peerClient = new ZLinkSessionRelocationPeerClient(node);
            var relocationReplyClient = ZLinkSpotRetireControl.client(node);
            var target = new ZLinkUserSpotRetireTargetEndpoint(
                node.status().routingId(),
                node.status().lifecycleGeneration(),
                coordinator,
                staging,
                stableType -> relocatableSpots
                    .containsKey(stableType)
                        ? relocatableSpots
                            .get(stableType).spotType()
                        : null,
                null,
                peerClient,
                CONTROL_TIMEOUT,
                request -> coordinator.normalizePublishedAggregate(
                        request.participants().stream()
                            .map(value -> new ZLinkAggregateRelocationCoordinator
                                .ExpectedParticipant(
                                    value.authorityKey(),
                                    value.objectGeneration(),
                                    value.sourceAuthorityOwnerGeneration()))
                            .toList(),
                        new ZLinkAggregateFence(
                            request.fence().aggregateId(),
                            request.fence().aggregateGeneration()),
                        new ZLinkLocationOwnerToken(
                                request.targetOwnerId(),
                                request.targetOwnerLeaseGeneration()),
                        () -> false)
                    .thenRun(() -> restoreBoundSessions(
                        actors,
                        node,
                        request)),
                spots,
                relocationReplyClient,
                locations,
                new ZLinkStandaloneActorRelocationStagingOwner(
                    node.spotNode(),
                    spots.actorSessions(),
                    adapters,
                    spots),
                actorJoin);
            var relocationClient =
                new ZLinkCanonicalRelocationStateMachine(
                    node,
                    registration.meshName(),
                    registration.entrySpotId(),
                    locations,
                    coordinator,
                    target,
                    spots::scheduleRelocationCleanup);
            new ZLinkCanonicalRelocationTransitionOwner(relocationClient)
                .install(node);
            node.setRelocationReplyRelayHandler(
                target::relayCanonicalReply);
            node.spotNode().setRelocationStagingIngressHandler(target);
            if (!relocatableSpots.isEmpty()) {
                configured.put(registration.meshName(), new Lane(
                    new ZLinkUserSpotRetireSourceBuilder(
                    registration.meshName(),
                    node.status().routingId(),
                    node.status().lifecycleGeneration(),
                    locations,
                    coordinator,
                    spots.spotLifecycle(),
                    spots.actorSessions(),
                    adapters,
                    relocatableSpots,
                    relocatableActors,
                    spots,
                    peerClient,
                    sessionRelocationSealTimeout),
                new ZLinkUserSpotRetireScheduler(),
                    relocationClient,
                    node));
            }
            if (!relocatableActors.isEmpty()) {
                var actorSource =
                    new ZLinkStandaloneActorRelocationSourceBuilder(
                        registration.meshName(),
                        node.status().routingId(),
                        node.status().lifecycleGeneration(),
                        locations,
                        coordinator,
                        spots.actorSessions(),
                        adapters,
                        relocatableActors,
                        spots,
                        peerClient,
                        sessionRelocationSealTimeout);
                configuredActors.put(
                    registration.meshName(),
                    new ActorLane(
                        actorSource,
                        new ZLinkStandaloneActorRelocationScheduler(),
                        relocationClient,
                        node));
                actorJoin.register(
                    node.status().routingId(),
                    node,
                    actorSource,
                    relocationClient);
            }
        }
        lanes = Map.copyOf(configured);
        actorLanes = Map.copyOf(configuredActors);
        if (!actorLanes.isEmpty()) {
            actors.setActorJoinRelocationPort(actorJoin);
        }
    }

    private static void restoreBoundSessions(
        ZLinkActorRuntime actors,
        ZLinkInternalMeshNode node,
        ZLinkSpotRetireControl.StageRequest request) {
        for (ZLinkSpotRetireControl.SessionRouteFence route
            : request.sessionRoutes()) {
            ZLinkActor actor = actors.localActor(route.actorId())
                .orElseThrow(() -> new IllegalStateException(
                    "relocated Actor is unavailable while restoring its "
                        + "bound Session: " + route.actorId()));
            actors.bindNativeSession(
                actor,
                node.spotNode(),
                actors.currentRef(actor),
                route.sessionOwnerNodeRid(),
                route.sessionRid());
        }
    }

    public CompletionStage<Void> startup() {
        return CompletableFuture.completedFuture(null);
    }

    public boolean requiresStartupRecovery() {
        return false;
    }

    public boolean supportsActiveInventory() {
        HashSet<String> aggregateActors = new HashSet<>();
        for (String spotId : spots.activeUserSpotIds()) {
            aggregateActors.addAll(spots.actorSessions().actorIdsInSpot(spotId));
            if (!lanes.containsKey(spots.userSpotMeshName(spotId))) {
                return false;
            }
        }
        for (String actorId : actors.activeActorIds()) {
            if (!aggregateActors.contains(actorId)
                && !actorLanes.containsKey(
                    spots.actorSessions().actorMeshName(actorId))) {
                return false;
            }
        }
        return true;
    }

    public CompletionStage<Void> relocateAll(Instant deadline) {
        return relocateAll(deadline, () -> false);
    }

    public CompletionStage<Void> relocateAll(
        Instant deadline,
        BooleanSupplier stopBeforeNextUnit) {
        return relocateAll(
            deadline,
            stopBeforeNextUnit,
            ZLinkFrameworkRelocationMode.PLANNED_MAINTENANCE,
            applicationVersion,
            () -> {
            });
    }

    public CompletionStage<Void> relocateAll(
        Instant deadline,
        BooleanSupplier stopBeforeNextUnit,
        ZLinkFrameworkRelocationMode mode,
        long targetApplicationVersion,
        Runnable onPreflightPassed) {
        return relocateAll(
            deadline,
            stopBeforeNextUnit,
            mode,
            targetApplicationVersion,
            () -> {
                onPreflightPassed.run();
                return CompletableFuture.completedFuture(null);
            });
    }

    public CompletionStage<Void> relocateAll(
        Instant deadline,
        BooleanSupplier stopBeforeNextUnit,
        ZLinkFrameworkRelocationMode mode,
        long targetApplicationVersion,
        Supplier<CompletionStage<Void>>
            onPreflightPassed) {
        Objects.requireNonNull(deadline, "deadline");
        Objects.requireNonNull(
            stopBeforeNextUnit, "stopBeforeNextUnit");
        Objects.requireNonNull(onPreflightPassed, "onPreflightPassed");
        var targetPolicy = new ZLinkRelocationTargetPolicy(
            mode,
            applicationVersion,
            maintenanceWave,
            targetApplicationVersion);
        ZLinkStoreCancellation cancellation = () ->
            !Instant.now().isBefore(deadline);
        List<String> spotIds = spots.activeUserSpotIds().stream()
            .sorted()
            .toList();
        HashSet<String> aggregateActors = new HashSet<>();
        for (String spotId : spotIds) {
            aggregateActors.addAll(spots.actorSessions().actorIdsInSpot(spotId));
        }
        ArrayList<String> actorIds = new ArrayList<>();
        for (String actorId : actors.activeActorIds()) {
            if (!aggregateActors.contains(actorId)) {
                actorIds.add(actorId);
            }
        }
        actorIds.sort(String::compareTo);
        var plan = new RelocationPlan(spotIds, List.copyOf(actorIds));
        return executePlan(
            plan,
            spotId -> preflightSpot(
                spotId, cancellation, targetPolicy),
            actorId -> preflightActor(
                actorId, cancellation, targetPolicy),
            () -> {
                if (!plan.equals(snapshotPlan())) {
                    throw new RelocationBlockedException(
                        ZLinkFrameworkRelocationReason.OPERATION_IN_PROGRESS,
                        "Relocation workload changed during host preflight");
                }
                return onPreflightPassed.get();
            },
            stopBeforeNextUnit,
            spotId -> relocateOne(
                spotId, deadline, cancellation, targetPolicy),
            actorId -> relocateActor(
                actorId, deadline, cancellation, targetPolicy));
    }

    private RelocationPlan snapshotPlan() {
        List<String> spotIds = spots.activeUserSpotIds().stream()
            .sorted()
            .toList();
        HashSet<String> aggregateActors = new HashSet<>();
        for (String spotId : spotIds) {
            aggregateActors.addAll(spots.actorSessions().actorIdsInSpot(spotId));
        }
        ArrayList<String> actorIds = new ArrayList<>();
        for (String actorId : actors.activeActorIds()) {
            if (!aggregateActors.contains(actorId)) {
                actorIds.add(actorId);
            }
        }
        actorIds.sort(String::compareTo);
        return new RelocationPlan(spotIds, List.copyOf(actorIds));
    }

    static CompletionStage<Void> executePlan(
        RelocationPlan plan,
        UnitOperation spotPreflight,
        UnitOperation actorPreflight,
        Runnable onPreflightPassed,
        BooleanSupplier stopBeforeNextUnit,
        UnitOperation spotRelocation,
        UnitOperation actorRelocation) {
        Objects.requireNonNull(onPreflightPassed, "onPreflightPassed");
        return executePlan(
            plan,
            spotPreflight,
            actorPreflight,
            () -> {
                onPreflightPassed.run();
                return CompletableFuture.completedFuture(null);
            },
            stopBeforeNextUnit,
            spotRelocation,
            actorRelocation);
    }

    static CompletionStage<Void> executePlan(
        RelocationPlan plan,
        UnitOperation spotPreflight,
        UnitOperation actorPreflight,
        Supplier<CompletionStage<Void>>
            onPreflightPassed,
        BooleanSupplier stopBeforeNextUnit,
        UnitOperation spotRelocation,
        UnitOperation actorRelocation) {
        Objects.requireNonNull(plan, "plan");
        Objects.requireNonNull(spotPreflight, "spotPreflight");
        Objects.requireNonNull(actorPreflight, "actorPreflight");
        Objects.requireNonNull(onPreflightPassed, "onPreflightPassed");
        Objects.requireNonNull(stopBeforeNextUnit, "stopBeforeNextUnit");
        Objects.requireNonNull(spotRelocation, "spotRelocation");
        Objects.requireNonNull(actorRelocation, "actorRelocation");
        CompletionStage<Void> preflight =
            CompletableFuture.completedFuture(null);
        for (String spotId : plan.spotIds()) {
            preflight = preflight.thenCompose(
                ignored -> spotPreflight.execute(spotId));
        }
        for (String actorId : plan.actorIds()) {
            preflight = preflight.thenCompose(
                ignored -> actorPreflight.execute(actorId));
        }
        return preflight.thenCompose(ignored -> {
            CompletionStage<Void> admission = Objects.requireNonNull(
                onPreflightPassed.get(),
                "onPreflightPassed result");
            return admission.thenCompose(admissionIgnored -> {
                //  Every unit starts here. The spec unit gate of
                //  30-host-relocation-flow §7 (outbound 64, inbound 64,
                ArrayList<CompletionStage<Void>> started =
                    new ArrayList<>(
                        plan.spotIds().size() + plan.actorIds().size());
                for (String spotId : plan.spotIds()) {
                    started.add(startUnit(
                        stopBeforeNextUnit,
                        () -> spotRelocation.execute(spotId)));
                }
                for (String actorId : plan.actorIds()) {
                    started.add(startUnit(
                        stopBeforeNextUnit,
                        () -> actorRelocation.execute(actorId)));
                }
                return awaitAllUnits(started);
            });
        });
    }

    //  Shutdown is re-checked as the unit starts, not when the plan was built.
    private static CompletionStage<Void> startUnit(
        BooleanSupplier stopBeforeNextUnit,
        Supplier<CompletionStage<Void>> relocation) {
        if (stopBeforeNextUnit.getAsBoolean()) {
            return shutdownRequested();
        }
        try {
            return Objects.requireNonNull(
                relocation.get(), "relocation action result");
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    //  Mirrors Task.WhenAll: every unit runs to a terminal state and the first
    //  failure is the reported one. Later failures are attached to it.
    private static CompletionStage<Void> awaitAllUnits(
        List<CompletionStage<Void>> units) {
        if (units.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        AtomicReference<Throwable> firstFailure =
            new AtomicReference<>();
        CompletableFuture<?>[] settled = units.stream()
            .map(unit -> unit.toCompletableFuture()
                .handle((ignored, failure) -> {
                    if (failure != null
                        && !firstFailure.compareAndSet(null, failure)) {
                        Throwable first = firstFailure.get();
                        if (first != failure) {
                            first.addSuppressed(failure);
                        }
                    }
                    return null;
                }))
            .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(settled).thenCompose(ignored -> {
            Throwable failure = firstFailure.get();
            return failure == null
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.<Void>failedFuture(failure);
        });
    }

    private static CompletionStage<Void> shutdownRequested() {
        return CompletableFuture.failedFuture(new RelocationBlockedException(
            ZLinkFrameworkRelocationReason.SHUTDOWN_REQUESTED,
            "Shutdown was requested during relocation"));
    }

    private CompletionStage<Void> preflightSpot(
        String spotId,
        ZLinkStoreCancellation cancellation,
        ZLinkRelocationTargetPolicy targetPolicy) {
        String meshName = spots.userSpotMeshName(spotId);
        Lane lane = lanes.get(meshName);
        if (lane == null) {
            return CompletableFuture.failedFuture(new RelocationBlockedException(
                ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE,
                "User Spot Retire lane is unavailable: " + meshName));
        }
        return lane.source().preflight(spotId, targetPolicy, cancellation);
    }

    private CompletionStage<Void> preflightActor(
        String actorId,
        ZLinkStoreCancellation cancellation,
        ZLinkRelocationTargetPolicy targetPolicy) {
        String meshName = spots.actorSessions().actorMeshName(actorId);
        ActorLane lane = actorLanes.get(meshName);
        if (lane == null) {
            return CompletableFuture.failedFuture(new RelocationBlockedException(
                ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE,
                "Entry Spot Actor relocation lane is unavailable: "
                    + meshName));
        }
        return lane.source().preflight(actorId, targetPolicy, cancellation);
    }

    private CompletionStage<Void> relocateOne(
        String spotId,
        Instant deadline,
        ZLinkStoreCancellation cancellation,
        ZLinkRelocationTargetPolicy targetPolicy) {
        String meshName = spots.userSpotMeshName(spotId);
        Lane lane = lanes.get(meshName);
        if (lane == null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "User Spot Retire lane is unavailable: " + meshName));
        }
        Duration timeout = Duration.between(Instant.now(), deadline);
        if (timeout.isZero() || timeout.isNegative()) {
            return CompletableFuture.failedFuture(new java.util.concurrent
                .TimeoutException("User Spot Retire deadline elapsed"));
        }
        return lane.source().prepare(spotId, targetPolicy, cancellation)
            .thenCompose(source -> {
                requireExactCoreReady(
                    lane.node(), source.stageRequest().targetNodeRid(),
                    source.stageRequest().targetNodeGeneration());
                return lane.scheduler().executeRemote(
                    new ZLinkUserSpotRetireScheduler.RemoteRequest(
                        source,
                        lane.client(),
                        timeout.compareTo(CONTROL_TIMEOUT) < 0
                            ? timeout : CONTROL_TIMEOUT,
                        () -> source.cleanupLocal(deadline)),
                    cancellation);
            })
            .thenApply(ignored -> null);
    }

    private CompletionStage<Void> relocateActor(
        String actorId,
        Instant deadline,
        ZLinkStoreCancellation cancellation,
        ZLinkRelocationTargetPolicy targetPolicy) {
        String meshName = spots.actorSessions().actorMeshName(actorId);
        ActorLane lane = actorLanes.get(meshName);
        if (lane == null) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "Entry Spot Actor relocation lane is unavailable: "
                + meshName));
        }
        Duration timeout = Duration.between(Instant.now(), deadline);
        if (timeout.isZero() || timeout.isNegative()) {
            return CompletableFuture.failedFuture(new java.util.concurrent
                .TimeoutException("Actor relocation deadline elapsed"));
        }
        return lane.source().prepare(actorId, targetPolicy, cancellation)
            .thenCompose(source -> {
                requireExactCoreReady(
                    lane.node(), source.stageRequest().targetNodeRid(),
                    source.stageRequest().targetNodeGeneration());
                return lane.scheduler().executeRemote(
                    source,
                    lane.client(),
                    timeout.compareTo(CONTROL_TIMEOUT) < 0
                        ? timeout : CONTROL_TIMEOUT,
                    cancellation);
            });
    }

    private record Lane(
        ZLinkUserSpotRetireSourceBuilder source,
        ZLinkUserSpotRetireScheduler scheduler,
        ZLinkRelocationTransitionClient client,
        ZLinkInternalMeshNode node) {
    }

    private record ActorLane(
        ZLinkStandaloneActorRelocationSourceBuilder source,
        ZLinkStandaloneActorRelocationScheduler scheduler,
        ZLinkRelocationTransitionClient client,
        ZLinkInternalMeshNode node) {
    }

    private static void requireExactCoreReady(
        ZLinkInternalMeshNode node,
        systems.zlink.contracts.core.RoutingId targetRid,
        long targetGeneration) {
        boolean ready = node.peers().stream().anyMatch(peer ->
            peer.routingId().equals(targetRid)
                && peer.lifecycleGeneration()
                    == targetGeneration
                && peer.state()
                    == systems.zlink.framework.runtime.internal.binding.spot
                        .MeshPeerState.ADMITTED);
        if (!ready) {
            throw new RelocationBlockedException(
                ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE,
                "Relocation target is not Core-ready: " + targetRid);
        }
    }

    record RelocationPlan(
        List<String> spotIds,
        List<String> actorIds) {
        RelocationPlan {
            spotIds = List.copyOf(spotIds);
            actorIds = List.copyOf(actorIds);
        }
    }

    @FunctionalInterface
    interface UnitOperation {
        CompletionStage<Void> execute(String objectId);
    }

}
