package systems.zlink.framework.runtime.spots;

import static systems.zlink.framework.runtime.spots.SpotActivationBase.unwrapCompletion;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.actors.ZLinkSessionRelocationPeerClient;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationMode;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRelocationReason;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorRef;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.configuration.ZLinkObjectFactoryRegistration.RelocatableActorFactory;
import systems.zlink.framework.runtime.internal.configuration.ZLinkObjectFactoryRegistration.RelocatableSpotFactory;
import systems.zlink.framework.runtime.internal.configuration.ZLinkObjectFactoryRegistration.RelocationPolicy;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateFence;
import systems.zlink.framework.runtime.internal.locations.ZLinkAggregateRelocationCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationOwnerToken;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.locations.ZLinkStoreCancellation;
import systems.zlink.framework.runtime.internal.relocation.ZLinkRelocationAdapterRegistry;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6BWireCodec;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;

import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashSet;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.TimeoutException;
import java.util.concurrent.atomic.AtomicInteger;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.BooleanSupplier;
import java.util.function.Supplier;
import java.util.stream.Collectors;

/** Process-wide production bridge from host Retire to per-Mesh Spot units. */
public final class ZLinkUserSpotRetireRuntime {
    private static final Duration CONTROL_TIMEOUT = Duration.ofSeconds(5);

    private final ZLinkSpotRuntime spots;
    private final ZLinkActorRuntime actors;
    private final long applicationVersion;
    private final Optional<String> maintenanceWave;
    private final Duration sessionRelocationSealTimeout;
    private final List<MeshNodeRegistration> registrations;
    private final Map<String, Lane> lanes;
    private final Map<String, ActorLane> actorLanes;
    final List<ZLinkCanonicalRelocationStateMachine> stateMachines;

    public static class RelocationBlockedException extends IllegalStateException {
        private final ZLinkFrameworkRelocationReason reason;

        public RelocationBlockedException(ZLinkFrameworkRelocationReason reason, String message) {
            super(message);
            this.reason = Objects.requireNonNull(reason, "reason");
        }

        public ZLinkFrameworkRelocationReason reason() {
            return reason;
        }
    }

    static void requireReleasedPublication(
            systems.zlink.framework.runtime.internal.locations.ZLinkAuthoritySnapshot snapshot) {
        if (snapshot.relocation() != null && snapshot.relocation().targetPublicationPending()) {
            throw new RelocationBlockedException(
                    ZLinkFrameworkRelocationReason.OPERATION_IN_PROGRESS,
                    "Authority has an in-progress relocation publication: "
                            + snapshot.relocation().relocationId());
        }
    }

    /**
     * A relocation whose settled unit authority leaves the host split — some units committed on the
     * target while others stayed on the source — or whose source owner lease expired before the
     * {@code Preserve} fence. The host enters {@code Error} with {@code Blocked/RelocationFailed}
     * (spec 30 §13).
     */
    public static final class RelocationAuthorityErrorException extends RelocationBlockedException {
        public RelocationAuthorityErrorException(String message) {
            super(ZLinkFrameworkRelocationReason.RELOCATION_FAILED, message);
        }
    }

    public ZLinkUserSpotRetireRuntime(
            ZLinkSpotRuntime spots,
            ZLinkActorRuntime actors,
            List<MeshNodeRegistration> registrations,
            Map<String, ZLinkInternalMeshNode> nodes,
            ZLinkLocationRepository locations,
            ZLinkStoreLocationResolvers locationResolvers,
            ZLinkLocationRepository authorities,
            ZLinkLocationOptions options,
            ZLinkRelocationAdapterRegistry adapters,
            long applicationVersion,
            Optional<String> maintenanceWave,
            Duration sessionRelocationSealTimeout,
            java.util.function.Predicate<String> targetAdmissionSealed) {
        this(
                spots,
                actors,
                registrations,
                nodes,
                locations,
                locationResolvers,
                authorities,
                options,
                adapters,
                applicationVersion,
                maintenanceWave,
                sessionRelocationSealTimeout,
                targetAdmissionSealed,
                null,
                null,
                null);
    }

