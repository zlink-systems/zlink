package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActor;
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
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendTopicMessage;
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
        return context.prepareActivation(
                        () -> {
                            ZLinkSpot<?> spot = createSpot(spotType, context);
                            if (spot == null) {
                                return CompletableFuture.completedFuture(
                                        new SpotActivationCreateResult(
                                                new SpotActivation(
                                                        host,
                                                        handlerInvoker,
                                                        null,
                                                        backendSpot,
                                                        context),
                                                ZLinkSpotCreateResponse.accept()));
                            }
                            context.setSpot(spot);
                            spot.configure();
                            context.closeRegistration();
                            context.bindSubscriptions(backendSpot);
                            return host.runWithOutbound(
                                            context.dispatchOutbound(),
                                            () ->
                                                    ZLinkHandlerStages.fromStageSupplier(
                                                            () -> spot.onCreate(effectiveRequest)))
                                    .thenCompose(
                                            response ->
                                                    initializeAcceptedSpot(
                                                            spot, backendSpot, context, response));
                        })
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
            RoutingId nodeRid,
            byte[] timerEnvelope) {
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
        return context.prepareRelocationTarget(
                        () -> {
                            ZLinkSpot<?> spot = createSpot(spotType, context);
                            if (spot == null) {
                                throw new ZLinkConfigurationException(
                                        "relocation target Spot factory returned no instance: "
                                                + spotType.getName());
                            }
                            context.setSpot(spot);
                            context.stageTimerRelocationEnvelope(timerEnvelope);
                            return context.runTimerRelocationInitialization(
                                    () -> {
                                        spot.configure();
                                        context.closeRegistration();
                                        context.bindSubscriptions(backendSpot);
                                        return CompletableFuture.completedFuture(
                                                new SpotActivationCreateResult(
                                                        new SpotActivation(
                                                                host,
                                                                handlerInvoker,
                                                                spot,
                                                                backendSpot,
                                                                context),
                                                        ZLinkSpotCreateResponse.accept()));
                                    });
                        })
                .exceptionallyCompose(
                        failure ->
                                SpotActivationBase.finishCleanup(
                                                failure,
                                                context.discardActivationPreparation()
                                                        .thenCompose(
                                                                ignored ->
                                                                        context
                                                                                .releaseRelocationPreparation())
                                                        .thenCompose(
                                                                ignored ->
                                                                        context
                                                                                .closeResourcesAsync()))
                                        .thenApply(ignored -> null));
    }

    CompletionStage<Void> initializeRelocation(SpotActivation activation) {
        return activation
                .context
                .runTimerRelocationInitialization(
                        () ->
                                activation.context.runLifecycleExecution(
                                        () ->
                                                host.runWithOutbound(
                                                        activation.context.dispatchOutbound(),
                                                        () ->
                                                                ZLinkHandlerStages
                                                                        .fromStageSupplier(
                                                                                activation.spot()
                                                                                        ::onInitialize)
                                                                        .handle(
                                                                                (ignored,
                                                                                        failure) -> {
                                                                                    if (failure
                                                                                            != null)
                                                                                        host
                                                                                                .reportSpotLifecycleFailure(
                                                                                                        activation
                                                                                                                .context
                                                                                                                .spotId(),
                                                                                                        failure);
                                                                                    return (Void)
                                                                                            null;
                                                                                }))))
                .thenRun(() -> registerDispatchHandler(activation.backendSpot, activation))
                .thenCompose(
                        ignored ->
                                activation.context.runRelocationReadyCompletion(
                                        systems.zlink.framework.spots
                                                .ZLinkSpotRelocationReadyOutcome.RELOCATED,
                                        failure ->
                                                host.reportSpotLifecycleFailure(
                                                        activation.context.spotId(), failure)));
    }

    CompletionStage<EntrySpotActivation> activateEntry(
            RoutingId nodeRid,
            ZLinkBackendSpot backendSpot,
            Class<? extends ZLinkEntrySpot<?>> entrySpotType) {
        DefaultEntrySpotContext context =
                new DefaultEntrySpotContext(host, workerPool, handlerLoader, nodeRid, backendSpot);
        ZLinkEntrySpot<?> entrySpot;
        try {
            // The node's Entry Spot owns Actor infrastructure even when the application
            // registers no Entry Spot type (MeshNode spec §3.2).
            entrySpot =
                    entrySpotType == null
                            ? new ZLinkEntrySpot<ZLinkActor>() {
                                @Override
                                public ZLinkEntrySpotContext context() {
                                    return context;
                                }

                                @Override
                                public CompletionStage<Void> onJoinedActor(ZLinkActor actor) {
                                    return CompletableFuture.completedFuture(null);
                                }

                                @Override
                                public CompletionStage<Void> onLeaveActor(ZLinkActor actor) {
                                    return CompletableFuture.completedFuture(null);
                                }
                            }
                            : createEntrySpot(entrySpotType, context);
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
        return context.enqueueDispatch(
                        () ->
                                host.runWithOutbound(
                                        context.dispatchOutbound(),
                                        () ->
                                                ZLinkHandlerStages.fromStageSupplier(
                                                        entrySpot::onInitialize)))
                .handle(
                        (ignored, failure) ->
                                failure == null
                                        ? CompletableFuture.<Void>completedFuture(null)
                                        : SpotActivationBase.finishCleanup(
                                                failure, context.closeResourcesAsync()))
                .thenCompose(stage -> stage)
                .thenApply(
                        ignored -> {
                            registerDispatchHandler(
                                    backendSpot,
                                    activation::handleDispatchEvent,
                                    activation::admitRoute,
                                    activation::admitTopic,
                                    activation::admitActor);
                            return activation;
                        });
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
        java.util.function.Supplier<CompletionStage<ZLinkInstanceSpotActivation>> initialize =
                () -> {
                    ZLinkInstanceSpot spot;
                    try {
                        spot =
                                (ZLinkInstanceSpot)
                                        ZLinkHandlerActivator.services(handlerFactory)
                                                .add(ZLinkInstanceSpotContext.class, context)
                                                .create(spotType);
                    } catch (RuntimeException error) {
                        throw new ZLinkConfigurationException(
                                "failed to create Instance Spot: " + spotType.getName(), error);
                    }
                    if (spot == null || spot.context() != context) {
                        throw new ZLinkConfigurationException(
                                "Instance Spot must expose the context provided by the runtime: "
                                        + spotType.getName());
                    }
                    var preparedActivation =
                            new ZLinkInstanceSpotActivation(
                                    host, handlerInvoker, spot, backendSpot, context);
                    context.bind(spot, preparedActivation);
                    spot.configure();
                    context.closeRegistration(spotType);
                    return spot.onInitialize()
                            .thenApply(
                                    ignored -> {
                                        // Instance Spots do not register Logical Multicast
                                        // handlers.
                                        registerDispatchHandler(
                                                backendSpot,
                                                preparedActivation::handleDispatchEvent,
                                                preparedActivation::admitRoute,
                                                message -> null,
                                                preparedActivation::admitActor);
                                        return preparedActivation;
                                    });
                };
        return (ownerQueue == null
                        ? context.prepareActivation(initialize)
                        : ZLinkSerialExecutionQueue.manageCurrent(
                                context.runLifecycleExecution(initialize)
                                        .exceptionallyCompose(
                                                failure ->
                                                        CompletableFuture.failedFuture(
                                                                instanceInitializationFailure(
                                                                        failure)))))
                .handle(
                        (activation, failure) ->
                                failure == null
                                        ? CompletableFuture.completedFuture(activation)
                                        : SpotActivationBase.finishCleanup(
                                                        failure,
                                                        context.closeResourcesAsync()
                                                                .handle(
                                                                        (ignored, cleanupFailure) ->
                                                                                SpotActivationBase
                                                                                        .finishCleanup(
                                                                                                cleanupFailure,
                                                                                                context
                                                                                                        .finishActivationPreparation()))
                                                                .thenCompose(stage -> stage))
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
                    routeHandler,
            Function<ZLinkBackendTopicMessage, Boolean> topicHandler,
            BiFunction<
                            java.util.List<
                                    systems.zlink.framework.runtime.internal.backend
                                            .ZLinkBackendActorReceived>,
                            CompletableFuture<Void>,
                            CompletionStage<Void>>
                    actorHandler) {
        backendSpot.onDispatchEvent(
                new ZLinkInternalAsyncSpotDispatchHandler() {
                    @Override
                    public CompletionStage<Void> handleAsync(ZLinkBackendSpotDispatchInfo info) {
                        return handler.apply(info);
                    }

                    @Override
                    public CompletionStage<Void> handleRoute(
                            ZLinkBackendReceived received, CompletableFuture<Void> admission) {
                        return routeHandler.apply(received, admission);
                    }

                    @Override
                    public Boolean handleTopic(ZLinkBackendTopicMessage message) {
                        return topicHandler.apply(message);
                    }

                    @Override
                    public CompletionStage<Void> handleActor(
                            java.util.List<
                                            systems.zlink.framework.runtime.internal.backend
                                                    .ZLinkBackendActorReceived>
                                    messages,
                            CompletableFuture<Void> admission) {
                        return actorHandler.apply(messages, admission);
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
                                    messages,
                            CompletableFuture<Void> requestAdmission) {
                        return activation.admitActor(messages, requestAdmission);
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
