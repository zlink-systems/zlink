package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.util.ArrayList;
import java.util.List;
import java.util.Objects;
import java.util.Optional;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.execution.ZLinkWorkerPool;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.runtime.internal.metrics.ZLinkRuntimeMetrics;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotHandlerRegistry;
import systems.zlink.framework.spots.ZLinkSpotOutbound;
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyCall;
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyCompletion;
import systems.zlink.framework.spots.ZLinkSpotRelocationReadyOutcome;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkWorkerCall;
import systems.zlink.framework.spots.ZLinkIoWorkerTask;
import systems.zlink.framework.spots.ZLinkWorkerTask;

final class DefaultEntrySpotContext implements ZLinkEntrySpotContext, SpotDispatchLine {
    private final ZLinkSpotContextHost host;
    private final ZLinkWorkerPool workerPool;
    private final ZLinkSpotHandlerLoader handlerLoader;
    private final RoutingId nodeRid;
    private final ZLinkBackendSpot backendSpot;
    private final DefaultSpotOutbound outbound;
    private final ZLinkAsyncSerialQueue dispatchQueue;
    private final ZLinkAsyncSerialQueue infrastructureQueue;
    private final ZLinkHandlerInstanceOwner handlerInstances;
    private final List<DefaultSpotContext> timerContexts = new ArrayList<>();
    private final java.util.Map<String, ZLinkSpotTimerRegistry> actorTimers =
        new java.util.concurrent.ConcurrentHashMap<>();
    private final ZLinkSpotHandlerCatalog handlerCatalog = new ZLinkSpotHandlerCatalog(
        "EntrySpot handler registration is only allowed while configure is running");
    private ZLinkEntrySpot<?> entrySpot;

    DefaultEntrySpotContext(
        ZLinkSpotContextHost host,
        ZLinkWorkerPool workerPool,
        ZLinkSpotHandlerLoader handlerLoader,
        RoutingId nodeRid,
        ZLinkBackendSpot backendSpot) {
        this.host = host;
        this.workerPool = workerPool;
        this.handlerLoader = handlerLoader;
        this.nodeRid = nodeRid;
        this.backendSpot = backendSpot;
        this.dispatchQueue = new ZLinkAsyncSerialQueue(
            host.serialExecutor(), false);
        this.infrastructureQueue = new ZLinkAsyncSerialQueue(
            host.infrastructureExecutor(), false);
        this.outbound = host.createContextOutbound(backendSpot, nodeRid);
        this.handlerInstances = host.createHandlerInstances();
    }

    @Override public String spotId() { return backendSpot.spotId(); }
    @Override public long objectGeneration() {
        return backendSpot.lifecycleGeneration();
    }
    @Override public RoutingId nodeRid() { return nodeRid; }
    @Override public ZLinkSpotOutbound outbound() { return outbound; }
    @Override public DefaultSpotOutbound dispatchOutbound() { return outbound; }
    @Override public ZLinkSpotHandlerRegistry handlers() { return handlerCatalog; }
    @Override public ZLinkSpotHandlerCatalog handlerCatalog() { return handlerCatalog; }

    void setEntrySpot(ZLinkEntrySpot<?> entrySpot) {
        this.entrySpot = entrySpot;
        actorTimers.values().forEach(
            timer -> timer.setSpot(new ZLinkEntrySpotTimerSurface(this)));
    }

    @Override
    public CompletionStage<Void> destroyActor(ZLinkActor actor) {
        return host.destroyActorFromEntry(nodeRid, actor);
    }

    @Override
    public CompletionStage<ZLinkTimer> addTimer(
        String name,
        Duration period,
        Class<?> handlerType,
        ZLinkTimerOptions options) {
        String actorId = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentActorDispatch();
        if (actorId != null) {
            return actorTimer(actorId).add(
                name, period, handlerType, options);
        }
        DefaultSpotContext timerContext = new DefaultSpotContext(
            host,
            workerPool,
            handlerLoader,
            nodeRid,
            backendSpot,
            dispatchQueue,
            ZLinkUserSpotExecutionMode.PER_ACTOR,
            false,
            handlerInstances);
        timerContext.setSpot(new ZLinkEntrySpotTimerSurface(this));
        timerContexts.add(timerContext);
        return timerContext.addTimer(name, period, handlerType, options);
    }

    void closeTimers() {
        timerContexts.forEach(DefaultSpotContext::closeTimers);
        actorTimers.values().forEach(ZLinkSpotTimerRegistry::close);
        actorTimers.clear();
    }

    @Override
    public ZLinkHandlerInstanceOwner handlerInstances() {
        return handlerInstances;
    }

    void closeHandlerInstances() {
        handlerInstances.close();
    }

    void sealTimerAdmission() {
        timerContexts.forEach(DefaultSpotContext::sealTimerAdmission);
        actorTimers.values().forEach(ZLinkSpotTimerRegistry::freeze);
    }

