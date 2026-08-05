package systems.zlink.framework.runtime.spots;

import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotCreateState;
import systems.zlink.framework.spots.ZLinkSpotInfo;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;

final class ZLinkSpotLifecycle {
    @FunctionalInterface
    interface ActivationFactory {
        CompletionStage<SpotActivationCreateResult> activate(
            Class<? extends ZLinkSpot<?>> spotType,
            ZLinkBackendSpot backendSpot,
            ZLinkMessage request);
    }

    @FunctionalInterface
    interface ActorOccupancy {
        boolean hasActorsInSpot(String spotId);
    }

    private final ZLinkInternalSpotNode primaryNode;
    private final String meshName;
    private final Executor backendExecutor;
    private final ZLinkSpotLocationCoordinator locations;
    private final ActivationFactory activationFactory;
    private final ActorOccupancy actorOccupancy;
    private final Set<Class<? extends ZLinkSpot<?>>> registeredSpotTypes;
    private final List<EntrySpotActivation> entrySpots;
    private final Map<String, SpotActivation> spots = new ConcurrentHashMap<>();
    private final Map<String, CompletionStage<ZLinkSpotCreateResult>> pendingCreates =
        new ConcurrentHashMap<>();

    ZLinkSpotLifecycle(
        ZLinkInternalSpotNode primaryNode,
        String meshName,
        Executor backendExecutor,
        ZLinkSpotLocationCoordinator locations,
        Collection<Class<? extends ZLinkSpot<?>>> registeredSpotTypes,
        ActivationFactory activationFactory,
        ActorOccupancy actorOccupancy) {
        this.primaryNode = primaryNode;
        this.meshName = meshName;
        this.backendExecutor = backendExecutor;
        this.locations = locations;
        this.registeredSpotTypes = Set.copyOf(registeredSpotTypes);
        this.entrySpots = new java.util.ArrayList<>();
        this.activationFactory = activationFactory;
        this.actorOccupancy = actorOccupancy;
    }

    void addEntrySpot(EntrySpotActivation activation) {
        entrySpots.add(activation);
        ZLinkRuntimeMetrics.add("zlink.spot.count", 1, Map.of("kind", "entry"));
        ZLinkRuntimeMetrics.increment("zlink.spot.created", Map.of("kind", "entry"));
    }

    CompletionStage<ZLinkSpotCreateResult> create(
        Class<? extends ZLinkSpot<?>> spotType,
        ZLinkMessage request) {
        requireRegistered(spotType);
        return createBackendSpotAsync()
            .thenCompose(backendSpot -> {
                String spotId = backendSpot.spotId();
                if (spots.containsKey(spotId)) {
                    backendSpot.close();
                    throw duplicateSpot(spotId);
                }
                return activateAndClaim(spotType, backendSpot, request);
            });
    }

    CompletionStage<ZLinkSpotCreateResult> create(
        Class<? extends ZLinkSpot<?>> spotType,
        String spotId,
        ZLinkMessage request) {
        requireRegistered(spotType);
        requireSpotId(spotId);
        if (spots.containsKey(spotId) || pendingCreates.containsKey(spotId)) {
            throw duplicateSpot(spotId);
        }
        return beginCreate(spotType, spotId, request, false);
    }

    CompletionStage<ZLinkSpotCreateResult> getOrCreate(
        Class<? extends ZLinkSpot<?>> spotType,
        String spotId,
        ZLinkMessage request) {
        requireRegistered(spotType);
        requireSpotId(spotId);
        SpotActivation existing = spots.get(spotId);
        if (existing != null) {
            if (existing.spot().getClass() != spotType) {
                throw new ZLinkConfigurationException("spot type mismatch: " + spotId);
            }
            return CompletableFuture.completedFuture(existingResult(spotId));
        }
        CompletionStage<ZLinkSpotCreateResult> pending = pendingCreates.get(spotId);
        return pending == null
            ? beginCreate(spotType, spotId, request, true)
            : asExisting(pending);
    }