    ZLinkUserSpotRetireRuntime(
            ZLinkSpotRuntime spots,
            ZLinkActorRuntime actors,
            List<MeshNodeRegistration> registrations,
            Map<String, ZLinkInternalMeshNode> nodes,
            ZLinkLocationRepository locations,
            ZLinkStoreLocationResolvers locationResolvers,
            ZLinkLocationRepository authorities,
            ZLinkLocationOptions options,
            ZLinkRelocationAdapterRegistry adapters,
            long applicationVersion,
            Optional<String> maintenanceWave,
            Duration sessionRelocationSealTimeout,
            java.util.function.Predicate<String> targetAdmissionSealed,
            List<?> injectedStateMachines,
            java.util.function.UnaryOperator<ZLinkLocationRepository>
                    aggregateRelocationAuthorityStoreDecorator,
            java.util.function.UnaryOperator<
                            java.util.function.Function<Object, CompletionStage<Void>>>
                    targetNormalizerDecorator) {
        this.spots = Objects.requireNonNull(spots, "spots");
        Objects.requireNonNull(targetAdmissionSealed, "targetAdmissionSealed");
        this.actors = Objects.requireNonNull(actors, "actors");
        Objects.requireNonNull(locationResolvers, "locationResolvers");
        if (applicationVersion < 0) {
            throw new IllegalArgumentException("applicationVersion must not be negative");
        }
        this.applicationVersion = applicationVersion;
        this.maintenanceWave = Objects.requireNonNull(maintenanceWave, "maintenanceWave");
        if (sessionRelocationSealTimeout == null
                || sessionRelocationSealTimeout.isZero()
                || sessionRelocationSealTimeout.isNegative()) {
            throw new IllegalArgumentException("session relocation seal timeout must be positive");
        }
        this.sessionRelocationSealTimeout = sessionRelocationSealTimeout;
        this.registrations = List.copyOf(registrations);
        Objects.requireNonNull(nodes, "nodes");
        var coordinator =
                new ZLinkAggregateRelocationCoordinator(
                        aggregateRelocationAuthorityStoreDecorator == null
                                ? Objects.requireNonNull(authorities, "authorities")
                                : Objects.requireNonNull(
                                        aggregateRelocationAuthorityStoreDecorator.apply(
                                                Objects.requireNonNull(authorities, "authorities")),
                                        "decorated authorities"),
                        spots::isOwnerLeaseValid);
        Objects.requireNonNull(options, "options");
        var transferOptions =
                new ZLinkRelocationPayloadTransfer.Options(
                        options.relocationPayloadChunkLimitBytes(),
                        options.relocationInFlightPayloadBudgetBytes(),
                        options.relocationNodeInFlightPayloadBudgetBytes(),
                        options.relocationCutoverWaitTimeout(),
                        options.messageFollowDuration());
        var machines = new ArrayList<ZLinkCanonicalRelocationStateMachine>();
        LinkedHashMap<String, Lane> configured = new LinkedHashMap<>();
        LinkedHashMap<String, ActorLane> configuredActors = new LinkedHashMap<>();
        var actorJoin = new ZLinkActorJoinCanonicalAdapter(actors, spots);
        for (MeshNodeRegistration registration : this.registrations) {
            ZLinkInternalMeshNode node = nodes.get(registration.meshName());
            Map<String, RelocatableSpotFactory<?>> relocatableSpots =
                    registration.relocatableSpotFactories().entrySet().stream()
                            .filter(
                                    entry ->
                                            !(entry.getValue().relocationPolicy()
                                                    instanceof RelocationPolicy.Disabled))
                            .collect(
                                    Collectors.toUnmodifiableMap(
                                            Map.Entry::getKey, Map.Entry::getValue));
            Map<String, RelocatableActorFactory<?>> relocatableActors =
                    registration.relocatableActorFactories().entrySet().stream()
                            .filter(
                                    entry ->
                                            !(entry.getValue().relocationPolicy()
                                                    instanceof RelocationPolicy.Disabled))
                            .collect(
                                    Collectors.toUnmodifiableMap(
                                            Map.Entry::getKey, Map.Entry::getValue));
            var instanceFactories =
                    registration.relocatableInstanceSpotFactories().entrySet().stream()
                            .filter(
                                    entry ->
                                            !(entry.getValue().relocationPolicy()
                                                    instanceof RelocationPolicy.Disabled))
                            .collect(
                                    Collectors.toUnmodifiableMap(
                                            Map.Entry::getKey, Map.Entry::getValue));
            boolean relocatesSpots = !relocatableSpots.isEmpty() || !instanceFactories.isEmpty();
            if (node == null || !relocatesSpots && relocatableActors.isEmpty()) {
                continue;
            }
            var staging =
                    new ZLinkUserSpotAggregateStagingOwner(
                            spots, adapters, registration.meshName());
            var peerClient = new ZLinkSessionRelocationPeerClient(node);
            var relocationReplyClient = ZLinkSpotRetireControl.client(node);
            java.util.function.Function<Object, CompletionStage<Void>> normalize =
                    value -> {
                        ZLinkSpotRetireControl.StageRequest request =
                                (ZLinkSpotRetireControl.StageRequest) value;
                        return coordinator.normalizePublishedAggregate(
                                request.participants().stream()
                                        .map(
                                                participant ->
                                                        new ZLinkAggregateRelocationCoordinator
                                                                .ExpectedParticipant(
                                                                participant.authorityKey(),
                                                                participant.objectGeneration(),
                                                                participant
                                                                        .sourceAuthorityOwnerGeneration()))
                                        .toList(),
                                new ZLinkAggregateFence(
                                        request.fence().aggregateId(),
                                        request.fence().aggregateGeneration()),
                                new ZLinkLocationOwnerToken(
                                        request.targetOwnerId(),
                                        request.targetOwnerLeaseGeneration()),
                                () -> false);
                    };
            var normalized =
                    targetNormalizerDecorator == null
                            ? normalize
                            : targetNormalizerDecorator.apply(normalize);
            var normalizer =
                    (ZLinkUserSpotRetireTargetEndpoint.SteadyNormalizer)
                            request -> normalized.apply(request);
            var target =
                    new ZLinkUserSpotRetireTargetEndpoint(
                            node.status().routingId(),
                            node.status().lifecycleGeneration(),
                            coordinator,
                            staging,
                            (kind, stableType) ->
                                    kind == ZLinkPlacementObjectKind.INSTANCE_SPOT
                                            ? Optional.ofNullable(instanceFactories.get(stableType))
                                                    .map(factory -> factory.spotType())
                                                    .orElse(null)
                                            : relocatableSpots.containsKey(stableType)
                                                    ? relocatableSpots.get(stableType).spotType()
                                                    : null,
                            null,
                            peerClient,
                            CONTROL_TIMEOUT,
                            normalizer,
                            spots,
                            relocationReplyClient,
                            locations,
                            new ZLinkStandaloneActorRelocationStagingOwner(
                                    node.spotNode(),
                                    spots.actorSessions(),
                                    adapters,
                                    spots,
                                    registration.meshName()),
                            actorJoin,
                            (request, generations) ->
                                    installBoundSessions(node.spotNode(), request, generations));
            var relocationClient =
                    new ZLinkCanonicalRelocationStateMachine(
                            node,
                            registration.meshName(),
                            registration.entrySpotId(),
                            locations,
                            coordinator,
                            target,
                            spots::scheduleRelocationCleanup,
                            transferOptions,
                            () -> targetAdmissionSealed.test(registration.meshName()),
                            spots.dispatchErrors);
            machines.add(relocationClient);
            new ZLinkCanonicalRelocationTransitionOwner(relocationClient).install(node);
            node.spotNode().setRelocationStagingIngressHandler(target);
            if (relocatesSpots) {
                configured.put(
                        registration.meshName(),
                        new Lane(
                                new ZLinkUserSpotRetireSourceBuilder(
                                        registration.meshName(),
                                        node.status().routingId(),
                                        node.status().lifecycleGeneration(),
                                        locations,
                                        locationResolvers,
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
                                locationResolvers,
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
                actorJoin.register(node.status().routingId(), node, actorSource, relocationClient);
            }
        }
        lanes = Map.copyOf(configured);
        actorLanes = Map.copyOf(configuredActors);
        stateMachines =
                injectedStateMachines == null
                        ? List.copyOf(machines)
                        : injectedStateMachines.stream()
                                .map(ZLinkCanonicalRelocationStateMachine.class::cast)
                                .toList();
        if (!actorLanes.isEmpty()) {
            actors.setActorJoinRelocationPort(actorJoin);
        }
    }

    static CompletionStage<Void> installBoundSessions(
            ZLinkInternalSpotNode node,
            ZLinkSpotRetireControl.StageRequest request,
            Map<String, Long> actorOwnerGenerations) {
        List<CompletableFuture<Void>> installed = new ArrayList<>();
        for (var route : request.sessionRoutes()) {
            installed.add(
                    node.installRelocatingActorBoundSession(
                                    new ZLinkServiceM6BWireCodec.ActorRouteFence(
                                            new ZLinkBackendActorRef(
                                                    request.targetNodeRid(),
                                                    route.actorId(),
                                                    route.actorObjectGeneration()),
                                            request.targetNodeGeneration(),
                                            Objects.requireNonNull(
                                                    actorOwnerGenerations.get(route.actorId()),
                                                    "committed Actor owner generation"),
                                            request.targetOwnerLeaseGeneration()),
                                    new ZLinkServiceM6BWireCodec.SessionOwnerFence(
                                            route.sessionOwnerNodeRid(),
                                            route.sessionOwnerNodeGeneration(),
                                            route.sessionOwnerId(),
                                            route.sessionOwnerLeaseGeneration(),
                                            route.sessionRid(),
                                            route.bindingGeneration()))
                            .toCompletableFuture());
        }
        return CompletableFuture.allOf(installed.toArray(CompletableFuture[]::new));
    }

    /**
     * True when every source-side relocation unit has passed its Message Follow route removal point
     * (S4) and its cutover retransmission window — the source-local SafeToShutdown condition (spec
     * 30 §11).
     */
    public CompletionStage<Void> renewReplyRows(Instant storeNow) {
        return CompletableFuture.allOf(
                stateMachines.stream()
                        .map(machine -> machine.renewReplyRows(storeNow).toCompletableFuture())
                        .toArray(CompletableFuture[]::new));
    }

    public boolean relocationSourceQuiescent() {
        return stateMachines.stream()
                .allMatch(ZLinkCanonicalRelocationStateMachine::sourceQuiescent);
    }

    public void onSourceQuiescenceChanged(Runnable listener) {
        stateMachines.forEach(machine -> machine.onSourceQuiescenceChanged(listener));
    }

    public CompletionStage<Void> sealAcceptedTargetRelocations() {
        return CompletableFuture.allOf(
                stateMachines.stream()
                        .map(ZLinkCanonicalRelocationStateMachine::sealAcceptedTargetRelocations)
                        .map(CompletionStage::toCompletableFuture)
                        .toArray(CompletableFuture[]::new));
    }

    public CompletionStage<Void> startup() {
        return CompletableFuture.completedFuture(null);
    }

    public boolean requiresStartupRecovery() {
        return false;
    }

    public CompletionStage<Void> relocateAll(
            Instant deadline,
            BooleanSupplier stopBeforeNextUnit,
            ZLinkFrameworkRelocationMode mode,
            long targetApplicationVersion,
            Supplier<CompletionStage<Void>> targetPopulationPreflight,
            Supplier<CompletionStage<Void>> onPreflightPassed,
            CompletionStage<?> cancellationSignal) {
        Objects.requireNonNull(deadline, "deadline");
        Objects.requireNonNull(cancellationSignal, "cancellationSignal");
        Objects.requireNonNull(stopBeforeNextUnit, "stopBeforeNextUnit");
        Objects.requireNonNull(targetPopulationPreflight, "targetPopulationPreflight");
        Objects.requireNonNull(onPreflightPassed, "onPreflightPassed");
        var targetPolicy =
                new ZLinkRelocationTargetPolicy(
                        mode, applicationVersion, maintenanceWave, targetApplicationVersion);
        ZLinkStoreCancellation cancellation = () -> !Instant.now().isBefore(deadline);
        RelocationInventory inventory;
        try {
            inventory = snapshotInventory(spots, actors, registrations);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
        RelocationPlan<ZLinkSpotRelocationUnit> plan = inventory.plan();
        return inventory
                .preflightTargets(targetPopulationPreflight)
                .thenCompose(
                        populated ->
                                executePlan(
                                        plan,
                                        spotId -> preflightSpot(spotId, cancellation, targetPolicy),
                                        actorId ->
                                                preflightActor(actorId, cancellation, targetPolicy),
                                        () -> {
                                            if (!inventory.matches(
                                                    snapshotInventory(
                                                            spots, actors, registrations))) {
                                                throw new RelocationBlockedException(
                                                        ZLinkFrameworkRelocationReason
                                                                .OPERATION_IN_PROGRESS,
                                                        "Relocation workload changed during host"
                                                                + " preflight");
                                            }
                                            return onPreflightPassed.get();
                                        },
                                        stopBeforeNextUnit,
                                        spotId ->
                                                relocateOne(
                                                        spotId,
                                                        deadline,
                                                        cancellation,
                                                        targetPolicy,
                                                        cancellationSignal),
                                        actorId ->
                                                relocateActor(
                                                                actorId,
                                                                deadline,
                                                                cancellation,
                                                                targetPolicy)
                                                        .thenApply(ignored -> true),
                                        () -> closeSourceShells(plan.shellSpotIds(), deadline)));
    }

    public static CompletionStage<Void> preflightWithoutRelocationRuntime(
            ZLinkSpotRuntime spots,
            ZLinkActorRuntime actors,
            List<MeshNodeRegistration> registrations,
            Supplier<CompletionStage<Void>> targetPopulationPreflight) {
        try {
            var inventory = snapshotInventory(spots, actors, registrations);
            if (!inventory.plan().isEmpty()) {
                throw new RelocationBlockedException(
                        ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE,
                        "Host relocation runtime is unavailable for the active workload");
            }
            return inventory.preflightTargets(targetPopulationPreflight);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    private static RelocationInventory snapshotInventory(
            ZLinkSpotRuntime spots,
            ZLinkActorRuntime actors,
            List<MeshNodeRegistration> registrations) {
        var shellSpotIds = new ArrayList<ZLinkSpotRelocationUnit>();
        var aggregateSpotIds = new ArrayList<ZLinkSpotRelocationUnit>();
        var aggregateActors = new HashSet<String>();
        if (spots != null) {
            for (SpotActivation activation : spots.spotLifecycle().userSpotActivations()) {
                String spotId = activation.context.spotId();
                var factory =
                        registrations.stream()
                                .filter(
                                        registration ->
                                                registration
                                                        .meshName()
                                                        .equals(activation.meshName()))
                                .flatMap(
                                        registration ->
                                                registration
                                                        .relocatableSpotFactories()
                                                        .values()
                                                        .stream())
                                .filter(
                                        value ->
                                                value.spotType()
                                                        .equals(activation.spot().getClass()))
                                .findFirst();
                requireRelocationPolicy(
                        spotId,
                        factory.map(RelocatableSpotFactory::relocationPolicy),
                        registrations);
                var registration = factory.orElseThrow();
                var unit =
                        new ZLinkSpotRelocationUnit(
                                spotId,
                                activation.meshName(),
                                ZLinkPlacementObjectKind.USER_SPOT,
                                registration.stableType(),
                                registration.spotType(),
                                registration.relocationPolicy(),
                                activation);
                if (activation.context.executionMode() == ZLinkUserSpotExecutionMode.PER_ACTOR) {
                    shellSpotIds.add(unit);
                } else {
                    aggregateSpotIds.add(unit);
                    aggregateActors.addAll(spots.actorSessions().actorIdsInSpot(spotId));
                }
            }
            // Close owns settlement once it commits (Host relocation §12).
            for (var activation : spots.instanceSpotActivations.values()) {
                if (activation.hasCommittedClose() || !activation.hasExpectedNodeGeneration())
                    continue;
                var context = activation.context;
                var factory =
                        registrations.stream()
                                .filter(
                                        registration ->
                                                registration.meshName().equals(context.meshName()))
                                .flatMap(
                                        registration ->
                                                registration
                                                        .relocatableInstanceSpotFactories()
                                                        .values()
                                                        .stream())
                                .filter(
                                        value ->
                                                value.spotType()
                                                        .equals(activation.spotSurface.getClass()))
                                .findFirst();
                requireRelocationPolicy(
                        context.spotId(),
                        factory.map(value -> value.relocationPolicy()),
                        registrations);
                var registration = factory.orElseThrow();
                aggregateSpotIds.add(
                        new ZLinkSpotRelocationUnit(
                                context.spotId(),
                                context.meshName(),
                                ZLinkPlacementObjectKind.INSTANCE_SPOT,
                                registration.stableType(),
                                registration.spotType(),
                                registration.relocationPolicy(),
                                activation));
            }
        }
        shellSpotIds.sort(Comparator.comparing(ZLinkSpotRelocationUnit::id));
        aggregateSpotIds.sort(Comparator.comparing(ZLinkSpotRelocationUnit::id));
        List<String> activeActors = actors == null ? List.of() : actors.activeActorIds();
        List<String> actorIds =
                activeActors.stream()
                        .filter(actorId -> !aggregateActors.contains(actorId))
                        .toList();
        for (String actorId : activeActors) {
            String actorType = spots.actorSessions().actorType(actorId);
            String mesh = spots.actorSessions().actorMeshName(actorId);
            var policy =
                    registrations.stream()
                            .filter(registration -> registration.meshName().equals(mesh))
                            .map(
                                    registration ->
                                            registration.relocatableActorFactories().get(actorType))
                            .filter(Objects::nonNull)
                            .map(RelocatableActorFactory::relocationPolicy)
                            .findFirst();
            requireRelocationPolicy(actorId, policy, registrations);
        }
        return new RelocationInventory(
                new RelocationPlan<>(shellSpotIds, actorIds, aggregateSpotIds));
    }

    private static void requireRelocationPolicy(
            String objectId,
            Optional<RelocationPolicy> policy,
            List<MeshNodeRegistration> registrations) {
        if (policy.isEmpty()) {
            boolean hasRelocatableFactories =
                    registrations.stream().anyMatch(MeshNodeRegistration::hasRelocatableFactories);
            throw new RelocationBlockedException(
                    hasRelocatableFactories
                            ? ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE
                            : ZLinkFrameworkRelocationReason.RELOCATION_DISABLED,
                    "No relocation policy is registered for " + objectId);
        }
        checkDisabledRelocation(objectId, policy.orElseThrow());
    }

    private static void checkDisabledRelocation(String objectId, RelocationPolicy policy) {
        if (policy instanceof RelocationPolicy.Disabled) {
            throw new RelocationBlockedException(
                    ZLinkFrameworkRelocationReason.RELOCATION_DISABLED,
                    "Relocation is disabled for " + objectId);
        }
    }

    private CompletionStage<Void> closeSourceShells(
            List<ZLinkSpotRelocationUnit> shellSpotIds, Instant deadline) {
        return ZLinkRelocationHandOff.completeSourceCleanup(
                shellSpotIds.stream()
                        .<Supplier<? extends CompletionStage<Void>>>map(
                                spotId ->
                                        () ->
                                                spots.spotLifecycle()
                                                        .completeRelocatedShellSource(
                                                                spotId.id(), deadline))
                        .toList());
    }

    static <S> CompletionStage<Void> executePlan(
            RelocationPlan<S> plan,
            UnitOperation<S> spotPreflight,
            UnitOperation<String> actorPreflight,
            Runnable onPreflightPassed,
            BooleanSupplier stopBeforeNextUnit,
            UnitOperation<S> spotRelocation,
            UnitOperation<String> actorRelocation) {
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

    static <S> CompletionStage<Void> executePlan(
            RelocationPlan<S> plan,
            UnitOperation<S> spotPreflight,
            UnitOperation<String> actorPreflight,
            Supplier<CompletionStage<Void>> onPreflightPassed,
            BooleanSupplier stopBeforeNextUnit,
            UnitOperation<S> spotRelocation,
            UnitOperation<String> actorRelocation) {
        return executePlan(
                plan,
                spotPreflight,
                actorPreflight,
                onPreflightPassed,
                stopBeforeNextUnit,
                id -> spotRelocation.execute(id).thenApply(ignored -> true),
                id -> actorRelocation.execute(id).thenApply(ignored -> true),
                () -> CompletableFuture.completedFuture(null));
    }

    private static <S> CompletionStage<Void> executePlan(
            RelocationPlan<S> plan,
            UnitOperation<S> spotPreflight,
            UnitOperation<String> actorPreflight,
            Supplier<CompletionStage<Void>> onPreflightPassed,
            BooleanSupplier stopBeforeNextUnit,
            RelocationOperation<S> spotRelocation,
            RelocationOperation<String> actorRelocation,
            Supplier<CompletionStage<Void>> onActorBatchComplete) {
        Objects.requireNonNull(plan, "plan");
        Objects.requireNonNull(spotPreflight, "spotPreflight");
        Objects.requireNonNull(actorPreflight, "actorPreflight");
        Objects.requireNonNull(onPreflightPassed, "onPreflightPassed");
        Objects.requireNonNull(stopBeforeNextUnit, "stopBeforeNextUnit");
        Objects.requireNonNull(spotRelocation, "spotRelocation");
        Objects.requireNonNull(actorRelocation, "actorRelocation");
        CompletionStage<Void> preflight = CompletableFuture.completedFuture(null);
        for (S spotId : plan.shellSpotIds()) {
            preflight = preflight.thenCompose(ignored -> spotPreflight.execute(spotId));
        }
        for (S spotId : plan.aggregateSpotIds()) {
            preflight = preflight.thenCompose(ignored -> spotPreflight.execute(spotId));
        }
        for (String actorId : plan.actorIds()) {
            preflight = preflight.thenCompose(ignored -> actorPreflight.execute(actorId));
        }
        CompletionStage<Void> completion =
                preflight.thenCompose(
                        ignored ->
                                Objects.requireNonNull(
                                        onPreflightPassed.get(), "onPreflightPassed result"));
        var committed = new AtomicInteger();
        // Host relocation §7 orders the shell, Actor and aggregate batches.
        for (var batch :
                List.of(
                        new RelocationBatch<>(plan.shellSpotIds(), spotRelocation),
                        new RelocationBatch<>(
                                plan.actorIds(), actorRelocation, onActorBatchComplete),
                        new RelocationBatch<>(plan.aggregateSpotIds(), spotRelocation))) {
            completion =
                    completion.thenCompose(ignored -> batch.execute(stopBeforeNextUnit, committed));
        }
        return completion;
    }

    //  Shutdown is re-checked as the unit starts, not when the plan was built.
    private static CompletionStage<Boolean> startUnit(
            BooleanSupplier stopBeforeNextUnit, Supplier<CompletionStage<Boolean>> relocation) {
        if (stopBeforeNextUnit.getAsBoolean()) {
            return shutdownRequested();
        }
        try {
            return Objects.requireNonNull(relocation.get(), "relocation action result");
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    // Each batch settles before reporting its first failure. Later failures are
    // attached to it. A successful unit confirms target authority, so commits
    // from every batch count when classifying split authority (Host relocation §13).
    private static CompletionStage<Void> awaitAllUnits(
            List<CompletionStage<Boolean>> units, AtomicInteger committed) {
        if (units.isEmpty()) {
            return CompletableFuture.completedFuture(null);
        }
        AtomicReference<Throwable> firstFailure = new AtomicReference<>();
        CompletableFuture<?>[] settled =
                units.stream()
                        .map(
                                unit ->
                                        unit.toCompletableFuture()
                                                .handle(
                                                        (moved, failure) -> {
                                                            if (Boolean.TRUE.equals(moved)) {
                                                                committed.incrementAndGet();
                                                            }
                                                            if (failure != null
                                                                    && !firstFailure.compareAndSet(
                                                                            null, failure)) {
                                                                Throwable first =
                                                                        firstFailure.get();
                                                                if (first != failure) {
                                                                    first.addSuppressed(failure);
                                                                }
                                                            }
                                                            return null;
                                                        }))
                        .toArray(CompletableFuture[]::new);
        return CompletableFuture.allOf(settled)
                .thenCompose(
                        ignored -> {
                            Throwable failure = firstFailure.get();
                            if (failure == null) {
                                return CompletableFuture.completedFuture(null);
                            }
                            Throwable cause = unwrapCompletion(failure);
                            if (committed.get() == 0
                                    || cause instanceof RelocationAuthorityErrorException) {
                                return CompletableFuture.<Void>failedFuture(failure);
                            }
                            var split =
                                    new RelocationAuthorityErrorException(
                                            "Relocation units committed on different sides"
                                                    + " (some on source, some on target)");
                            split.initCause(cause);
                            return CompletableFuture.<Void>failedFuture(split);
                        });
    }

    private static <T> CompletionStage<T> shutdownRequested() {
        return CompletableFuture.failedFuture(
                new RelocationBlockedException(
                        ZLinkFrameworkRelocationReason.SHUTDOWN_REQUESTED,
                        "Shutdown was requested during relocation"));
    }

    private CompletionStage<Void> preflightSpot(
            ZLinkSpotRelocationUnit spot,
            ZLinkStoreCancellation cancellation,
            ZLinkRelocationTargetPolicy targetPolicy) {
        String meshName = spot.mesh();
        Lane lane = lanes.get(meshName);
        if (lane == null) {
            return CompletableFuture.failedFuture(
                    new RelocationBlockedException(
                            ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE,
                            "User Spot Retire lane is unavailable: " + meshName));
        }
        return lane.source().preflight(spot, targetPolicy, cancellation);
    }

    private CompletionStage<Void> preflightActor(
            String actorId,
            ZLinkStoreCancellation cancellation,
            ZLinkRelocationTargetPolicy targetPolicy) {
        String meshName = spots.actorSessions().actorMeshName(actorId);
        ActorLane lane = actorLanes.get(meshName);
        if (lane == null) {
            return CompletableFuture.failedFuture(
                    new RelocationBlockedException(
                            ZLinkFrameworkRelocationReason.STATE_INCOMPATIBLE,
                            "Entry Spot Actor relocation lane is unavailable: " + meshName));
        }
        return lane.source().preflight(actorId, targetPolicy, cancellation);
    }

    private CompletionStage<Boolean> relocateOne(
            ZLinkSpotRelocationUnit spot,
            Instant deadline,
            ZLinkStoreCancellation cancellation,
            ZLinkRelocationTargetPolicy targetPolicy,
            CompletionStage<?> cancellationSignal) {
        String meshName = spot.mesh();
        Lane lane = lanes.get(meshName);
        if (lane == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("User Spot Retire lane is unavailable: " + meshName));
        }
        Duration timeout = Duration.between(Instant.now(), deadline);
        if (timeout.isZero() || timeout.isNegative()) {
            return CompletableFuture.failedFuture(
                    new TimeoutException("User Spot Retire deadline elapsed"));
        }
        return lane.awaitUnitAdmission()
                .thenCompose(
                        admitted ->
                                lane.source()
                                        .prepare(
                                                spot,
                                                targetPolicy,
                                                cancellation,
                                                cancellationSignal))
                .thenCompose(
                        prepared -> {
                            if (prepared.isEmpty()) {
                                return CompletableFuture.completedFuture(false);
                            }
                            var source = prepared.orElseThrow();
                            requireExactCoreReady(
                                    lane.node(),
                                    source.stageRequest().targetNodeRid(),
                                    source.stageRequest().targetNodeGeneration());
                            return lane.scheduler()
                                    .executeRemote(
                                            new ZLinkUserSpotRetireScheduler.RemoteRequest(
                                                    source,
                                                    lane.client(),
                                                    timeout.compareTo(CONTROL_TIMEOUT) < 0
                                                            ? timeout
                                                            : CONTROL_TIMEOUT,
                                                    deadline,
                                                    () -> source.cleanupLocal(deadline)),
                                            cancellation)
                                    .thenApply(ignored -> true);
                        });
    }

    private CompletionStage<Void> relocateActor(
            String actorId,
            Instant deadline,
            ZLinkStoreCancellation cancellation,
            ZLinkRelocationTargetPolicy targetPolicy) {
        String meshName = spots.actorSessions().actorMeshName(actorId);
        ActorLane lane = actorLanes.get(meshName);
        if (lane == null) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException(
                            "Entry Spot Actor relocation lane is unavailable: " + meshName));
        }
        Duration timeout = Duration.between(Instant.now(), deadline);
        if (timeout.isZero() || timeout.isNegative()) {
            return CompletableFuture.failedFuture(
                    new TimeoutException("Actor relocation deadline elapsed"));
        }
        return lane.awaitUnitAdmission()
                .thenCompose(admitted -> lane.source().prepare(actorId, targetPolicy, cancellation))
                .thenCompose(
                        source -> {
                            requireExactCoreReady(
                                    lane.node(),
                                    source.stageRequest().targetNodeRid(),
                                    source.stageRequest().targetNodeGeneration());
                            return lane.scheduler()
                                    .executeRemote(
                                            source,
                                            lane.client(),
                                            timeout.compareTo(CONTROL_TIMEOUT) < 0
                                                    ? timeout
                                                    : CONTROL_TIMEOUT,
                                            deadline,
                                            cancellation);
                        });
    }

    private record Lane(
            ZLinkUserSpotRetireSourceBuilder source,
            ZLinkUserSpotRetireScheduler scheduler,
            ZLinkRelocationTransitionClient client,
            ZLinkInternalMeshNode node) {
        CompletionStage<Void> awaitUnitAdmission() {
            return client instanceof ZLinkCanonicalRelocationStateMachine machine
                    ? machine.awaitUnitAdmission()
                    : CompletableFuture.completedFuture(null);
        }
    }

    private record ActorLane(
            ZLinkStandaloneActorRelocationSourceBuilder source,
            ZLinkStandaloneActorRelocationScheduler scheduler,
            ZLinkRelocationTransitionClient client,
            ZLinkInternalMeshNode node) {
        CompletionStage<Void> awaitUnitAdmission() {
            return client instanceof ZLinkCanonicalRelocationStateMachine machine
                    ? machine.awaitUnitAdmission()
                    : CompletableFuture.completedFuture(null);
        }
    }

    private static void requireExactCoreReady(
            ZLinkInternalMeshNode node, RoutingId targetRid, long targetGeneration) {
        boolean ready = node.isCanonicalRelocationTargetAdmitted(targetRid, targetGeneration);
        if (!ready) {
            throw new RelocationBlockedException(
                    ZLinkFrameworkRelocationReason.TARGET_UNAVAILABLE,
                    "Relocation target is not Core-ready: " + targetRid);
        }
    }

    record RelocationPlan<S>(
            List<S> shellSpotIds, List<String> actorIds, List<S> aggregateSpotIds) {
        RelocationPlan {
            shellSpotIds = List.copyOf(shellSpotIds);
            actorIds = List.copyOf(actorIds);
            aggregateSpotIds = List.copyOf(aggregateSpotIds);
        }

        boolean isEmpty() {
            return shellSpotIds.isEmpty() && actorIds.isEmpty() && aggregateSpotIds.isEmpty();
        }
    }

    private record RelocationInventory(RelocationPlan<ZLinkSpotRelocationUnit> plan) {
        boolean matches(RelocationInventory current) {
            var next = current.plan;
            var remaining = new HashSet<>(next.aggregateSpotIds());
            // Planned Instance Spot departures are settled by the source Store fence.
            return plan.shellSpotIds().equals(next.shellSpotIds())
                    && plan.actorIds().equals(next.actorIds())
                    && new HashSet<>(plan.aggregateSpotIds()).containsAll(remaining)
                    && plan.aggregateSpotIds().stream()
                            .filter(spot -> spot.kind() == ZLinkPlacementObjectKind.USER_SPOT)
                            .allMatch(remaining::contains);
        }

        CompletionStage<Void> preflightTargets(Supplier<CompletionStage<Void>> targetPreflight) {
            return plan.isEmpty()
                    ? CompletableFuture.completedFuture(null)
                    : Objects.requireNonNull(targetPreflight.get(), "target preflight result");
        }
    }

    private record RelocationBatch<T>(
            List<T> ids,
            RelocationOperation<T> operation,
            Supplier<CompletionStage<Void>> onComplete) {
        private RelocationBatch(List<T> ids, RelocationOperation<T> operation) {
            this(ids, operation, () -> CompletableFuture.completedFuture(null));
        }

        CompletionStage<Void> execute(BooleanSupplier stopBeforeNextUnit, AtomicInteger committed) {
            var started = new ArrayList<CompletionStage<Boolean>>(ids.size());
            for (T id : ids) {
                started.add(startUnit(stopBeforeNextUnit, () -> operation.execute(id)));
            }
            return awaitAllUnits(started, committed).thenCompose(done -> onComplete.get());
        }
    }

    @FunctionalInterface
    private interface RelocationOperation<T> {
        CompletionStage<Boolean> execute(T objectId);
    }

    @FunctionalInterface
    interface UnitOperation<T> {
        CompletionStage<Void> execute(T objectId);
    }
}