    CompletionStage<Void> awaitAllLanes() {
        List<CompletionStage<Void>> lanes = new ArrayList<>();
        lanes.add(dispatchQueue.awaitQuiescence());
        lanes.add(infrastructureQueue.awaitQuiescence());
        timerContexts.forEach(context -> lanes.add(context.awaitAllLanes()));
        return java.util.concurrent.CompletableFuture.allOf(
            lanes.stream()
                .map(CompletionStage::toCompletableFuture)
                .toArray(java.util.concurrent.CompletableFuture[]::new));
    }

    @Override
    public CompletionStage<Void> enqueueDispatch(
        Supplier<CompletionStage<Void>> operation) {
        return enqueueDispatch(0, operation);
    }

    @Override
    public CompletionStage<Void> enqueueDispatch(
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        return dispatchQueue.enqueueWithPayloadBytes(
            payloadBytes,
            () -> runApplicationExecution(null, false,
                () -> host.runEntryDispatch(this, operation)));
    }

    @Override
    public CompletionStage<Void> enqueueInfrastructureDispatch(
        Supplier<CompletionStage<Void>> operation) {
        Objects.requireNonNull(operation, "operation");
        return infrastructureQueue.enqueueWithPayloadBytes(
            0,
            () -> host.runEntryDispatch(this, operation));
    }

    @Override
    public CompletionStage<Void> enqueueActorDispatch(
        String actorId,
        Supplier<CompletionStage<Void>> operation) {
        return enqueueActorDispatch(actorId, 0, operation);
    }

    @Override
    public CompletionStage<Void> enqueueActorDispatch(
        String actorId,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        Objects.requireNonNull(actorId, "actorId");
        return host.enqueueActorDispatch(
            actorId,
            payloadBytes,
            () -> runApplicationExecution(actorId, false, operation));
    }

    @Override
    public <T> ZLinkWorkerCall<T> runCpuWorker(ZLinkWorkerTask<T> work) {
        Objects.requireNonNull(work, "work");
        return new DefaultZLinkWorkerCall<>(workerPool, work);
    }

    @Override
    public <T> ZLinkWorkerCall<T> runIoWorker(ZLinkIoWorkerTask<T> work) {
        Objects.requireNonNull(work, "work");
        return new DefaultZLinkIoWorkerCall<>(workerPool, work);
    }

    private CompletionStage<Void> runApplicationExecution(
        String actorId,
        boolean yieldAllowed,
        Supplier<CompletionStage<Void>> operation) {
        var execution = new systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.ApplicationExecution(
                spotId(),
                actorId,
                false,
                yieldAllowed,
                ignored -> false);
        try (var ignored = systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.enterApplicationExecution(execution)) {
            return systems.zlink.framework.runtime.actors
                .ZLinkDeferredActorJoinHandlerScope.run(
                    host.deferredActorJoinRuntimeScope(),
                    candidate -> host.isActorAtSpot(candidate, spotId()),
                    operation);
        } catch (RuntimeException failure) {
            return java.util.concurrent.CompletableFuture.failedFuture(failure);
        }
    }

    byte[] freezeActorTimerRelocationEnvelope(String actorId) {
        ZLinkSpotTimerRegistry registry = actorTimers.get(actorId);
        return ZLinkSpotTimerRelocationEnvelope.encode(
            registry == null
                ? new ZLinkSpotTimerRegistry.FrozenTimers(List.of())
                : registry.freeze());
    }

    void resumeActorTimersAfterRelocationAbort(String actorId) {
        ZLinkSpotTimerRegistry registry = actorTimers.get(actorId);
        if (registry != null) {
            registry.resume();
        }
    }

    void stageActorTimerRelocationEnvelope(
        String actorId,
        byte[] envelope) {
        actorTimer(actorId).stageRestore(
            ZLinkSpotTimerRelocationEnvelope.decode(
                envelope,
                name -> loadActorTimerHandler(
                    entrySpot.getClass().getClassLoader(), name)));
    }

    void publishStagedActorTimerRelocation(String actorId) {
        actorTimer(actorId).publishStagedRestore();
    }

    void closeActorTimers(String actorId) {
        ZLinkSpotTimerRegistry registry = actorTimers.remove(actorId);
        if (registry != null) {
            registry.close();
        }
    }

    private ZLinkSpotTimerRegistry actorTimer(String actorId) {
        return actorTimers.computeIfAbsent(actorId, key -> {
            ZLinkSpotTimerRegistry timer = host.createTimerRegistry(
                spotId(),
                handlerInstances,
                (timerName, operation) -> host.runActorTimerDispatch(
                    key,
                    () -> runApplicationExecution(
                        key,
                        false,
                        () -> host.runWithOutbound(outbound, operation))));
            if (entrySpot != null) {
                timer.setSpot(new ZLinkEntrySpotTimerSurface(this));
            }
            return timer;
        });
    }