    CompletionStage<Optional<ZLinkSpotInfo>> find(String spotId) {
        requireSpotId(spotId);
        return CompletableFuture.completedFuture(
            spots.containsKey(spotId)
                ? Optional.of(new ZLinkSpotInfo(spotId))
                : Optional.empty());
    }

    CompletionStage<List<ZLinkSpotInfo>> list() {
        return CompletableFuture.completedFuture(
            spots.keySet().stream().map(ZLinkSpotInfo::new).toList());
    }

    List<String> userSpotIds() {
        return spots.keySet().stream().sorted().toList();
    }

    CompletionStage<Boolean> close(String spotId) {
        requireSpotId(spotId);
        if (actorOccupancy.hasActorsInSpot(spotId)) {
            return CompletableFuture.completedFuture(false);
        }
        SpotActivation removed = spots.remove(spotId);
        if (removed == null) {
            return CompletableFuture.completedFuture(false);
        }
        removed.close();
        return locations.releaseUserSpotAsync(primaryNode.routingId(), spotId)
            .whenComplete((ignored, error) -> {
                ZLinkRuntimeMetrics.add("zlink.spot.count", -1, Map.of("kind", "user"));
                ZLinkRuntimeMetrics.increment("zlink.spot.closed", Map.of("kind", "user"));
            })
            .thenApply(ignored -> true);
    }

