package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.configuration.ZLinkSpotRelocationCoordinationMode;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.execution.ZLinkExecutionLanePolicy;
import systems.zlink.framework.execution.ZLinkSerialExecutionQueue;
import systems.zlink.framework.execution.ZLinkWorkerPool;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendReceived;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalAsyncSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkInstanceSpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;

import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;
import java.util.function.BiFunction;
import java.util.function.Function;

final class ZLinkSpotActivationFactory {
    private final ZLinkSpotRuntime host;
    private final ZLinkWorkerPool workerPool;
    private final ZLinkSpotHandlerLoader handlerLoader;
    private final ZLinkSpotHandlerInvoker handlerInvoker;
    private final ZLinkHandlerActivator handlerFactory;
    private final Map<Class<? extends ZLinkSpot<?>>, ZLinkUserSpotExecutionMode> executionModes;
    private final Map<Class<? extends ZLinkSpot<?>>, ZLinkSpotRelocationCoordinationMode>
            relocationCoordinationModes;

    ZLinkSpotActivationFactory(
            ZLinkSpotRuntime host,
            ZLinkWorkerPool workerPool,
            ZLinkSpotHandlerLoader handlerLoader,
            ZLinkSpotHandlerInvoker handlerInvoker,
            ZLinkHandlerActivator handlerFactory,
            Map<Class<? extends ZLinkSpot<?>>, ZLinkUserSpotExecutionMode> executionModes,
            Map<Class<? extends ZLinkSpot<?>>, ZLinkSpotRelocationCoordinationMode>
                    relocationCoordinationModes) {
        this.host = host;
        this.workerPool = workerPool;
        this.handlerLoader = handlerLoader;
        this.handlerInvoker = handlerInvoker;
        this.handlerFactory = handlerFactory;
        this.executionModes = Map.copyOf(executionModes);
        this.relocationCoordinationModes = Map.copyOf(relocationCoordinationModes);
    }

    /** Activates a User Spot on the MeshNode {@code nodeRid} that admitted it. */
    CompletionStage<SpotActivationCreateResult> activate(
            Class<? extends ZLinkSpot<?>> spotType,
            ZLinkBackendSpot backendSpot,
            ZLinkMessage request,
            RoutingId nodeRid) {
        ZLinkMessage effectiveRequest = request == null ? ZLinkMessage.empty() : request;
        DefaultSpotContext context =
                new DefaultSpotContext(
                        host,
                        workerPool,
                        handlerLoader,
                        nodeRid,
                        backendSpot,
                        new ZLinkSerialExecutionQueue(
                                host.serialExecutor(), ZLinkExecutionLanePolicy.spot()),
                        executionModes.getOrDefault(spotType, ZLinkUserSpotExecutionMode.SPOT_WIDE),
                        ZLinkInstanceSpot.class.isAssignableFrom(spotType),
                        null,
                        relocationCoordinationModes.getOrDefault(
                                spotType, ZLinkSpotRelocationCoordinationMode.FRAMEWORK_MANAGED));
        ZLinkSpot<?> spot;
        try {
            spot = createSpot(spotType, context);
        } catch (RuntimeException failure) {
            return SpotActivationBase.finishCleanup(failure, context.closeResourcesAsync())
                    .thenApply(ignored -> null);
        }
        if (spot == null) {
            return CompletableFuture.completedFuture(
                    new SpotActivationCreateResult(
                            new SpotActivation(host, handlerInvoker, null, backendSpot, context),
                            ZLinkSpotCreateResponse.accept()));
        }
        try {
            context.setSpot(spot);
            spot.configure();
            context.closeRegistration();
            context.bindSubscriptions(backendSpot);
        } catch (RuntimeException failure) {
            return SpotActivationBase.finishCleanup(failure, context.closeResourcesAsync())
                    .thenApply(ignored -> null);
        }
        return context.runLifecycleExecution(
                        () ->
                                host.runWithOutbound(
                                        context.dispatchOutbound(),
                                        () ->
                                                ZLinkHandlerStages.fromStageSupplier(
                                                        () -> spot.onCreate(effectiveRequest))))
                .thenCompose(
                        response -> initializeAcceptedSpot(spot, backendSpot, context, response))
                .handle(
                        (activation, error) -> {
                            if (error == null) {
                                return CompletableFuture.completedFuture(activation);
                            }
                            return SpotActivationBase.finishCleanup(
                                            error, context.closeResourcesAsync())
                                    .thenApply(ignored -> (SpotActivationCreateResult) null);
                        })
                .thenCompose(stage -> stage);
    }