    void closeRegistration() {
        handlerCatalog.closeRegistration(handlerTypes -> handlerLoader.load(
            entrySpot.getClass(),
            handlerTypes,
            this::addTimer));
    }

    void bindSubscriptions(ZLinkBackendSpot spot) {
        for (String topic : handlerCatalog.subscriptionTopics()) {
            spot.setSubscription(topic);
        }
    }

    private static Class<?> loadActorTimerHandler(
        ClassLoader loader,
        String name) {
        try {
            return Class.forName(name, false, loader);
        } catch (ClassNotFoundException error) {
            throw new systems.zlink.framework.errors.ZLinkConfigurationException(
                "timer handler is not available on the relocation target: "
                    + name,
                error);
        }
    }
}

final class DefaultSpotContext implements ZLinkSpotContext, SpotDispatchLine {
    private static final boolean STREAM_TRACE =
        "1".equals(System.getenv("ZLINK_JAVA_STREAM_TRACE"));
    private static final java.util.logging.Logger LOGGER =
        java.util.logging.Logger.getLogger(DefaultSpotContext.class.getName());
    private final ZLinkSpotContextHost host;
    private final ZLinkWorkerPool workerPool;
    private final ZLinkSpotHandlerLoader handlerLoader;
    private final RoutingId nodeRid;
    private final ZLinkBackendSpot backendSpot;
    private final DefaultSpotOutbound outbound;
    private final ZLinkSpotTimerRegistry timers;
    private final ZLinkHandlerInstanceOwner handlerInstances;
    private final ZLinkAsyncSerialQueue dispatchQueue;
    private final ZLinkAsyncSerialQueue infrastructureQueue;
    private final java.util.concurrent.ConcurrentHashMap<
        String, ZLinkAsyncSerialQueue> timerQueues =
            new java.util.concurrent.ConcurrentHashMap<>();
    private final java.util.Map<String, ZLinkSpotTimerRegistry> actorTimers =
        new java.util.concurrent.ConcurrentHashMap<>();
    private final ZLinkUserSpotExecutionMode executionMode;
    private final ZLinkSpotRelocationReadinessMode relocationReadiness;
    private final boolean instanceSpot;
    private final Object relocationReadyLock = new Object();
    private RelocationReadyWaiter relocationReadyWaiter;
    private ZLinkUserSpotRelocationBarrier relocationBarrier;
    private final ZLinkSpotHandlerCatalog handlerCatalog = new ZLinkSpotHandlerCatalog(
        "SPOT handler registration is only allowed while configure is running");
    private ZLinkSpot<?> spot;

    DefaultSpotContext(
        ZLinkSpotContextHost host,
        ZLinkWorkerPool workerPool,
        ZLinkSpotHandlerLoader handlerLoader,
        RoutingId nodeRid,
        ZLinkBackendSpot backendSpot) {
        this(
            host,
            workerPool,
            handlerLoader,
            nodeRid,
            backendSpot,
            new ZLinkAsyncSerialQueue(host.serialExecutor(), false),
            ZLinkUserSpotExecutionMode.SPOT_WIDE,
            false);
    }

    DefaultSpotContext(
        ZLinkSpotContextHost host,
        ZLinkWorkerPool workerPool,
        ZLinkSpotHandlerLoader handlerLoader,
        RoutingId nodeRid,
        ZLinkBackendSpot backendSpot,
        ZLinkAsyncSerialQueue dispatchQueue) {
        this(
            host,
            workerPool,
            handlerLoader,
            nodeRid,
            backendSpot,
            dispatchQueue,
            ZLinkUserSpotExecutionMode.SPOT_WIDE,
            false);
    }

    DefaultSpotContext(
        ZLinkSpotContextHost host,
        ZLinkWorkerPool workerPool,
        ZLinkSpotHandlerLoader handlerLoader,
        RoutingId nodeRid,
        ZLinkBackendSpot backendSpot,
        ZLinkAsyncSerialQueue dispatchQueue,
        ZLinkUserSpotExecutionMode executionMode,
        boolean instanceSpot) {
        this(
            host,
            workerPool,
            handlerLoader,
            nodeRid,
            backendSpot,
            dispatchQueue,
            executionMode,
            instanceSpot,
            null,
            ZLinkSpotRelocationReadinessMode.ANY_TURN_BOUNDARY);
    }

    DefaultSpotContext(
        ZLinkSpotContextHost host,
        ZLinkWorkerPool workerPool,
        ZLinkSpotHandlerLoader handlerLoader,
        RoutingId nodeRid,
        ZLinkBackendSpot backendSpot,
        ZLinkAsyncSerialQueue dispatchQueue,
        ZLinkUserSpotExecutionMode executionMode,
        boolean instanceSpot,
        ZLinkHandlerInstanceOwner sharedHandlerInstances) {
        this(
            host,
            workerPool,
            handlerLoader,
            nodeRid,
            backendSpot,
            dispatchQueue,
            executionMode,
            instanceSpot,
            sharedHandlerInstances,
            ZLinkSpotRelocationReadinessMode.ANY_TURN_BOUNDARY);
    }

