package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.errors.ZlinkCloseException;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalSpotNode;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationWriteStatus;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.runtime.internal.spots.ZLinkSpotIdValidator;
import systems.zlink.framework.spots.SpotRef;
import systems.zlink.framework.spots.ZLinkActorCreateResponse;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotCloseReason;
import systems.zlink.framework.spots.ZLinkSpotCreateResult;
import systems.zlink.framework.spots.ZLinkSpotCreateState;
import systems.zlink.framework.spots.ZLinkSpotInfo;

import java.time.Instant;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.Set;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.ConcurrentHashMap;
import java.util.concurrent.Executor;
import java.util.concurrent.atomic.AtomicReference;
import java.util.function.Function;

final class ZLinkSpotLifecycle {
    @FunctionalInterface
    interface ActorOccupancy {
        boolean hasActorsInSpot(String spotId);
    }

    private final Function<String, ZLinkInternalSpotNode> meshSpotNodes;
    private final Executor backendExecutor;
    private final ZLinkSpotLocationCoordinator locations;
    private final ZLinkSpotActivationFactory activationFactory;
    private final ActorOccupancy actorOccupancy;
    private final Set<Class<? extends ZLinkSpot<?>>> registeredSpotTypes;
    private final List<EntrySpotActivation> entrySpots;
    private final Map<String, SpotActivation> spots = new ConcurrentHashMap<>();

    /**
     * @param meshSpotNodes the Spot node of each MeshNode by mesh name; a User Spot is activated on
     *     the MeshNode that admitted it (Location runtime §7)
     */
    ZLinkSpotLifecycle(
            Function<String, ZLinkInternalSpotNode> meshSpotNodes,
            Executor backendExecutor,
            ZLinkSpotLocationCoordinator locations,
            Collection<Class<? extends ZLinkSpot<?>>> registeredSpotTypes,
            ZLinkSpotActivationFactory activationFactory,
            ActorOccupancy actorOccupancy) {
        this.meshSpotNodes = meshSpotNodes;
        this.backendExecutor = backendExecutor;
        this.locations = locations;
        this.registeredSpotTypes = Set.copyOf(registeredSpotTypes);
        this.entrySpots = new ArrayList<>();
        this.activationFactory = activationFactory;
        this.actorOccupancy = actorOccupancy;
    }

    void addEntrySpot(EntrySpotActivation activation) {
        entrySpots.add(activation);
        ZLinkRuntimeMetrics.add("zlink.spot.count", 1, Map.of("kind", "entry"));
        ZLinkRuntimeMetrics.increment("zlink.spot.created", Map.of("kind", "entry"));
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
        return locations
                .releaseUserSpotAsync(removed.context.nodeRid(), spotId)
                .whenComplete(
                        (ignored, error) -> {
                            ZLinkRuntimeMetrics.add("zlink.spot.count", -1, Map.of("kind", "user"));
                            ZLinkRuntimeMetrics.increment(
                                    "zlink.spot.closed", Map.of("kind", "user"));
                        })
                .thenApply(ignored -> true);
    }