    /** Activates a relocated User Spot on the target MeshNode {@code nodeRid}. */
    CompletionStage<SpotActivationCreateResult> activateRelocation(
            Class<? extends ZLinkSpot<?>> spotType,
            ZLinkBackendSpot backendSpot,
            RoutingId nodeRid) {
        DefaultSpotContext context =
                new DefaultSpotContext(
                        host,
                        workerPool,
                        handlerLoader,
                        nodeRid,
                        backendSpot,
                        new ZLinkSerialExecutionQueue(
                                host.serialExecutor(), ZLinkExecutionLanePolicy.spot()),
                        executionModes.getOrDefault(spotType, ZLinkUserSpotExecutionMode.SPOT_WIDE),
                        ZLinkInstanceSpot.class.isAssignableFrom(spotType),
                        null,
                        relocationCoordinationModes.getOrDefault(
                                spotType, ZLinkSpotRelocationCoordinationMode.FRAMEWORK_MANAGED));
        ZLinkSpot<?> spot;
        try {
            spot = createSpot(spotType, context);
            if (spot == null) {
                throw new ZLinkConfigurationException(
                        "relocation target Spot factory returned no instance: "
                                + spotType.getName());
            }
            context.setSpot(spot);
            spot.configure();
            context.closeRegistration();
            context.bindSubscriptions(backendSpot);
        } catch (RuntimeException failure) {
            return SpotActivationBase.finishCleanup(failure, context.closeResourcesAsync())
                    .thenApply(ignored -> null);
        }

        // Relocation has no creation request. The old activate() reuse ran
        // onCreate/onInitialize before the target ingress seal was acquired.
        return CompletableFuture.completedFuture(
                new SpotActivationCreateResult(
                        new SpotActivation(host, handlerInvoker, spot, backendSpot, context),
                        ZLinkSpotCreateResponse.accept()));
    }

    CompletionStage<Void> initializeRelocation(SpotActivation activation) {
        return activation
                .context
                .runLifecycleExecution(
                        () ->
                                host.runWithOutbound(
                                        activation.context.dispatchOutbound(),
                                        () ->
                                                ZLinkHandlerStages.fromStageSupplier(
                                                        activation.spot()::onInitialize)))
                .thenRun(() -> registerDispatchHandler(activation.backendSpot, activation));
    }

    CompletionStage<EntrySpotActivation> activateEntry(
            RoutingId nodeRid,
            ZLinkBackendSpot backendSpot,
            Class<? extends ZLinkEntrySpot<?>> entrySpotType) {
        DefaultEntrySpotContext context =
                new DefaultEntrySpotContext(host, workerPool, handlerLoader, nodeRid, backendSpot);
        ZLinkEntrySpot<?> entrySpot;
        try {
            entrySpot = createEntrySpot(entrySpotType, context);
        } catch (RuntimeException failure) {
            return SpotActivationBase.finishCleanup(failure, context.closeResourcesAsync())
                    .thenApply(ignored -> null);
        }
        if (entrySpot == null) {
            return SpotActivationBase.finishCleanup(
                            new ZLinkConfigurationException(
                                    "entry spot requires a public constructor accepting ZLinkEntrySpotContext "
                                            + "or a public no-arg constructor: "
                                            + entrySpotType.getName()),
                            context.closeResourcesAsync())
                    .thenApply(ignored -> null);
        }
        if (entrySpot.context() != context) {
            return SpotActivationBase.finishCleanup(
                            new ZLinkConfigurationException(
                                    "entry spot must expose the context provided by the runtime: "
                                            + entrySpotType.getName()),
                            context.closeResourcesAsync())
                    .thenApply(ignored -> null);
        }
        try {
            context.setEntrySpot(entrySpot);
            entrySpot.configure();
            context.closeRegistration();
            context.bindSubscriptions(backendSpot);
        } catch (RuntimeException failure) {
            return SpotActivationBase.finishCleanup(failure, context.closeResourcesAsync())
                    .thenApply(ignored -> null);
        }
        EntrySpotActivation activation =
                new EntrySpotActivation(host, handlerInvoker, entrySpot, backendSpot, context);
        context.enqueueDispatch(
                        () ->
                                host.runWithOutbound(
                                        context.dispatchOutbound(),
                                        () ->
                                                ZLinkHandlerStages.fromRunnable(
                                                        entrySpot::onInitialize)))
                .handle(
                        (ignored, failure) ->
                                failure == null
                                        ? CompletableFuture.<Void>completedFuture(null)
                                        : SpotActivationBase.finishCleanup(
                                                failure, context.closeResourcesAsync()))
                .thenCompose(stage -> stage);
        registerDispatchHandler(
                backendSpot, activation::handleDispatchEvent, activation::admitRoute);
        return CompletableFuture.completedFuture(activation);
    }