    DefaultSpotContext(
        ZLinkSpotContextHost host,
        ZLinkWorkerPool workerPool,
        ZLinkSpotHandlerLoader handlerLoader,
        RoutingId nodeRid,
        ZLinkBackendSpot backendSpot,
        ZLinkAsyncSerialQueue dispatchQueue,
        ZLinkUserSpotExecutionMode executionMode,
        boolean instanceSpot,
        ZLinkHandlerInstanceOwner sharedHandlerInstances,
        ZLinkSpotRelocationReadinessMode relocationReadiness) {
        this.host = host;
        this.workerPool = workerPool;
        this.handlerLoader = handlerLoader;
        this.nodeRid = nodeRid;
        this.backendSpot = backendSpot;
        this.dispatchQueue = dispatchQueue;
        this.infrastructureQueue = new ZLinkAsyncSerialQueue(
            host.infrastructureExecutor(), false);
        this.executionMode = Objects.requireNonNull(executionMode, "executionMode");
        this.relocationReadiness = Objects.requireNonNull(
            relocationReadiness, "relocationReadiness");
        this.instanceSpot = instanceSpot;
        this.handlerInstances = sharedHandlerInstances == null
            ? host.createHandlerInstances()
            : sharedHandlerInstances;
        this.outbound = host.createContextOutbound(backendSpot, nodeRid);
        this.timers = host.createTimerRegistry(
            backendSpot.spotId(),
            handlerInstances,
            (timerName, operation) -> enqueueTimerDispatch(
                timerName,
                () -> host.runWithOutbound(outbound, operation)));
    }

    void setSpot(ZLinkSpot<?> spot) {
        this.spot = spot;
        timers.setSpot(spot);
        actorTimers.values().forEach(timer -> timer.setSpot(spot));
    }

    @Override public String spotId() { return backendSpot.spotId(); }
    @Override public long objectGeneration() {
        return backendSpot.lifecycleGeneration();
    }
    @Override public RoutingId nodeRid() { return nodeRid; }
    @Override public ZLinkSpotOutbound outbound() { return outbound; }
    @Override public ZLinkSpotRelocationReadyCall relocationReady() {
        return this::deferRelocationReady;
    }
    @Override public DefaultSpotOutbound dispatchOutbound() { return outbound; }
    @Override public ZLinkSpotHandlerRegistry handlers() { return handlerCatalog; }
    @Override public ZLinkSpotHandlerCatalog handlerCatalog() { return handlerCatalog; }

    @Override
    public CompletionStage<Void> leaveActor(ZLinkActor actor) {
        rejectAfterRelocationReady("leaveActor");
        return host.leaveActor(nodeRid, spot, actor, spotId());
    }

    @Override
    public CompletionStage<Boolean> close() {
        rejectAfterRelocationReady("close");
        return host.closeSpot(spotId());
    }

    @Override
    public CompletionStage<ZLinkTimer> addTimer(
        String name,
        Duration period,
        Class<?> handlerType,
        ZLinkTimerOptions options) {
        rejectAfterRelocationReady("addTimer");
        String actorId = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentActorDispatch();
        if (actorId != null) {
            return actorTimer(actorId).add(
                name, period, handlerType, options);
        }
        return timers.add(name, period, handlerType, options);
    }

    void closeTimers() {
        timers.close();
        actorTimers.values().forEach(ZLinkSpotTimerRegistry::close);
        actorTimers.clear();
    }

    @Override
    public ZLinkHandlerInstanceOwner handlerInstances() {
        return handlerInstances;
    }

    void closeHandlerInstances() {
        handlerInstances.close();
    }

    void sealTimerAdmission() {
        timers.freeze();
        actorTimers.values().forEach(ZLinkSpotTimerRegistry::freeze);
    }

    byte[] freezeTimerRelocationEnvelope() {
        return ZLinkSpotTimerRelocationEnvelope.encode(timers.freeze());
    }

    void resumeTimersAfterRelocationAbort() {
        timers.resume();
    }

    void restoreTimerRelocationEnvelope(byte[] envelope) {
        ClassLoader loader = spot.getClass().getClassLoader();
        timers.restore(ZLinkSpotTimerRelocationEnvelope.decode(
            envelope,
            name -> loadTimerHandler(loader, name)));
    }

    void stageTimerRelocationEnvelope(byte[] envelope) {
        ClassLoader loader = spot.getClass().getClassLoader();
        timers.stageRestore(ZLinkSpotTimerRelocationEnvelope.decode(
            envelope,
            name -> loadTimerHandler(loader, name)));
    }

    void publishStagedTimerRelocation() {
        timers.publishStagedRestore();
    }

