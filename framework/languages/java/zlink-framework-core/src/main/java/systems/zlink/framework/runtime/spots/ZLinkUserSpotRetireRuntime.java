package systems.zlink.framework.runtime.spots;

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
import systems.zlink.framework.runtime.internal.locations.ZLinkMeshNodeDescriptor;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationPermitPool;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkRelocationStartupScanner;
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
    private final java.util.Optional<String> maintenanceWave;
    private final Map<String, Lane> lanes;
    private final Map<String, ActorLane> actorLanes;
    private final ZLinkRelocationStartupScanner startupScanner;
    private final List<ZLinkCanonicalRelocationStateMachine> recoveryOwners;

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
        java.util.Optional<String> maintenanceWave) {
        this.spots = Objects.requireNonNull(spots, "spots");
        this.actors = Objects.requireNonNull(actors, "actors");
        if (applicationVersion < 0) {
            throw new IllegalArgumentException(
                "applicationVersion must not be negative");
        }
        this.applicationVersion = applicationVersion;
        this.maintenanceWave = Objects.requireNonNull(
            maintenanceWave, "maintenanceWave");
        Objects.requireNonNull(registrations, "registrations");
        Objects.requireNonNull(nodes, "nodes");
        var coordinator = new ZLinkAggregateRelocationCoordinator(
            Objects.requireNonNull(authorities, "authorities"),
            Objects.requireNonNull(relocationStore, "relocationStore"));
        startupScanner = new ZLinkRelocationStartupScanner(
            authorities, relocationStore);
        var permits = new ZLinkRelocationPermitPool(
            Objects.requireNonNull(options, "options"));
        LinkedHashMap<String, Lane> configured = new LinkedHashMap<>();
        LinkedHashMap<String, ActorLane> configuredActors =
            new LinkedHashMap<>();
        java.util.ArrayList<ZLinkCanonicalRelocationStateMachine>
            configuredRecoveryOwners = new java.util.ArrayList<>();
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
                    .collect(java.util.stream.Collectors.toUnmodifiableMap(
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
                    .collect(java.util.stream.Collectors.toUnmodifiableMap(
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
                request -> coordinator.normalizeCompletedAggregate(
                        request.participants().stream()
                            .map(value -> new ZLinkAggregateRelocationCoordinator
                                .ExpectedParticipant(
                                    value.authorityKey(),
                                    value.objectGeneration(),
                                    value.sourceAuthorityOwnerGeneration()))
                            .toList(),
                        new systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence(
                            request.fence().aggregateId(),
                            request.fence().aggregateGeneration()),
                        new systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken(
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
                    node.status().routingId(),
                    spots.actorSessions(),
                    adapters,
                    spots),
                permits);
            var relocationClient =
                new ZLinkCanonicalRelocationStateMachine(
                    node,
                    registration.meshName(),
                    registration.entrySpotId(),
                    locations,
                    coordinator,
                    target);
            configuredRecoveryOwners.add(relocationClient);
            new ZLinkCanonicalRelocationTransitionOwner(relocationClient)
                .install(node);
            node.setRelocationReplyRelayHandler(
                target::relayCanonicalReply);
            if (!relocatableSpots.isEmpty()) {
                configured.put(registration.meshName(), new Lane(
                    new ZLinkUserSpotRetireSourceBuilder(
                    registration.meshName(),
                    node.status().routingId(),
                    node.status().lifecycleGeneration(),
                    locations,
                    coordinator,
                    permits,
                    spots.spotLifecycle(),
                    spots.actorSessions(),
                    adapters,
                    relocatableSpots,
                    relocatableActors,
                    spots),
                new ZLinkUserSpotRetireScheduler(coordinator, staging),
                    relocationClient,
                    node));
            }
            if (!relocatableActors.isEmpty()) {
                configuredActors.put(
                    registration.meshName(),
                    new ActorLane(
                        new ZLinkStandaloneActorRelocationSourceBuilder(
                            registration.meshName(),
                            node.status().routingId(),
                            node.status().lifecycleGeneration(),
                            locations,
                            coordinator,
                            permits,
                            spots.actorSessions(),
                            adapters,
                            relocatableActors,
                            spots),
                        new ZLinkStandaloneActorRelocationScheduler(),
                        relocationClient,
                        node));
            }
        }
        lanes = Map.copyOf(configured);
        actorLanes = Map.copyOf(configuredActors);
        recoveryOwners = List.copyOf(configuredRecoveryOwners);
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
        if (recoveryOwners.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        return startupScanner.scan(() -> false)
            .thenCompose(candidates -> {
                CompletionStage<Void> chain =
                    CompletableFuture.completedFuture(null);
                for (var candidate : candidates) {
                    for (var owner : recoveryOwners) {
                        chain = chain.thenCompose(ignored ->
                            owner.recoverPublished(candidate));
                    }
                }
                return chain;
            });
    }

    public boolean requiresStartupRecovery() {
        return !recoveryOwners.isEmpty();
    }

    public boolean supportsActiveInventory() {
        java.util.HashSet<String> aggregateActors = new java.util.HashSet<>();
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
        java.util.function.Supplier<CompletionStage<Void>>
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
        java.util.HashSet<String> aggregateActors = new java.util.HashSet<>();
        for (String spotId : spotIds) {
            aggregateActors.addAll(spots.actorSessions().actorIdsInSpot(spotId));
        }
        java.util.ArrayList<String> actorIds = new java.util.ArrayList<>();
        for (String actorId : actors.activeActorIds()) {
            if (!aggregateActors.contains(actorId)) {
                actorIds.add(actorId);
            }
        }
        actorIds.sort(String::compareTo);
        var plan = new RelocationPlan(spotIds, List.copyOf(actorIds));
        var capacityPlan = new ZLinkRelocationCapacityPlan();
        return executePlan(
            plan,
            spotId -> preflightSpot(
                spotId, cancellation, targetPolicy, capacityPlan),
            actorId -> preflightActor(
                actorId, cancellation, targetPolicy, capacityPlan),
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
                spotId, deadline, cancellation, targetPolicy, capacityPlan),
            actorId -> relocateActor(
                actorId, deadline, cancellation, targetPolicy, capacityPlan));
    }

    private RelocationPlan snapshotPlan() {
        List<String> spotIds = spots.activeUserSpotIds().stream()
            .sorted()
            .toList();
        java.util.HashSet<String> aggregateActors = new java.util.HashSet<>();
        for (String spotId : spotIds) {
            aggregateActors.addAll(spots.actorSessions().actorIdsInSpot(spotId));
        }
        java.util.ArrayList<String> actorIds = new java.util.ArrayList<>();
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
        java.util.function.Supplier<CompletionStage<Void>>
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
                //  28-graceful-drain-handoff §7 (outbound 64, inbound 64,
                //  payload 256 MiB, capture and restore 8) belongs to
                //  ZLinkRelocationPermitPool, which admits at the turn
                //  boundary where the actual payload is known. Bounding the
                //  units a second time here would hold input-order slots
                //  across a turn wait and starve a unit that is already ready.
                java.util.ArrayList<CompletionStage<Void>> started =
                    new java.util.ArrayList<>(
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
        java.util.function.Supplier<CompletionStage<Void>> relocation) {
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
        java.util.concurrent.atomic.AtomicReference<Throwable> firstFailure =
            new java.util.concurrent.atomic.AtomicReference<>();
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
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan) {
        String meshName = spots.userSpotMeshName(spotId);
        Lane lane = lanes.get(meshName);
        if (lane == null) {
            return CompletableFuture.failedFuture(new RelocationBlockedException(
                ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE,
                "User Spot Retire lane is unavailable: " + meshName));
        }
        return lane.source().preflight(
            spotId, targetPolicy, capacityPlan, cancellation);
    }

    private CompletionStage<Void> preflightActor(
        String actorId,
        ZLinkStoreCancellation cancellation,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan) {
        String meshName = spots.actorSessions().actorMeshName(actorId);
        ActorLane lane = actorLanes.get(meshName);
        if (lane == null) {
            return CompletableFuture.failedFuture(new RelocationBlockedException(
                ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE,
                "Entry Spot Actor relocation lane is unavailable: "
                    + meshName));
        }
        return lane.source().preflight(
            actorId, targetPolicy, capacityPlan, cancellation);
    }

    private CompletionStage<Void> relocateOne(
        String spotId,
        Instant deadline,
        ZLinkStoreCancellation cancellation,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan) {
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
        ZLinkMeshNodeDescriptor pinned =
            capacityPlan.userSpotTarget(spotId);
        requireExactCoreReady(lane.node(), pinned);
        return lane.source().preparePinned(
                spotId,
                targetPolicy,
                pinned,
                cancellation)
            .thenCompose(source -> lane.scheduler().executeRemote(
                new ZLinkUserSpotRetireScheduler.RemoteRequest(
                    source,
                    lane.client(),
                    timeout.compareTo(CONTROL_TIMEOUT) < 0
                        ? timeout : CONTROL_TIMEOUT,
                    () -> source.cleanupLocal(deadline),
                    source.stagedRoot().request().root()),
                cancellation))
            .thenApply(ignored -> null);
    }

    private CompletionStage<Void> relocateActor(
        String actorId,
        Instant deadline,
        ZLinkStoreCancellation cancellation,
        ZLinkRelocationTargetPolicy targetPolicy,
        ZLinkRelocationCapacityPlan capacityPlan) {
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
        ZLinkMeshNodeDescriptor pinned = capacityPlan.actorTarget(actorId);
        requireExactCoreReady(lane.node(), pinned);
        return lane.source().preparePinned(
                actorId,
                targetPolicy,
                pinned,
                cancellation)
            .thenCompose(source -> lane.scheduler().executeRemote(
                source,
                lane.client(),
                timeout.compareTo(CONTROL_TIMEOUT) < 0
                    ? timeout : CONTROL_TIMEOUT,
                cancellation));
    }

    private record Lane(
        ZLinkUserSpotRetireSourceBuilder source,
        ZLinkUserSpotRetireScheduler scheduler,
        ZLinkRelocationTransitionClient client,
        systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode node) {
    }

    private record ActorLane(
        ZLinkStandaloneActorRelocationSourceBuilder source,
        ZLinkStandaloneActorRelocationScheduler scheduler,
        ZLinkRelocationTransitionClient client,
        systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode node) {
    }

    private static void requireExactCoreReady(
        systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode node,
        ZLinkMeshNodeDescriptor target) {
        boolean ready = node.peers().stream().anyMatch(peer ->
            peer.routingId().equals(target.rid())
                && peer.lifecycleGeneration()
                    == target.lifecycleGeneration()
                && peer.state()
                    == systems.zlink.framework.runtime.internal.binding.spot
                        .MeshPeerState.ADMITTED);
        if (!ready) {
            throw new RelocationBlockedException(
                ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE,
                "Pinned relocation target is not Core-ready: "
                    + target.rid());
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