    CompletionStage<PreparedUserSpot> prepareReserved(
        Class<? extends ZLinkSpot<?>> spotType,
        String spotId,
        long objectGeneration,
        ZLinkMessage request) {
        requireRegistered(spotType);
        requireSpotId(spotId);
        if (objectGeneration <= 0) {
            return CompletableFuture.failedFuture(
                new IllegalArgumentException(
                    "objectGeneration must be positive"));
        }
        SpotActivation existing = spots.get(spotId);
        if (existing != null) {
            if (existing.backendSpot.lifecycleGeneration()
                    != objectGeneration
                || existing.spot().getClass() != spotType) {
                return CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "User Spot reservation is stale"));
            }
            return CompletableFuture.completedFuture(
                PreparedUserSpot.existing(
                    spotId, objectGeneration));
        }
        return CompletableFuture.supplyAsync(
                () -> primaryNode.createSpot(
                    spotId, objectGeneration),
                backendExecutor)
            .thenCompose(backendSpot -> activationFactory
                .activate(spotType, backendSpot, request)
                .thenApply(created -> new PreparedUserSpot(
                        spotId,
                        objectGeneration,
                        created)));
    }

    void publishReserved(PreparedUserSpot prepared) {
        if (prepared.existing()) {
            return;
        }
        if (!prepared.created().response().accepted()) {
            throw new IllegalStateException(
                "Rejected User Spot cannot cross the Ready barrier");
        }
        SpotActivation activation =
            prepared.created().activation();
        SpotActivation current = spots.putIfAbsent(
            prepared.spotId(), activation);
        if (current != null && current != activation) {
            activation.close();
            throw new IllegalStateException(
                "User Spot Ready publication lost local admission");
        }
        ZLinkRuntimeMetrics.add(
            "zlink.spot.count", 1, Map.of("kind", "user"));
        ZLinkRuntimeMetrics.increment(
            "zlink.spot.created", Map.of("kind", "user"));
    }

    void discardReserved(PreparedUserSpot prepared) {
        if (!prepared.existing()) {
            SpotActivation activation =
                prepared.created().activation();
            if (activation != null) {
                activation.close();
            }
        }
    }

    Object preparedSpot(PreparedUserSpot prepared) {
        requireNewPrepared(prepared);
        return prepared.created().activation().spot();
    }

    CompletionStage<Void> completeRelocationReady(
        PreparedUserSpot prepared) {
        requireNewPrepared(prepared);
        return prepared.created().activation().context
            .runRelocationReadyCompletion(
                systems.zlink.framework.spots
                    .ZLinkSpotRelocationReadyOutcome.RELOCATED);
    }

    CompletionStage<List<byte[]>> replayReserved(
        PreparedUserSpot prepared,
        ZLinkSpotAcceptedJournal.Record record) {
        requireNewPrepared(prepared);
        return prepared.created().activation().replayAccepted(record);
    }

    void stageReservedTimers(
        PreparedUserSpot prepared,
        byte[] timerEnvelope) {
        requireNewPrepared(prepared);
        prepared.created().activation().context
            .stageTimerRelocationEnvelope(timerEnvelope);
    }

    void publishReservedTimers(PreparedUserSpot prepared) {
        requireNewPrepared(prepared);
        prepared.created().activation().context
            .publishStagedTimerRelocation();
    }

    void stageReservedActorTimers(
        PreparedUserSpot prepared,
        String actorId,
        byte[] timerEnvelope) {
        requireNewPrepared(prepared);
        prepared.created().activation().context
            .stageActorTimerRelocationEnvelope(actorId, timerEnvelope);
    }

    void publishReservedActorTimers(
        PreparedUserSpot prepared,
        String actorId) {
        requireNewPrepared(prepared);
        prepared.created().activation().context
            .publishStagedActorTimerRelocation(actorId);
    }

    private static void requireNewPrepared(PreparedUserSpot prepared) {
        if (prepared == null || prepared.existing()) {
            throw new IllegalStateException(
                "relocation staging requires a new prepared User Spot");
        }
    }

    CompletionStage<Boolean> closeReserved(
        String spotId,
        long objectGeneration) {
        requireSpotId(spotId);
        SpotActivation current = spots.get(spotId);
        if (current == null) {
            return CompletableFuture.completedFuture(false);
        }
        if (current.backendSpot.lifecycleGeneration()
                != objectGeneration) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "User Spot generation is stale"));
        }
        if (actorOccupancy.hasActorsInSpot(spotId)) {
            return CompletableFuture.completedFuture(false);
        }
        if (!spots.remove(spotId, current)) {
            return CompletableFuture.failedFuture(
                new IllegalStateException(
                    "User Spot is moving or closing"));
        }
        current.close();
        ZLinkRuntimeMetrics.add(
            "zlink.spot.count", -1, Map.of("kind", "user"));
        ZLinkRuntimeMetrics.increment(
            "zlink.spot.closed", Map.of("kind", "user"));
        return CompletableFuture.completedFuture(true);
    }

    CompletionStage<Void> completeRelocationSource(
        String spotId,
        long objectGeneration,
        java.time.Instant deadline) {
        requireSpotId(spotId);
        java.util.Objects.requireNonNull(deadline, "deadline");
        SpotActivation current = spots.get(spotId);
        if (current == null) {
            return CompletableFuture.completedFuture(null);
        }
        if (current.backendSpot.lifecycleGeneration() != objectGeneration) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "User Spot generation changed before relocation cleanup"));
        }
        if (actorOccupancy.hasActorsInSpot(spotId)) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "User Spot still has source Actor participants"));
        }
        if (!spots.remove(spotId, current)) {
            return CompletableFuture.failedFuture(new IllegalStateException(
                "User Spot relocation source changed during cleanup"));
        }
        current.close(
            systems.zlink.framework.spots.ZLinkSpotCloseReason.RELOCATION_OUT,
            deadline);
        ZLinkRuntimeMetrics.add(
            "zlink.spot.count", -1, Map.of("kind", "user"));
        ZLinkRuntimeMetrics.increment(
            "zlink.spot.closed", Map.of("kind", "user"));
        return CompletableFuture.completedFuture(null);
    }

    CloseReadiness closeReadiness(
        String spotId,
        long objectGeneration) {
        requireSpotId(spotId);
        SpotActivation current = spots.get(spotId);
        if (current == null) {
            return CloseReadiness.LOCAL_MISSING;
        }
        if (current.backendSpot.lifecycleGeneration() != objectGeneration) {
            return CloseReadiness.GENERATION_STALE;
        }
        if (actorOccupancy.hasActorsInSpot(spotId)) {
            return CloseReadiness.HAS_ACTORS;
        }
        return CloseReadiness.READY;
    }

    enum CloseReadiness {
        READY,
        HAS_ACTORS,
        LOCAL_MISSING,
        GENERATION_STALE
    }

    void drainRoutedDispatchQueues() {
        for (EntrySpotActivation activation : entrySpots) {
            activation.drainPolledDispatchQueues();
        }
        for (SpotActivation activation : spots.values()) {
            activation.drainPolledDispatchQueues();
        }
    }

    void sealApplicationAdmission() {
        for (EntrySpotActivation activation : entrySpots) {
            activation.context.sealTimerAdmission();
        }
        for (SpotActivation activation : spots.values()) {
            activation.context.sealTimerAdmission();
        }
    }

    CompletionStage<Void> awaitApplicationTurns() {
        List<CompletableFuture<Void>> barriers = new java.util.ArrayList<>();
        for (EntrySpotActivation activation : entrySpots) {
            barriers.add(
                activation.context.awaitAllLanes().toCompletableFuture());
        }
        for (SpotActivation activation : spots.values()) {
            barriers.add(
                activation.context.awaitAllLanes().toCompletableFuture());
        }
        return CompletableFuture.allOf(barriers.toArray(CompletableFuture[]::new));
    }

    CompletionStage<systems.zlink.framework.spots.ZLinkActorCreateResponse> notifyEntrySpotActorCreated(
        RoutingId nodeRid,
        ZLinkActor actor,
        ZLinkMessage createRequest,
        Object createContext) {
        for (EntrySpotActivation activation : entrySpots) {
            if (activation.context.nodeRid().equals(nodeRid)) {
                return activation.notifyActorCreated(actor, createRequest, createContext);
            }
        }
        return java.util.concurrent.CompletableFuture.completedFuture(
            systems.zlink.framework.spots.ZLinkActorCreateResponse.accept());
    }

    ZLinkSpot<?> spotFor(String spotId) {
        SpotActivation activation = spots.get(spotId);
        return activation == null ? null : activation.spot();
    }

    DefaultSpotContext contextFor(ZLinkSpot<?> spot) {
        for (SpotActivation activation : spots.values()) {
            if (activation.spot() == spot) {
                return activation.context;
            }
        }
        return null;
    }

    ZLinkUserSpotRelocationBarrier relocationBarrier(
        String spotId,
        ZLinkActorSessionCoordinator actors) {
        SpotActivation activation = spots.get(spotId);
        if (activation == null) {
            throw new ZLinkConfigurationException(
                "User Spot is not active locally: " + spotId);
        }
        return activation.context.relocationBarrier(actors);
    }

    boolean hasUserSpot(String spotId) {
        return spots.containsKey(spotId);
    }

    Object spotSurfaceFor(String spotId) {
        SpotActivation activation = spots.get(spotId);
        if (activation != null) {
            return activation.spot();
        }
        EntrySpotActivation entry = entrySpotActivationFor(spotId);
        return entry == null ? null : entry.entrySpot();
    }

    EntrySpotActivation entrySpotActivationFor(String spotId) {
        for (EntrySpotActivation activation : entrySpots) {
            if (activation.backendSpot.spotId().equals(spotId)) {
                return activation;
            }
        }
        return null;
    }

    Object firstEntrySpot() {
        return entrySpots.isEmpty() ? null : entrySpots.get(0).entrySpot();
    }

    void closeAll() {
        closeAllAsync();
    }

    CompletionStage<Void> closeAllAsync() {
        return closeAllAsync(java.time.Instant.now());
    }

    CompletionStage<Void> closeAllAsync(java.time.Instant deadline) {
        java.util.concurrent.atomic.AtomicReference<RuntimeException> firstFailure =
            new java.util.concurrent.atomic.AtomicReference<>();
        List<EntrySpotActivation> closingEntrySpots = List.copyOf(entrySpots);
        List<SpotActivation> closingSpots = List.copyOf(spots.values());
        for (EntrySpotActivation entrySpot : closingEntrySpots) {
            recordCloseFailure(
                firstFailure,
                closeComponent(() -> entrySpot.close(deadline), null));
        }
        for (SpotActivation spot : closingSpots) {
            recordCloseFailure(
                firstFailure,
                closeComponent(
                    () -> spot.close(
                        systems.zlink.framework.spots
                            .ZLinkSpotCloseReason.HOST_SHUTDOWN,
                        deadline),
                    null));
        }
        if (!entrySpots.isEmpty()) {
            ZLinkRuntimeMetrics.add("zlink.spot.count", -entrySpots.size(), Map.of("kind", "entry"));
        }
        entrySpots.clear();
        if (!spots.isEmpty()) {
            ZLinkRuntimeMetrics.add("zlink.spot.count", -spots.size(), Map.of("kind", "user"));
        }
        spots.clear();
        List<CompletableFuture<Void>> cleanups = new java.util.ArrayList<>();
        for (EntrySpotActivation entrySpot : closingEntrySpots) {
            cleanups.add(locations.releaseEntrySpotAsync(entrySpot.context.nodeRid())
                .handle((ignored, error) -> {
                    recordCloseFailure(firstFailure, error);
                    return (Void) null;
                }).toCompletableFuture());
        }
        for (SpotActivation spot : closingSpots) {
            cleanups.add(locations.releaseUserSpotAsync(
                    primaryNode.routingId(), spot.backendSpot.spotId())
                .handle((ignored, error) -> {
                    recordCloseFailure(firstFailure, error);
                    return (Void) null;
                }).toCompletableFuture());
        }
        return CompletableFuture.allOf(cleanups.toArray(CompletableFuture[]::new))
            .thenCompose(ignored -> firstFailure.get() == null
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(firstFailure.get()));
    }

    private static void recordCloseFailure(
        java.util.concurrent.atomic.AtomicReference<RuntimeException> target,
        Throwable error) {
        if (error == null) {
            return;
        }
        Throwable value = error;
        while (value instanceof java.util.concurrent.CompletionException && value.getCause() != null) {
            value = value.getCause();
        }
        RuntimeException failure = value instanceof RuntimeException runtime
            ? runtime : new RuntimeException(value);
        RuntimeException first = target.get();
        if (first == null) {
            target.compareAndSet(null, failure);
        } else {
            first.addSuppressed(failure);
        }
    }

    CompletionStage<Void> releaseRecreatableSpots() {
        return releaseRecreatableSpots(
            systems.zlink.framework.spots.ZLinkSpotCloseReason.HOST_SHUTDOWN,
            java.time.Instant.now());
    }

    CompletionStage<Void> releaseRecreatableSpots(
        systems.zlink.framework.spots.ZLinkSpotCloseReason reason,
        java.time.Instant deadline) {
        List<String> spotIds = List.copyOf(spots.keySet());
        for (String spotId : spotIds) {
            if (actorOccupancy.hasActorsInSpot(spotId)) {
                return CompletableFuture.failedFuture(new IllegalStateException(
                    "recreatable spot still has actors: " + spotId));
            }
        }
        List<SpotActivation> released = new java.util.ArrayList<>(spotIds.size());
        for (String spotId : spotIds) {
            SpotActivation activation = spots.remove(spotId);
            if (activation != null) {
                released.add(activation);
            }
        }
        java.util.concurrent.atomic.AtomicReference<RuntimeException> firstFailure =
            new java.util.concurrent.atomic.AtomicReference<>();
        for (SpotActivation activation : released) {
            recordCloseFailure(
                firstFailure,
                closeComponent(
                    () -> activation.close(reason, deadline),
                    null));
        }
        List<CompletableFuture<Void>> cleanups = new java.util.ArrayList<>(released.size());
        for (SpotActivation activation : released) {
            cleanups.add(locations.releaseUserSpotAsync(
                    primaryNode.routingId(), activation.backendSpot.spotId())
                .handle((ignored, error) -> {
                    recordCloseFailure(firstFailure, error);
                    ZLinkRuntimeMetrics.add("zlink.spot.count", -1, Map.of("kind", "user"));
                    ZLinkRuntimeMetrics.increment("zlink.spot.closed", Map.of("kind", "user"));
                    return (Void) null;
                }).toCompletableFuture());
        }
        return CompletableFuture.allOf(cleanups.toArray(CompletableFuture[]::new))
            .thenCompose(ignored -> firstFailure.get() == null
                ? CompletableFuture.completedFuture(null)
                : CompletableFuture.failedFuture(firstFailure.get()));
    }

    boolean userSpotsDrained() {
        return spots.isEmpty() && pendingCreates.isEmpty();
    }

    int userSpotCount() {
        return spots.size();
    }

    private CompletionStage<ZLinkSpotCreateResult> beginCreate(
        Class<? extends ZLinkSpot<?>> spotType,
        String spotId,
        ZLinkMessage request,
        boolean reuseConcurrentCreate) {
        CompletableFuture<ZLinkSpotCreateResult> result = new CompletableFuture<>();
        CompletionStage<ZLinkSpotCreateResult> concurrent =
            pendingCreates.putIfAbsent(spotId, result);
        if (concurrent != null) {
            if (reuseConcurrentCreate) {
                return asExisting(concurrent);
            }
            throw duplicateSpot(spotId);
        }
        try {
            createBackendSpotAsync(spotId)
                .thenCompose(backendSpot -> activationFactory
                    .activate(spotType, backendSpot, request)
                    .thenCompose(created -> createResultAsync(
                        spotId,
                        backendSpot.lifecycleGeneration(),
                        spotType,
                        created)))
                .whenComplete((created, error) -> {
                    pendingCreates.remove(spotId, result);
                    if (error == null) {
                        result.complete(created);
                    } else {
                        result.completeExceptionally(error);
                    }
                });
        } catch (RuntimeException error) {
            pendingCreates.remove(spotId, result);
            result.completeExceptionally(error);
        }
        return result;
    }

    private CompletionStage<ZLinkSpotCreateResult> activateAndClaim(
        Class<? extends ZLinkSpot<?>> spotType,
        ZLinkBackendSpot backendSpot,
        ZLinkMessage request) {
        String spotId = backendSpot.spotId();
        return activationFactory.activate(spotType, backendSpot, request)
            .thenCompose(result -> createResultAsync(
                spotId,
                backendSpot.lifecycleGeneration(),
                spotType,
                result));
    }

    private CompletionStage<ZLinkSpotCreateResult> createResultAsync(
        String spotId,
        long spotGeneration,
        Class<? extends ZLinkSpot<?>> spotType,
        SpotActivationCreateResult result) {
        if (!result.response().accepted()) {
            return CompletableFuture.completedFuture(new ZLinkSpotCreateResult(
                ref(spotId, spotGeneration),
                ZLinkSpotCreateState.REJECTED,
                result.response().reply()));
        }
        SpotActivation activation = result.activation();
        return locations.claimUserSpotAsync(
                primaryNode.routingId(),
                spotId,
                spotGeneration,
                spotType,
                () -> close(spotId))
            .thenApply(status -> {
                if (status != ZLinkLocationWriteStatus.STORED) {
                    throw spotCreateLocationFailure(spotId, status);
                }
                spots.put(spotId, activation);
                ZLinkRuntimeMetrics.add("zlink.spot.count", 1, Map.of("kind", "user"));
                ZLinkRuntimeMetrics.increment("zlink.spot.created", Map.of("kind", "user"));
                return new ZLinkSpotCreateResult(
                    ref(spotId, spotGeneration),
                    ZLinkSpotCreateState.CREATED,
                    result.response().reply());
            })
            .whenComplete((ignored, error) -> {
                if (error != null) {
                    activation.close();
                }
            });
    }

    private CompletionStage<ZLinkBackendSpot> createBackendSpotAsync() {
        return CompletableFuture.supplyAsync(primaryNode::createSpot, backendExecutor);
    }

    private CompletionStage<ZLinkBackendSpot> createBackendSpotAsync(String spotId) {
        return CompletableFuture.supplyAsync(
            () -> primaryNode.createSpot(spotId),
            backendExecutor);
    }

    private void requireRegistered(Class<? extends ZLinkSpot<?>> spotType) {
        if (spotType == null) {
            throw new ZLinkConfigurationException("spot type is required");
        }
        if (!registeredSpotTypes.contains(spotType)) {
            throw new ZLinkConfigurationException(
                "spot type is not registered: " + spotType.getName());
        }
    }

    private static void requireSpotId(String spotId) {
        try {
            systems.zlink.framework.runtime.internal.spots.ZLinkSpotIdValidator
                .requireValid(spotId);
        } catch (IllegalArgumentException error) {
            throw new ZLinkConfigurationException(error.getMessage(), error);
        }
    }

    private static CompletionStage<ZLinkSpotCreateResult> asExisting(
        CompletionStage<ZLinkSpotCreateResult> create) {
        return create.thenApply(result -> result.state() == ZLinkSpotCreateState.CREATED
            ? new ZLinkSpotCreateResult(
                result.spot(),
                ZLinkSpotCreateState.EXISTING,
                result.reply())
            : result);
    }

    private ZLinkSpotCreateResult existingResult(String spotId) {
        SpotActivation activation = spots.get(spotId);
        return new ZLinkSpotCreateResult(
            ref(spotId, activation.backendSpot.lifecycleGeneration()),
            ZLinkSpotCreateState.EXISTING,
            null);
    }

    private SpotRef ref(String spotId, long generation) {
        return new SpotRef(
            spotId, generation, meshName, primaryNode.routingId());
    }

    private static ZLinkConfigurationException duplicateSpot(String spotId) {
        return new ZLinkConfigurationException(
            ZLinkFrameworkErrorKind.ALREADY_EXISTS,
            "duplicate SpotId: " + spotId);
    }

    record PreparedUserSpot(
        String spotId,
        long objectGeneration,
        SpotActivationCreateResult created) {
        static PreparedUserSpot existing(
            String spotId,
            long objectGeneration) {
            return new PreparedUserSpot(
                spotId, objectGeneration, null);
        }

        boolean existing() {
            return created == null;
        }
    }

    private static ZLinkFrameworkException spotCreateLocationFailure(
        String spotId,
        ZLinkLocationWriteStatus status) {
        String message = status == ZLinkLocationWriteStatus.REJECTED_CONFLICT
            ? "SPOT '" + spotId + "' location is owned by another runtime."
            : "SPOT '" + spotId
                + "' location claim failed because the location store is unavailable.";
        return new ZLinkFrameworkException(
            status == ZLinkLocationWriteStatus.REJECTED_CONFLICT
                ? ZLinkFrameworkErrorKind.ALREADY_EXISTS
                : ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
            message);
    }

    private static RuntimeException closeComponent(
        Runnable close,
        RuntimeException firstFailure) {
        try {
            close.run();
        } catch (ZlinkCloseException ignored) {
        } catch (RuntimeException error) {
            if (firstFailure == null) {
                return error;
            }
            firstFailure.addSuppressed(error);
        }
        return firstFailure;
    }
}