    byte[] freezeActorTimerRelocationEnvelope(String actorId) {
        ZLinkSpotTimerRegistry registry = actorTimers.get(actorId);
        return ZLinkSpotTimerRelocationEnvelope.encode(
            registry == null
                ? new ZLinkSpotTimerRegistry.FrozenTimers(List.of())
                : registry.freeze());
    }

    void resumeActorTimersAfterRelocationAbort(String actorId) {
        ZLinkSpotTimerRegistry registry = actorTimers.get(actorId);
        if (registry != null) {
            registry.resume();
        }
    }

    void stageActorTimerRelocationEnvelope(
        String actorId,
        byte[] envelope) {
        actorTimer(actorId).stageRestore(
            ZLinkSpotTimerRelocationEnvelope.decode(
                envelope,
                name -> loadTimerHandler(
                    spot.getClass().getClassLoader(), name)));
    }

    void publishStagedActorTimerRelocation(String actorId) {
        actorTimer(actorId).publishStagedRestore();
    }

    void closeActorTimers(String actorId) {
        ZLinkSpotTimerRegistry registry = actorTimers.remove(actorId);
        if (registry != null) {
            registry.close();
        }
    }

    @Override
    public CompletionStage<Void> enqueueDispatch(
        Supplier<CompletionStage<Void>> operation) {
        return enqueueDispatch(0, operation);
    }

    @Override
    public CompletionStage<Void> enqueueDispatch(
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        streamTrace("dispatch-enqueue spot=" + spotId());
        return dispatchQueue.enqueueWithPayloadBytes(payloadBytes, () -> {
            streamTrace("dispatch-start spot=" + spotId());
            CompletionStage<Void> stage = runApplicationExecution(
                null,
                sharedSpotGate(),
                operation);
            stage.whenComplete((ignored, error) -> streamTrace(
                "dispatch-complete spot=" + spotId()
                    + " error=" + (error == null ? "none" : error)));
            return stage;
        });
    }

    @Override
    public CompletionStage<Void> enqueueInfrastructureDispatch(
        Supplier<CompletionStage<Void>> operation) {
        Objects.requireNonNull(operation, "operation");
        return infrastructureQueue.enqueueWithPayloadBytes(
            0,
            operation);
    }

    @Override
    public CompletionStage<Void> enqueueActorDispatch(
        String actorId,
        Supplier<CompletionStage<Void>> operation) {
        return enqueueActorDispatch(actorId, 0, operation);
    }

    @Override
    public CompletionStage<Void> enqueueActorDispatch(
        String actorId,
        long payloadBytes,
        Supplier<CompletionStage<Void>> operation) {
        Objects.requireNonNull(actorId, "actorId");
        streamTrace("actor-enqueue spot=" + spotId() + " actor=" + actorId
            + " shared=" + sharedSpotGate());
        CompletionStage<Void> queued = host.enqueueActorDispatch(
            actorId,
            payloadBytes,
            () -> {
                // The Actor queue owns payload admission. The shared Spot
                // gate reserves only its fixed turn cost here.
                return sharedSpotGate()
                    ? dispatchQueue.enqueue(
                        () -> runActorApplication(actorId, true, operation))
                    : runActorApplication(actorId, false, operation);
            });
        // A shared Spot turn may submit an Actor turn to the same gate. Yield
        // the current turn while that Actor turn acquires the gate; otherwise
        // the Actor queue would wait for the turn that is waiting for it.
        return sharedSpotGate() && dispatchQueue.isCurrent()
            ? ZLinkAsyncSerialQueue.yieldCurrent(queued)
            : queued;
    }

    private CompletionStage<Void> runActorApplication(
        String actorId,
        boolean yieldAllowed,
        Supplier<CompletionStage<Void>> operation) {
        streamTrace("actor-start spot=" + spotId()
            + " actor=" + actorId);
        CompletionStage<Void> stage;
        try (var ignored = systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.enterActorDispatch(actorId)) {
            stage = runApplicationExecution(actorId, yieldAllowed, operation);
        } catch (RuntimeException failure) {
            stage = CompletableFuture.failedFuture(failure);
        }
        stage.whenComplete((ignored, error) -> streamTrace(
            "actor-complete spot=" + spotId()
                + " actor=" + actorId
                + " error=" + (error == null ? "none" : error)));
        return stage;
    }

    CompletionStage<Void> enqueueAcceptedDispatch(
        byte[] acceptedJournalRecord,
        Supplier<CompletionStage<Void>> operation,
        Runnable relocationRelease) {
        return dispatchQueue.enqueueRelocatable(
            acceptedJournalRecord,
            () -> runApplicationExecution(
                null,
                sharedSpotGate(),
                operation),
            relocationRelease);
    }

