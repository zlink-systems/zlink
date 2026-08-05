package systems.zlink.framework.runtime.spots;

import java.time.Duration;
import java.util.Objects;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.Supplier;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.execution.ZLinkAsyncSerialQueue;
import systems.zlink.framework.execution.ZLinkWorkerPool;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerInstanceOwner;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkInstanceSpotHandlerRegistry;
import systems.zlink.framework.spots.ZLinkIoWorkerTask;
import systems.zlink.framework.spots.ZLinkSpotOutbound;
import systems.zlink.framework.spots.ZLinkTimer;
import systems.zlink.framework.spots.ZLinkTimerOptions;
import systems.zlink.framework.spots.ZLinkWorkerCall;
import systems.zlink.framework.spots.ZLinkWorkerTask;

final class DefaultInstanceSpotContext
    implements ZLinkInstanceSpotContext, SpotDispatchLine {
    private final ZLinkSpotContextHost host;
    private final ZLinkWorkerPool workerPool;
    private final ZLinkSpotHandlerLoader handlerLoader;
    private final String meshName;
    private final RoutingId nodeRid;
    private final ZLinkBackendSpot backendSpot;
    private final DefaultSpotOutbound outbound;
    private final ZLinkHandlerInstanceOwner handlerInstances;
    private final ZLinkAsyncSerialQueue dispatchQueue;
    private final ZLinkAsyncSerialQueue infrastructureQueue;
    private final ZLinkSpotHandlerCatalog handlers = new ZLinkSpotHandlerCatalog(
        "Instance Spot handler registration is only allowed while configure is running");
    private final ZLinkInstanceSpotHandlerRegistry publicHandlers =
        handlerType -> handlers.addHandler(handlerType);
    private final ZLinkSpotTimerRegistry timers;
    private ZLinkInstanceSpot spot;

    DefaultInstanceSpotContext(
        ZLinkSpotContextHost host,
        ZLinkWorkerPool workerPool,
        ZLinkSpotHandlerLoader handlerLoader,
        String meshName,
        RoutingId nodeRid,
        ZLinkBackendSpot backendSpot) {
        this.host = Objects.requireNonNull(host, "host");
        this.workerPool = Objects.requireNonNull(workerPool, "workerPool");
        this.handlerLoader = Objects.requireNonNull(handlerLoader, "handlerLoader");
        this.meshName = Objects.requireNonNull(meshName, "meshName");
        this.nodeRid = Objects.requireNonNull(nodeRid, "nodeRid");
        this.backendSpot = Objects.requireNonNull(backendSpot, "backendSpot");
        this.dispatchQueue = new ZLinkAsyncSerialQueue(
            host.serialExecutor(), false);
        this.infrastructureQueue = new ZLinkAsyncSerialQueue(
            host.infrastructureExecutor(), false);
        this.handlerInstances = host.createHandlerInstances();
        this.outbound = host.createContextOutbound(backendSpot, nodeRid);
        this.timers = host.createTimerRegistry(
            backendSpot.spotId(),
            handlerInstances,
            (timerName, operation) -> enqueueDispatch(() -> host.runWithOutbound(
                outbound, operation)));
    }

    void bind(ZLinkInstanceSpot value) {
        spot = Objects.requireNonNull(value, "spot");
        timers.setSpot(value);
    }

    void closeRegistration(Class<?> spotType) {
        handlers.closeRegistration(configured -> handlerLoader.load(
            spotType,
            configured,
            (name, period, handlerType, options) -> timers.add(
                name, period, handlerType, options)));
    }

    CompletionStage<Void> runLifecycle(
        Supplier<CompletionStage<Void>> operation) {
        return enqueueDispatch(() -> host.runWithOutbound(outbound, operation));
    }

    void closeResources() {
        timers.close();
        handlerInstances.close();
        backendSpot.close();
    }

    CompletionStage<Void> awaitQuiescence() {
        return CompletableFuture.allOf(
            dispatchQueue.awaitQuiescence().toCompletableFuture(),
            infrastructureQueue.awaitQuiescence().toCompletableFuture());
    }

    boolean hasActiveTimers() {
        return timers.hasActiveTimers();
    }

    <T> CompletionStage<T> runLifecycleExecution(
        Supplier<CompletionStage<T>> operation) {
        var execution = new systems.zlink.framework.runtime.internal.handlers
            .ZLinkSuspendInvocationContext.ApplicationExecution(
                spotId(),
                null,
                true,
                true,
                false,
                ignored -> false);
        try (var ignored = systems.zlink.framework.runtime.internal.handlers
                 .ZLinkSuspendInvocationContext.enterApplicationExecution(execution)) {
            return host.runWithOutbound(outbound, operation);
        } catch (RuntimeException failure) {
            return CompletableFuture.failedFuture(failure);
        }
    }

    @Override public String meshName() { return meshName; }
    @Override public String spotId() { return backendSpot.spotId(); }
    @Override public long objectGeneration() {
        return backendSpot.lifecycleGeneration();
    }
    @Override public RoutingId nodeRid() { return nodeRid; }
    @Override public ZLinkInstanceSpotHandlerRegistry handlers() {
        return publicHandlers;
    }
    @Override public ZLinkSpotOutbound outbound() { return outbound; }
    @Override public DefaultSpotOutbound dispatchOutbound() { return outbound; }
    @Override public ZLinkSpotHandlerCatalog handlerCatalog() { return handlers; }
    @Override public ZLinkHandlerInstanceOwner handlerInstances() {
        return handlerInstances;
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
        return dispatchQueue.enqueueWithPayloadBytes(payloadBytes, operation);
    }

    @Override
    public CompletionStage<Void> enqueueInfrastructureDispatch(
        Supplier<CompletionStage<Void>> operation) {
        Objects.requireNonNull(operation, "operation");
        return infrastructureQueue.enqueueWithPayloadBytes(0, operation);
    }

    @Override
    public CompletionStage<Void> enqueueActorDispatch(
        String actorId,
        Supplier<CompletionStage<Void>> operation) {
        return CompletableFuture.failedFuture(new IllegalStateException(
            "Instance Spot does not own Actor dispatch"));
    }

    @Override
    public <T> ZLinkWorkerCall<T> runCpuWorker(ZLinkWorkerTask<T> work) {
        return new DefaultZLinkWorkerCall<>(workerPool, Objects.requireNonNull(work, "work"));
    }

    @Override
    public <T> ZLinkWorkerCall<T> runIoWorker(ZLinkIoWorkerTask<T> work) {
        return new DefaultZLinkIoWorkerCall<>(workerPool, Objects.requireNonNull(work, "work"));
    }

    @Override
    public CompletionStage<Boolean> close() {
        return host.closeInstanceSpot(spotId(), objectGeneration());
    }

    @Override
    public CompletionStage<ZLinkTimer> addTimer(
        String name,
        Duration period,
        Class<?> handlerType,
        ZLinkTimerOptions options) {
        return timers.add(name, period, handlerType, options);
    }
}