    /** Activates an Instance Spot on the MeshNode {@code nodeRid} of {@code meshName}. */
    CompletionStage<ZLinkInstanceSpotActivation> activateInstance(
            String meshName,
            RoutingId nodeRid,
            Class<? extends ZLinkInstanceSpot> spotType,
            ZLinkBackendSpot backendSpot) {
        return activateInstance(meshName, nodeRid, spotType, backendSpot, null);
    }

    CompletionStage<ZLinkInstanceSpotActivation> activateInstance(
            String meshName,
            RoutingId nodeRid,
            Class<? extends ZLinkInstanceSpot> spotType,
            ZLinkBackendSpot backendSpot,
            systems.zlink.framework.execution.ZLinkSerialExecutionQueue ownerQueue) {
        DefaultInstanceSpotContext context =
                new DefaultInstanceSpotContext(
                        host,
                        workerPool,
                        handlerLoader,
                        meshName,
                        nodeRid,
                        backendSpot,
                        ownerQueue);
        ZLinkInstanceSpot spot;
        try {
            spot =
                    (ZLinkInstanceSpot)
                            ZLinkHandlerActivator.services(handlerFactory)
                                    .add(ZLinkInstanceSpotContext.class, context)
                                    .create(spotType);
        } catch (RuntimeException error) {
            return SpotActivationBase.finishCleanup(
                            new ZLinkConfigurationException(
                                    "failed to create Instance Spot: " + spotType.getName(), error),
                            context.closeResourcesAsync())
                    .thenApply(ignored -> null);
        }
        if (spot == null || spot.context() != context) {
            return SpotActivationBase.finishCleanup(
                            new ZLinkConfigurationException(
                                    "Instance Spot must expose the context provided by the runtime:"
                                            + " "
                                            + spotType.getName()),
                            context.closeResourcesAsync())
                    .thenApply(ignored -> null);
        }
        try {
            context.bind(spot);
            try {
                spot.configure();
            } catch (RuntimeException failure) {
                throw ownerQueue == null ? failure : instanceInitializationFailure(failure);
            }
            context.closeRegistration(spotType);
        } catch (RuntimeException failure) {
            return SpotActivationBase.finishCleanup(failure, context.closeResourcesAsync())
                    .thenApply(ignored -> null);
        }
        return (ownerQueue == null
                        ? context.runLifecycle(spot::onInitialize)
                        : systems.zlink.framework.execution.ZLinkSerialExecutionQueue.yieldCurrent(
                                context.runLifecycleExecution(spot::onInitialize)
                                        .exceptionallyCompose(
                                                failure ->
                                                        CompletableFuture.failedFuture(
                                                                instanceInitializationFailure(
                                                                        failure)))))
                .thenApply(
                        ignored -> {
                            var activation =
                                    new ZLinkInstanceSpotActivation(
                                            host, handlerInvoker, spot, backendSpot, context);
                            registerDispatchHandler(
                                    backendSpot,
                                    activation::handleDispatchEvent,
                                    activation::admitRoute);
                            return activation;
                        })
                .handle(
                        (activation, failure) ->
                                failure == null
                                        ? CompletableFuture.completedFuture(activation)
                                        : SpotActivationBase.finishCleanup(
                                                        failure, context.closeResourcesAsync())
                                                .thenApply(
                                                        ignored ->
                                                                (ZLinkInstanceSpotActivation) null))
                .thenCompose(stage -> stage);
    }