    CompletionStage<Void> enqueueLifecycle(
        Supplier<CompletionStage<Void>> operation) {
        boolean dispatchTurnAlreadyOwned = dispatchQueue.isCurrent();
        CompletionStage<Void> barrier;
        if (sharedSpotGate() || timerQueues.isEmpty()) {
            barrier = java.util.concurrent.CompletableFuture.completedFuture(null);
        } else {
            barrier = java.util.concurrent.CompletableFuture.allOf(
                timerQueues.values().stream()
                    .map(queue -> queue.enqueue(() ->
                        java.util.concurrent.CompletableFuture.completedFuture(null))
                        .toCompletableFuture())
                    .toArray(java.util.concurrent.CompletableFuture[]::new));
        }
        CompletionStage<Void> queued = barrier.thenCompose(ignored ->
            dispatchQueue.enqueueLifecycleBarrier(() ->
                runLifecycleExecution(operation)));
        // Lifecycle dispatch can be requested while the current Spot turn is
        // still composing its event. Yield that turn before waiting for the
        // lifecycle entry, otherwise the queued entry cannot acquire the
        // shared gate that the current turn is holding.
        return dispatchTurnAlreadyOwned
            ? ZLinkAsyncSerialQueue.yieldCurrent(queued)
            : queued;
    }

    @Override
    public boolean usesSharedExecutionGate() {
        return sharedSpotGate();
    }

    CompletionStage<Void> awaitAllLanes() {
        List<CompletionStage<Void>> lanes = new ArrayList<>();
        lanes.add(dispatchQueue.awaitQuiescence());
        lanes.add(infrastructureQueue.awaitQuiescence());
        timerQueues.values().forEach(
            queue -> lanes.add(queue.awaitQuiescence()));
        return java.util.concurrent.CompletableFuture.allOf(
            lanes.stream()
                .map(CompletionStage::toCompletableFuture)
                .toArray(java.util.concurrent.CompletableFuture[]::new));
    }

    java.util.Map<String, ZLinkAsyncSerialQueue> relocationLanes() {
        java.util.LinkedHashMap<String, ZLinkAsyncSerialQueue> lanes =
            new java.util.LinkedHashMap<>();
        lanes.put("spot", dispatchQueue);
        timerQueues.entrySet().stream()
            .sorted(java.util.Map.Entry.comparingByKey())
            .forEach(entry -> lanes.put(
                "timer:" + entry.getKey(), entry.getValue()));
        return java.util.Collections.unmodifiableMap(lanes);
    }

    synchronized ZLinkUserSpotRelocationBarrier relocationBarrier(
        ZLinkActorSessionCoordinator actors) {
        if (relocationBarrier == null) {
            relocationBarrier =
                new ZLinkUserSpotRelocationBarrier(this, actors);
        }
        return relocationBarrier;
    }

    <T> CompletionStage<T> runLifecycleExecution(
        Supplier<CompletionStage<T>> operation) {
        var execution = new systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.ApplicationExecution(
                spotId(),
                null,
                sharedSpotGate(),
                lifecycleYieldAllowed(),
                relocationReadyAllowed(),
                candidate -> host.isActorMember(spotId(), candidate));
        try (var ignored = systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.enterApplicationExecution(execution)) {
            streamTrace("lifecycle-start spot=" + spotId());
            CompletionStage<T> stage = Objects.requireNonNull(
                operation.get(), "operation result");
            stage.whenComplete((ignoredValue, error) -> streamTrace(
                "lifecycle-complete spot=" + spotId()
                    + " error=" + (error == null ? "none" : error)));
            return stage;
        } catch (RuntimeException failure) {
            return java.util.concurrent.CompletableFuture.failedFuture(failure);
        }
    }

    Optional<ZLinkAsyncSerialQueue.RelocationSeal> trySealRelocation() {
        return dispatchQueue.trySealRelocation();
    }

    boolean abortRelocation(ZLinkAsyncSerialQueue.RelocationSeal seal) {
        return dispatchQueue.abortRelocation(seal);
    }

    Optional<List<ZLinkAsyncSerialQueue.QueuedRecord>> commitRelocation(
        ZLinkAsyncSerialQueue.RelocationSeal seal) {
        return dispatchQueue.commitRelocation(seal);
    }

    @Override
    public <T> ZLinkWorkerCall<T> runCpuWorker(ZLinkWorkerTask<T> work) {
        rejectAfterRelocationReady("runCpuWorker");
        Objects.requireNonNull(work, "work");
        return new DefaultZLinkWorkerCall<>(workerPool, work);
    }

    @Override
    public <T> ZLinkWorkerCall<T> runIoWorker(ZLinkIoWorkerTask<T> work) {
        rejectAfterRelocationReady("runIoWorker");
        Objects.requireNonNull(work, "work");
        return new DefaultZLinkIoWorkerCall<>(workerPool, work);
    }

    ZLinkUserSpotExecutionMode executionMode() {
        return executionMode;
    }

    ZLinkSpotRelocationReadinessMode relocationReadiness() {
        return relocationReadiness;
    }