    CompletionStage<PreparedUserSpot> prepareReserved(
            String admittingMeshName,
            Class<? extends ZLinkSpot<?>> spotType,
            String spotId,
            long objectGeneration,
            ZLinkMessage request) {
        requireRegistered(spotType);
        requireSpotId(spotId);
        // objectGeneration (Spot ObjectGeneration) is spec-bounded to
        // `1..long.MaxValue` (01-glossary "ObjectGeneration": provider
        // global counter, fails with GenerationExhausted instead of
        // wrapping), so `<= 0` never misclassifies valid traffic. Aligned to
        // `== 0` for consistency with the equivalent check already fixed at
        // ZLinkJavaRawSpotNode.createSpot (commit b7443ed9b4).
        if (objectGeneration == 0) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException("objectGeneration must be non-zero"));
        }
        SpotActivation existing = spots.get(spotId);
        if (existing != null) {
            if (existing.backendSpot.lifecycleGeneration() != objectGeneration
                    || existing.spot().getClass() != spotType) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException("User Spot reservation is stale"));
            }
            return CompletableFuture.completedFuture(
                    PreparedUserSpot.existing(spotId, objectGeneration, admittingMeshName));
        }
        ZLinkInternalSpotNode node = spotNode(admittingMeshName);
        return CompletableFuture.supplyAsync(
                        () -> node.createSpot(spotId, objectGeneration), backendExecutor)
                .thenCompose(
                        backendSpot ->
                                activationFactory
                                        .activate(spotType, backendSpot, request, node.routingId())
                                        .thenApply(
                                                created ->
                                                        new PreparedUserSpot(
                                                                spotId,
                                                                objectGeneration,
                                                                admittingMeshName,
                                                                created)));
    }

    CompletionStage<PreparedUserSpot> prepareRelocationReserved(
            String admittingMeshName,
            Class<? extends ZLinkSpot<?>> spotType,
            String spotId,
            long objectGeneration) {
        requireRegistered(spotType);
        requireSpotId(spotId);
        if (objectGeneration == 0) {
            return CompletableFuture.failedFuture(
                    new IllegalArgumentException("objectGeneration must be non-zero"));
        }
        if (spots.containsKey(spotId)) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("User Spot relocation target already exists"));
        }
        ZLinkInternalSpotNode node = spotNode(admittingMeshName);
        return CompletableFuture.supplyAsync(
                        () -> node.createSpot(spotId, objectGeneration), backendExecutor)
                .thenCompose(
                        backendSpot ->
                                activationFactory
                                        .activateRelocation(spotType, backendSpot, node.routingId())
                                        .thenApply(
                                                created ->
                                                        new PreparedUserSpot(
                                                                spotId,
                                                                objectGeneration,
                                                                admittingMeshName,
                                                                created)));
    }

    void publishReserved(PreparedUserSpot prepared) {
        if (prepared.existing()) {
            return;
        }
        if (!prepared.created().response().accepted()) {
            throw new IllegalStateException("Rejected User Spot cannot cross the Ready barrier");
        }
        SpotActivation activation = prepared.created().activation();
        activation.admittedBy(prepared.meshName());
        SpotActivation current = spots.putIfAbsent(prepared.spotId(), activation);
        if (current != null && current != activation) {
            activation.close();
            throw new IllegalStateException("User Spot Ready publication lost local admission");
        }
        ZLinkRuntimeMetrics.add("zlink.spot.count", 1, Map.of("kind", "user"));
        ZLinkRuntimeMetrics.increment("zlink.spot.created", Map.of("kind", "user"));
    }

    void discardReserved(PreparedUserSpot prepared) {
        if (!prepared.existing()) {
            SpotActivation activation = prepared.created().activation();
            if (activation != null) {
                activation.close();
            }
        }
    }

    Object preparedSpot(PreparedUserSpot prepared) {
        requireNewPrepared(prepared);
        return prepared.created().activation().spot();
    }

    CompletionStage<Void> completeRelocationReady(PreparedUserSpot prepared) {
        requireNewPrepared(prepared);
        return activationFactory
                .initializeRelocation(prepared.created().activation())
                .thenCompose(
                        ignored ->
                                prepared.created()
                                        .activation()
                                        .context
                                        .runRelocationReadyCompletion(
                                                systems.zlink.framework.spots
                                                        .ZLinkSpotRelocationReadyOutcome
                                                        .RELOCATED));
    }

    Object beginReservedIngressHold(PreparedUserSpot prepared) {
        requireNewPrepared(prepared);
        return prepared.created()
                .activation()
                .context
                .trySealRelocation()
                .orElseThrow(
                        () ->
                                new IllegalStateException(
                                        "reserved relocation target ingress could not be sealed"));
    }

    void resumeReservedIngress(PreparedUserSpot prepared, Object ingressHold) {
        requireNewPrepared(prepared);
        if (!(ingressHold instanceof ZLinkSerialExecutionQueue.RelocationSeal seal)
                || !prepared.created().activation().context.abortRelocation(seal)) {
            throw new IllegalStateException("reserved relocation target ingress hold was lost");
        }
    }

    CompletionStage<List<byte[]>> replayReserved(
            PreparedUserSpot prepared, ZLinkSpotAcceptedJournal.Record record) {
        requireNewPrepared(prepared);
        return prepared.created().activation().replayAccepted(record);
    }

    void stageReservedTimers(PreparedUserSpot prepared, byte[] timerEnvelope) {
        requireNewPrepared(prepared);
        prepared.created().activation().context.stageTimerRelocationEnvelope(timerEnvelope);
    }

    void publishReservedTimers(PreparedUserSpot prepared) {
        requireNewPrepared(prepared);
        prepared.created().activation().context.publishStagedTimerRelocation();
    }

    void stageReservedActorTimers(PreparedUserSpot prepared, String actorId, byte[] timerEnvelope) {
        requireNewPrepared(prepared);
        prepared.created()
                .activation()
                .context
                .stageActorTimerRelocationEnvelope(actorId, timerEnvelope);
    }

    void publishReservedActorTimers(PreparedUserSpot prepared, String actorId) {
        requireNewPrepared(prepared);
        prepared.created().activation().context.publishStagedActorTimerRelocation(actorId);
    }

    private static void requireNewPrepared(PreparedUserSpot prepared) {
        if (prepared == null || prepared.existing()) {
            throw new IllegalStateException("relocation staging requires a new prepared User Spot");
        }
    }

    CompletionStage<Boolean> closeReserved(String spotId, long objectGeneration) {
        requireSpotId(spotId);
        SpotActivation current = spots.get(spotId);
        if (current == null) {
            return CompletableFuture.completedFuture(false);
        }
        if (current.backendSpot.lifecycleGeneration() != objectGeneration) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("User Spot generation is stale"));
        }
        if (actorOccupancy.hasActorsInSpot(spotId)) {
            return CompletableFuture.completedFuture(false);
        }
        if (!spots.remove(spotId, current)) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("User Spot is moving or closing"));
        }
        current.close();
        ZLinkRuntimeMetrics.add("zlink.spot.count", -1, Map.of("kind", "user"));
        ZLinkRuntimeMetrics.increment("zlink.spot.closed", Map.of("kind", "user"));
        return CompletableFuture.completedFuture(true);
    }

    CompletionStage<Void> completeRelocationSource(
            String spotId, long objectGeneration, Instant deadline) {
        requireSpotId(spotId);
        Objects.requireNonNull(deadline, "deadline");
        SpotActivation current = spots.get(spotId);
        if (current == null) {
            return CompletableFuture.completedFuture(null);
        }
        if (current.backendSpot.lifecycleGeneration() != objectGeneration) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException(
                            "User Spot generation changed before relocation cleanup"));
        }
        if (actorOccupancy.hasActorsInSpot(spotId)) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException("User Spot still has source Actor participants"));
        }
        if (!spots.remove(spotId, current)) {
            return CompletableFuture.failedFuture(
                    new IllegalStateException(
                            "User Spot relocation source changed during cleanup"));
        }
        current.close(ZLinkSpotCloseReason.RELOCATION_OUT, deadline);
        ZLinkRuntimeMetrics.add("zlink.spot.count", -1, Map.of("kind", "user"));
        ZLinkRuntimeMetrics.increment("zlink.spot.closed", Map.of("kind", "user"));
        return CompletableFuture.completedFuture(null);
    }

    CloseReadiness closeReadiness(String spotId, long objectGeneration) {
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
        List<CompletableFuture<Void>> barriers = new ArrayList<>();
        for (EntrySpotActivation activation : entrySpots) {
            barriers.add(activation.context.awaitAllLanes().toCompletableFuture());
        }
        for (SpotActivation activation : spots.values()) {
            barriers.add(activation.context.awaitAllLanes().toCompletableFuture());
        }
        return CompletableFuture.allOf(barriers.toArray(CompletableFuture[]::new));
    }

    CompletionStage<ZLinkActorCreateResponse> notifyEntrySpotActorCreated(
            RoutingId nodeRid, ZLinkActor actor, ZLinkMessage createRequest, Object createContext) {
        for (EntrySpotActivation activation : entrySpots) {
            if (activation.context.nodeRid().equals(nodeRid)) {
                return activation.notifyActorCreated(actor, createRequest, createContext);
            }
        }
        return CompletableFuture.completedFuture(ZLinkActorCreateResponse.accept());
    }

    ZLinkSpot<?> spotFor(String spotId) {
        SpotActivation activation = spots.get(spotId);
        return activation == null ? null : activation.spot();
    }

    SpotActivation spotActivationFor(String spotId) {
        return spots.get(spotId);
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
            String spotId, ZLinkActorSessionCoordinator actors) {
        SpotActivation activation = spots.get(spotId);
        if (activation == null) {
            throw new ZLinkConfigurationException("User Spot is not active locally: " + spotId);
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
        return closeAllAsync(Instant.now());
    }

    CompletionStage<Void> closeAllAsync(Instant deadline) {
        AtomicReference<RuntimeException> firstFailure = new AtomicReference<>();
        List<EntrySpotActivation> closingEntrySpots = List.copyOf(entrySpots);
        List<SpotActivation> closingSpots = List.copyOf(spots.values());
        for (EntrySpotActivation entrySpot : closingEntrySpots) {
            recordCloseFailure(firstFailure, closeComponent(() -> entrySpot.close(deadline), null));
        }
        for (SpotActivation spot : closingSpots) {
            recordCloseFailure(
                    firstFailure,
                    closeComponent(
                            () ->
                                    spot.close(
                                            systems.zlink.framework.spots.ZLinkSpotCloseReason
                                                    .HOST_SHUTDOWN,
                                            deadline),
                            null));
        }
        if (!entrySpots.isEmpty()) {
            ZLinkRuntimeMetrics.add(
                    "zlink.spot.count", -entrySpots.size(), Map.of("kind", "entry"));
        }
        entrySpots.clear();
        if (!spots.isEmpty()) {
            ZLinkRuntimeMetrics.add("zlink.spot.count", -spots.size(), Map.of("kind", "user"));
        }
        spots.clear();
        List<CompletableFuture<Void>> cleanups = new ArrayList<>();
        for (EntrySpotActivation entrySpot : closingEntrySpots) {
            cleanups.add(
                    locations
                            .releaseEntrySpotAsync(entrySpot.context.nodeRid())
                            .handle(
                                    (ignored, error) -> {
                                        recordCloseFailure(firstFailure, error);
                                        return (Void) null;
                                    })
                            .toCompletableFuture());
        }
        for (SpotActivation spot : closingSpots) {
            cleanups.add(
                    locations
                            .releaseUserSpotAsync(
                                    spot.context.nodeRid(), spot.backendSpot.spotId())
                            .handle(
                                    (ignored, error) -> {
                                        recordCloseFailure(firstFailure, error);
                                        return (Void) null;
                                    })
                            .toCompletableFuture());
        }
        return CompletableFuture.allOf(cleanups.toArray(CompletableFuture[]::new))
                .thenCompose(
                        ignored ->
                                firstFailure.get() == null
                                        ? CompletableFuture.completedFuture(null)
                                        : CompletableFuture.failedFuture(firstFailure.get()));
    }

    private static void recordCloseFailure(
            AtomicReference<RuntimeException> target, Throwable error) {
        if (error == null) {
            return;
        }
        Throwable value = error;
        while (value instanceof CompletionException && value.getCause() != null) {
            value = value.getCause();
        }
        RuntimeException failure =
                value instanceof RuntimeException runtime ? runtime : new RuntimeException(value);
        RuntimeException first = target.get();
        if (first == null) {
            target.compareAndSet(null, failure);
        } else {
            first.addSuppressed(failure);
        }
    }

    CompletionStage<Void> releaseRecreatableSpots() {
        return releaseRecreatableSpots(ZLinkSpotCloseReason.HOST_SHUTDOWN, Instant.now());
    }

    void notifyClosing(ZLinkSpotCloseReason reason, Instant deadline) {
        for (EntrySpotActivation activation : List.copyOf(entrySpots)) {
            activation.notifyClosing(deadline);
        }
        for (SpotActivation activation : List.copyOf(spots.values())) {
            activation.notifyClosing(reason, deadline);
        }
    }

    CompletionStage<Void> releaseRecreatableSpots(ZLinkSpotCloseReason reason, Instant deadline) {
        List<String> spotIds = List.copyOf(spots.keySet());
        for (String spotId : spotIds) {
            if (actorOccupancy.hasActorsInSpot(spotId)) {
                return CompletableFuture.failedFuture(
                        new IllegalStateException("recreatable spot still has actors: " + spotId));
            }
        }
        List<SpotActivation> released = new ArrayList<>(spotIds.size());
        for (String spotId : spotIds) {
            SpotActivation activation = spots.remove(spotId);
            if (activation != null) {
                released.add(activation);
            }
        }
        AtomicReference<RuntimeException> firstFailure = new AtomicReference<>();
        for (SpotActivation activation : released) {
            recordCloseFailure(
                    firstFailure, closeComponent(() -> activation.close(reason, deadline), null));
        }
        List<CompletableFuture<Void>> cleanups = new ArrayList<>(released.size());
        for (SpotActivation activation : released) {
            cleanups.add(
                    locations
                            .releaseUserSpotAsync(
                                    activation.context.nodeRid(), activation.backendSpot.spotId())
                            .handle(
                                    (ignored, error) -> {
                                        recordCloseFailure(firstFailure, error);
                                        ZLinkRuntimeMetrics.add(
                                                "zlink.spot.count", -1, Map.of("kind", "user"));
                                        ZLinkRuntimeMetrics.increment(
                                                "zlink.spot.closed", Map.of("kind", "user"));
                                        return (Void) null;
                                    })
                            .toCompletableFuture());
        }
        return CompletableFuture.allOf(cleanups.toArray(CompletableFuture[]::new))
                .thenCompose(
                        ignored ->
                                firstFailure.get() == null
                                        ? CompletableFuture.completedFuture(null)
                                        : CompletableFuture.failedFuture(firstFailure.get()));
    }

    boolean userSpotsDrained() {
        return spots.isEmpty();
    }

    int userSpotCount() {
        return spots.size();
    }

    /** User Spots whose activation the named MeshNode admitted. */
    int userSpotCount(String meshName) {
        return (int)
                spots.values().stream()
                        .filter(activation -> meshName.equals(activation.meshName()))
                        .count();
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
            ZLinkSpotIdValidator.requireValid(spotId);
        } catch (IllegalArgumentException error) {
            throw new ZLinkConfigurationException(error.getMessage(), error);
        }
    }

    private ZLinkInternalSpotNode spotNode(String meshName) {
        ZLinkInternalSpotNode node = meshSpotNodes.apply(meshName);
        if (node == null) {
            throw new ZLinkConfigurationException("RouteMesh is not configured: " + meshName);
        }
        return node;
    }

    record PreparedUserSpot(
            String spotId,
            long objectGeneration,
            String meshName,
            SpotActivationCreateResult created) {
        static PreparedUserSpot existing(String spotId, long objectGeneration, String meshName) {
            return new PreparedUserSpot(spotId, objectGeneration, meshName, null);
        }

        boolean existing() {
            return created == null;
        }
    }

    private static RuntimeException closeComponent(Runnable close, RuntimeException firstFailure) {
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