    private static RuntimeException instanceInitializationFailure(Throwable failure) {
        Throwable cause = failure;
        while (cause instanceof java.util.concurrent.CompletionException
                && cause.getCause() != null) cause = cause.getCause();
        return cause instanceof ZLinkFrameworkException typed
                ? typed
                : new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                        "Instance Spot reincarnation initialization failed",
                        cause);
    }

    private CompletionStage<SpotActivationCreateResult> initializeAcceptedSpot(
            ZLinkSpot<?> spot,
            ZLinkBackendSpot backendSpot,
            DefaultSpotContext context,
            ZLinkSpotCreateResponse response) {
        ZLinkSpotCreateResponse effectiveResponse =
                response == null ? ZLinkSpotCreateResponse.accept() : response;
        if (!effectiveResponse.accepted()) {
            return context.closeResourcesAsync()
                    .thenApply(ignored -> new SpotActivationCreateResult(null, effectiveResponse));
        }
        return context.runLifecycleExecution(
                        () ->
                                host.runWithOutbound(
                                        context.dispatchOutbound(),
                                        () ->
                                                ZLinkHandlerStages.fromStageSupplier(
                                                        spot::onInitialize)))
                .thenApply(
                        ignored -> {
                            SpotActivation activation =
                                    new SpotActivation(
                                            host, handlerInvoker, spot, backendSpot, context);
                            registerDispatchHandler(backendSpot, activation);
                            return new SpotActivationCreateResult(activation, effectiveResponse);
                        });
    }

    private static void registerDispatchHandler(
            ZLinkBackendSpot backendSpot,
            Function<ZLinkBackendSpotDispatchInfo, CompletionStage<Void>> handler,
            BiFunction<ZLinkBackendReceived, CompletableFuture<Void>, CompletionStage<Void>>
                    routeHandler) {
        backendSpot.onDispatchEvent(
                new ZLinkInternalAsyncSpotDispatchHandler() {
                    @Override
                    public CompletionStage<Void> handleAsync(ZLinkBackendSpotDispatchInfo info) {
                        return handler.apply(info);
                    }

                    @Override
                    public CompletionStage<Void> handleRoute(ZLinkBackendReceived received) {
                        return routeHandler.apply(received, null);
                    }

                    @Override
                    public CompletionStage<Void> handleRoute(
                            ZLinkBackendReceived received, CompletableFuture<Void> admission) {
                        return routeHandler.apply(received, admission);
                    }
                });
    }

    private static void registerDispatchHandler(
            ZLinkBackendSpot backendSpot, SpotActivation activation) {
        backendSpot.onDispatchEvent(
                new ZLinkInternalAsyncSpotDispatchHandler() {
                    @Override
                    public CompletionStage<Void> handleAsync(ZLinkBackendSpotDispatchInfo info) {
                        return activation.handleDispatchEvent(info);
                    }

                    @Override
                    public CompletionStage<Void> handleRoute(ZLinkBackendReceived received) {
                        return activation.admitRoute(received);
                    }

                    @Override
                    public CompletionStage<Void> handleRoute(
                            ZLinkBackendReceived received, CompletableFuture<Void> admission) {
                        return activation.admitRoute(received, admission);
                    }

                    @Override
                    public Boolean handleTopic(
                            systems.zlink.framework.runtime.internal.backend
                                            .ZLinkBackendTopicMessage
                                    message) {
                        return activation.admitTopic(message);
                    }

                    @Override
                    public CompletionStage<Void> handleActor(
                            java.util.List<
                                            systems.zlink.framework.runtime.internal.backend
                                                    .ZLinkBackendActorReceived>
                                    messages) {
                        return activation.admitActor(messages);
                    }
                });
    }

    private ZLinkSpot<?> createSpot(
            Class<? extends ZLinkSpot<?>> spotType, ZLinkSpotContext context) {
        try {
            return (ZLinkSpot<?>)
                    ZLinkHandlerActivator.services(handlerFactory)
                            .add(ZLinkSpotContext.class, context)
                            .create(spotType);
        } catch (RuntimeException error) {
            throw new ZLinkConfigurationException(
                    "failed to create spot: " + spotType.getName(), error);
        }
    }

    private ZLinkEntrySpot<?> createEntrySpot(
            Class<? extends ZLinkEntrySpot<?>> entrySpotType, ZLinkEntrySpotContext context) {
        try {
            return (ZLinkEntrySpot<?>)
                    ZLinkHandlerActivator.services(handlerFactory)
                            .add(ZLinkEntrySpotContext.class, context)
                            .create(entrySpotType);
        } catch (RuntimeException error) {
            throw new ZLinkConfigurationException(
                    "failed to create entry spot: " + entrySpotType.getName(), error);
        }
    }
}