    CompletionStage<Optional<ZLinkUserSpotRelocationBarrier.Seal>>
        awaitRelocationReadySignal(
            Supplier<Optional<ZLinkUserSpotRelocationBarrier.Seal>> claim,
            java.util.function.BooleanSupplier cancelled) {
        Objects.requireNonNull(claim, "claim");
        Objects.requireNonNull(cancelled, "cancelled");
        if (relocationReadiness
                != ZLinkSpotRelocationReadinessMode.APPLICATION_SIGNALED
            || executionMode != ZLinkUserSpotExecutionMode.SPOT_WIDE
            || instanceSpot) {
            return java.util.concurrent.CompletableFuture.failedFuture(
                invalidRelocationReady(
                    "application-signaled relocation is not configured"));
        }
        RelocationReadyWaiter waiter =
            new RelocationReadyWaiter(claim, cancelled);
        synchronized (relocationReadyLock) {
            if (relocationReadyWaiter != null) {
                return java.util.concurrent.CompletableFuture.failedFuture(
                    new IllegalStateException(
                        "a relocation readiness waiter is already active"));
            }
            if (cancelled.getAsBoolean()) {
                return java.util.concurrent.CompletableFuture.completedFuture(
                    Optional.empty());
            }
            relocationReadyWaiter = waiter;
        }
        pollRelocationReadyCancellation(waiter);
        return waiter.result;
    }

    CompletionStage<Void> runRelocationReadyCompletion(
        ZLinkSpotRelocationReadyOutcome outcome) {
        if (relocationReadiness
            != ZLinkSpotRelocationReadinessMode.APPLICATION_SIGNALED) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }
        return runLifecycleExecution(() ->
            spot.onRelocationReadyCompleted(
                new ZLinkSpotRelocationReadyCompletion(outcome)));
    }

    private CompletionStage<Void> enqueueTimerDispatch(
        String timerName,
        Supplier<CompletionStage<Void>> operation) {
        if (sharedSpotGate()) {
            return enqueueDispatch(operation);
        }
        ZLinkAsyncSerialQueue queue = timerQueues.computeIfAbsent(
            Objects.requireNonNull(timerName, "timerName"),
            ignored -> new ZLinkAsyncSerialQueue(host.serialExecutor(), false));
        return queue.enqueue(() -> runApplicationExecution(
            null,
            false,
            operation));
    }

    private ZLinkSpotTimerRegistry actorTimer(String actorId) {
        return actorTimers.computeIfAbsent(actorId, key -> {
            ZLinkSpotTimerRegistry timer = host.createTimerRegistry(
                spotId(),
                handlerInstances,
                (timerName, operation) -> host.runActorTimerDispatch(
                    key,
                    () -> runApplicationExecution(
                        key,
                        false,
                        () -> host.runWithOutbound(outbound, operation))));
            if (spot != null) {
                timer.setSpot(spot);
            }
            return timer;
        });
    }

    private boolean sharedSpotGate() {
        return instanceSpot
            || executionMode == ZLinkUserSpotExecutionMode.SPOT_WIDE;
    }

    private boolean lifecycleYieldAllowed() {
        return lifecycleYieldAllowed(executionMode, instanceSpot);
    }

    private boolean relocationReadyAllowed() {
        return relocationReadyAllowed(
            executionMode,
            relocationReadiness,
            instanceSpot);
    }

    static boolean lifecycleYieldAllowed(
        ZLinkUserSpotExecutionMode executionMode,
        boolean instanceSpot) {
        return instanceSpot
            || executionMode == ZLinkUserSpotExecutionMode.SPOT_WIDE;
    }

    static boolean relocationReadyAllowed(
        ZLinkUserSpotExecutionMode executionMode,
        ZLinkSpotRelocationReadinessMode relocationReadiness,
        boolean instanceSpot) {
        return !instanceSpot
            && executionMode == ZLinkUserSpotExecutionMode.SPOT_WIDE
            && relocationReadiness
                == ZLinkSpotRelocationReadinessMode.APPLICATION_SIGNALED;
    }

    private CompletionStage<Void> runApplicationExecution(
        String actorId,
        boolean yieldAllowed,
        Supplier<CompletionStage<Void>> operation) {
        var execution = new systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.ApplicationExecution(
                spotId(),
                actorId,
                sharedSpotGate(),
                yieldAllowed,
                relocationReadyAllowed(),
                candidate -> host.isActorMember(spotId(), candidate));
        try (var ignored = systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.enterApplicationExecution(execution)) {
            if (instanceSpot) {
                return Objects.requireNonNull(operation.get(), "operation result");
            }
            return systems.zlink.framework.runtime.actors
                .ZLinkDeferredActorJoinHandlerScope.run(
                    host.deferredActorJoinRuntimeScope(),
                    candidate -> host.isActorMember(spotId(), candidate),
                    operation);
        } catch (RuntimeException failure) {
            return java.util.concurrent.CompletableFuture.failedFuture(failure);
        }
    }

    void closeRegistration() {
        handlerCatalog.closeRegistration(handlerTypes -> handlerLoader.load(
            spot.getClass(),
            handlerTypes,
            this::addTimer));
    }

    void bindSubscriptions(ZLinkBackendSpot spot) {
        for (String topic : handlerCatalog.subscriptionTopics()) {
            spot.setSubscription(topic);
        }
    }

    private static Class<?> loadTimerHandler(
        ClassLoader loader,
        String name) {
        try {
            return Class.forName(name, false, loader);
        } catch (ClassNotFoundException error) {
            throw new systems.zlink.framework.errors.ZLinkConfigurationException(
                "timer handler is not available on the relocation target: "
                    + name,
                error);
        }
    }

    private void deferRelocationReady() {
        var execution = systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.currentApplicationExecution();
        if (execution == null
            || !execution.relocationReadyAllowed()
            || !execution.spotId().equals(spotId())) {
            throw invalidRelocationReady(
                "relocationReady().defer() is only valid in a SpotWide "
                    + "ApplicationSignaled User Spot turn");
        }
        if (!execution.tryDeferRelocationReady()) {
            throw invalidRelocationReady(
                "relocationReady().defer() can be called once per Spot turn");
        }
        streamTrace("relocation-ready-deferred spot=" + spotId());
        dispatchQueue.enqueueBarrierNext(this::reachRelocationReadyBoundary);
    }

    private CompletionStage<Void> reachRelocationReadyBoundary() {
        streamTrace("relocation-ready-boundary-start spot=" + spotId());
        RelocationReadyWaiter waiter;
        synchronized (relocationReadyLock) {
            waiter = relocationReadyWaiter;
            relocationReadyWaiter = null;
        }
        if (waiter == null || waiter.cancelled.getAsBoolean()) {
            if (waiter != null) {
                waiter.result.complete(Optional.empty());
            }
            CompletionStage<Void> continued = runRelocationReadyCompletion(
                ZLinkSpotRelocationReadyOutcome.CONTINUED);
            continued.whenComplete((ignored, error) -> streamTrace(
                "relocation-ready-boundary-complete spot=" + spotId()
                    + " error=" + (error == null ? "none" : error)));
            return continued;
        }
        Optional<ZLinkUserSpotRelocationBarrier.Seal> claimed;
        try {
            claimed = Objects.requireNonNull(
                waiter.claim.get(), "relocation readiness claim");
        } catch (RuntimeException failure) {
            waiter.result.completeExceptionally(failure);
            return java.util.concurrent.CompletableFuture.failedFuture(failure);
        }
        waiter.result.complete(claimed);
        CompletionStage<Void> completed = claimed.isPresent()
            ? java.util.concurrent.CompletableFuture.completedFuture(null)
            : runRelocationReadyCompletion(
                ZLinkSpotRelocationReadyOutcome.CONTINUED);
        completed.whenComplete((ignored, error) -> streamTrace(
            "relocation-ready-boundary-complete spot=" + spotId()
                + " claimed=" + claimed.isPresent()
                + " error=" + (error == null ? "none" : error)));
        return completed;
    }

    private static void streamTrace(String message) {
        if (STREAM_TRACE) {
            LOGGER.warning("[zlink-java-stream-trace] spot-context " + message);
        }
    }

    private void pollRelocationReadyCancellation(
        RelocationReadyWaiter waiter) {
        java.util.concurrent.CompletableFuture.delayedExecutor(
            25, java.util.concurrent.TimeUnit.MILLISECONDS)
            .execute(() -> {
                if (waiter.result.isDone()) {
                    return;
                }
                if (waiter.cancelled.getAsBoolean()) {
                    synchronized (relocationReadyLock) {
                        if (relocationReadyWaiter == waiter) {
                            relocationReadyWaiter = null;
                        }
                    }
                    waiter.result.complete(Optional.empty());
                    return;
                }
                pollRelocationReadyCancellation(waiter);
            });
    }

    private static ZLinkFrameworkException invalidRelocationReady(
        String message) {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.NOT_CONFIGURED,
            message);
    }

    private static void rejectAfterRelocationReady(String operation) {
        systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.rejectAfterRelocationReady(
                operation);
    }

    private static final class RelocationReadyWaiter {
        private final Supplier<Optional<
            ZLinkUserSpotRelocationBarrier.Seal>> claim;
        private final java.util.function.BooleanSupplier cancelled;
        private final java.util.concurrent.CompletableFuture<Optional<
            ZLinkUserSpotRelocationBarrier.Seal>> result =
                new java.util.concurrent.CompletableFuture<>();

        RelocationReadyWaiter(
            Supplier<Optional<ZLinkUserSpotRelocationBarrier.Seal>> claim,
            java.util.function.BooleanSupplier cancelled) {
            this.claim = claim;
            this.cancelled = cancelled;
        }
    }
}
