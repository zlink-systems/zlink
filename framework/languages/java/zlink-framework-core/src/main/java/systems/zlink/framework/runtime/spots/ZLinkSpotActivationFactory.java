package systems.zlink.framework.runtime.spots;

import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.CompletionStage;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.errors.ZLinkConfigurationException;
import systems.zlink.framework.execution.ZLinkWorkerPool;
import systems.zlink.framework.messaging.ZLinkMessage;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalAsyncSpotDispatchHandler;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.handlers.ZLinkHandlerStages;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import systems.zlink.framework.spots.ZLinkInstanceSpot;
import systems.zlink.framework.spots.ZLinkEntrySpotContext;
import systems.zlink.framework.spots.ZLinkSpot;
import systems.zlink.framework.spots.ZLinkSpotContext;
import systems.zlink.framework.spots.ZLinkSpotCreateResponse;
import systems.zlink.framework.configuration.ZLinkUserSpotExecutionMode;
import systems.zlink.framework.configuration.ZLinkSpotRelocationReadinessMode;

final class ZLinkSpotActivationFactory {
    private final ZLinkSpotRuntime host;
    private final ZLinkWorkerPool workerPool;
    private final ZLinkSpotHandlerLoader handlerLoader;
    private final ZLinkSpotHandlerInvoker handlerInvoker;
    private final ZLinkHandlerActivator handlerFactory;
    private final java.util.Map<
        Class<? extends ZLinkSpot<?>>, ZLinkUserSpotExecutionMode> executionModes;
    private final java.util.Map<
        Class<? extends ZLinkSpot<?>>, ZLinkSpotRelocationReadinessMode>
            relocationReadinessModes;

    ZLinkSpotActivationFactory(
        ZLinkSpotRuntime host,
        ZLinkWorkerPool workerPool,
        ZLinkSpotHandlerLoader handlerLoader,
        ZLinkSpotHandlerInvoker handlerInvoker,
        ZLinkHandlerActivator handlerFactory,
        java.util.Map<
            Class<? extends ZLinkSpot<?>>, ZLinkUserSpotExecutionMode> executionModes,
        java.util.Map<
            Class<? extends ZLinkSpot<?>>, ZLinkSpotRelocationReadinessMode>
                relocationReadinessModes) {
        this.host = host;
        this.workerPool = workerPool;
        this.handlerLoader = handlerLoader;
        this.handlerInvoker = handlerInvoker;
        this.handlerFactory = handlerFactory;
        this.executionModes = java.util.Map.copyOf(executionModes);
        this.relocationReadinessModes =
            java.util.Map.copyOf(relocationReadinessModes);
    }

    CompletionStage<SpotActivationCreateResult> activate(
        Class<? extends ZLinkSpot<?>> spotType,
        ZLinkBackendSpot backendSpot,
        ZLinkMessage request) {
        ZLinkMessage effectiveRequest = request == null ? ZLinkMessage.empty() : request;
        DefaultSpotContext context = new DefaultSpotContext(
            host,
            workerPool,
            handlerLoader,
            host.primaryNode().routingId(),
            backendSpot,
            new systems.zlink.framework.execution.ZLinkAsyncSerialQueue(
                host.serialExecutor(), false),
            executionModes.getOrDefault(
                spotType,
                ZLinkUserSpotExecutionMode.SPOT_WIDE),
            ZLinkInstanceSpot.class.isAssignableFrom(spotType),
            null,
            relocationReadinessModes.getOrDefault(
                spotType,
                ZLinkSpotRelocationReadinessMode.ANY_TURN_BOUNDARY));
        ZLinkSpot<?> spot;
        try {
            spot = createSpot(spotType, context);
        } catch (RuntimeException failure) {
            context.closeHandlerInstances();
            backendSpot.close();
            throw failure;
        }
        if (spot == null) {
            return CompletableFuture.completedFuture(new SpotActivationCreateResult(
                new SpotActivation(host, handlerInvoker, null, backendSpot, context),
                ZLinkSpotCreateResponse.accept()));
        }
        try {
            context.setSpot(spot);
            spot.configure();
            context.closeRegistration();
            context.bindSubscriptions(backendSpot);
        } catch (RuntimeException failure) {
            context.closeTimers();
            context.closeHandlerInstances();
            backendSpot.close();
            throw failure;
        }
        return context.runLifecycleExecution(() ->
                host.runWithOutbound(context.dispatchOutbound(), () ->
                    ZLinkHandlerStages.fromStageSupplier(
                        () -> spot.onCreate(effectiveRequest))))
            .thenCompose(response -> initializeAcceptedSpot(
                spot,
                backendSpot,
                context,
                response))
            .handle((activation, error) -> {
                if (error == null) {
                    return activation;
                }
                context.closeTimers();
                context.closeHandlerInstances();
                backendSpot.close();
                throw new CompletionException(error);
            });
    }

    EntrySpotActivation activateEntry(
        RoutingId nodeRid,
        ZLinkBackendSpot backendSpot,
        Class<? extends ZLinkEntrySpot<?>> entrySpotType) {
        DefaultEntrySpotContext context = new DefaultEntrySpotContext(
            host,
            workerPool,
            handlerLoader,
            nodeRid,
            backendSpot);
        ZLinkEntrySpot<?> entrySpot;
        try {
            entrySpot = createEntrySpot(entrySpotType, context);
        } catch (RuntimeException failure) {
            context.closeHandlerInstances();
            backendSpot.close();
            throw failure;
        }
        if (entrySpot == null) {
            backendSpot.close();
            throw new ZLinkConfigurationException(
                "entry spot requires a public constructor accepting ZLinkEntrySpotContext "
                    + "or a public no-arg constructor: " + entrySpotType.getName());
        }
        if (entrySpot.context() != context) {
            backendSpot.close();
            throw new ZLinkConfigurationException(
                "entry spot must expose the context provided by the runtime: "
                    + entrySpotType.getName());
        }
        try {
            context.setEntrySpot(entrySpot);
            entrySpot.configure();
            context.closeRegistration();
            context.bindSubscriptions(backendSpot);
        } catch (RuntimeException failure) {
            context.closeTimers();
            context.closeHandlerInstances();
            backendSpot.close();
            throw failure;
        }
        context.enqueueDispatch(() -> host.runWithOutbound(
                context.dispatchOutbound(),
                () -> ZLinkHandlerStages.fromRunnable(entrySpot::onInitialize)))
            .whenComplete((ignored, error) -> {
                if (error != null) {
                    context.closeTimers();
                    context.closeHandlerInstances();
                    backendSpot.close();
                }
            });
        EntrySpotActivation activation = new EntrySpotActivation(
            host,
            handlerInvoker,
            entrySpot,
            backendSpot,
            context);
        registerDispatchHandler(backendSpot, activation::handleDispatchEvent);
        return activation;
    }

    CompletionStage<ZLinkInstanceSpotActivation> activateInstance(
        String meshName,
        Class<? extends ZLinkInstanceSpot> spotType,
        ZLinkBackendSpot backendSpot) {
        DefaultInstanceSpotContext context = new DefaultInstanceSpotContext(
            host,
            workerPool,
            handlerLoader,
            meshName,
            host.primaryNode().routingId(),
            backendSpot);
        ZLinkInstanceSpot spot;
        try {
            spot = (ZLinkInstanceSpot) ZLinkHandlerActivator
                .services(handlerFactory)
                .add(systems.zlink.framework.spots.ZLinkInstanceSpotContext.class, context)
                .create(spotType);
        } catch (RuntimeException error) {
            context.closeResources();
            throw new ZLinkConfigurationException(
                "failed to create Instance Spot: " + spotType.getName(),
                error);
        }
        if (spot == null || spot.context() != context) {
            context.closeResources();
            throw new ZLinkConfigurationException(
                "Instance Spot must expose the context provided by the runtime: "
                    + spotType.getName());
        }
        try {
            context.bind(spot);
            spot.configure();
            context.closeRegistration(spotType);
        } catch (RuntimeException failure) {
            context.closeResources();
            throw failure;
        }
        return context.runLifecycle(spot::onInitialize)
            .thenApply(ignored -> {
                var activation = new ZLinkInstanceSpotActivation(
                    host, handlerInvoker, spot, backendSpot, context);
                registerDispatchHandler(
                    backendSpot, activation::handleDispatchEvent);
                return activation;
            })
            .whenComplete((ignored, failure) -> {
                if (failure != null) {
                    context.closeResources();
                }
            });
    }

    private CompletionStage<SpotActivationCreateResult> initializeAcceptedSpot(
        ZLinkSpot<?> spot,
        ZLinkBackendSpot backendSpot,
        DefaultSpotContext context,
        ZLinkSpotCreateResponse response) {
        ZLinkSpotCreateResponse effectiveResponse =
            response == null ? ZLinkSpotCreateResponse.accept() : response;
        if (!effectiveResponse.accepted()) {
            context.closeTimers();
            context.closeHandlerInstances();
            backendSpot.close();
            return CompletableFuture.completedFuture(
                new SpotActivationCreateResult(null, effectiveResponse));
        }
        return context.runLifecycleExecution(() -> host.runWithOutbound(
                context.dispatchOutbound(),
                () -> ZLinkHandlerStages.fromStageSupplier(spot::onInitialize)))
            .thenApply(ignored -> {
                SpotActivation activation = new SpotActivation(
                    host,
                    handlerInvoker,
                    spot,
                    backendSpot,
                    context);
                registerDispatchHandler(backendSpot, activation::handleDispatchEvent);
                return new SpotActivationCreateResult(activation, effectiveResponse);
            });
    }

    private static void registerDispatchHandler(
        ZLinkBackendSpot backendSpot,
        java.util.function.Function<
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo,
            CompletionStage<Void>> handler) {
        backendSpot.onDispatchEvent(new ZLinkInternalAsyncSpotDispatchHandler() {
            @Override
            public CompletionStage<Void> handleAsync(
                systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpotDispatchInfo info) {
                return handler.apply(info);
            }
        });
    }

    private ZLinkSpot<?> createSpot(
        Class<? extends ZLinkSpot<?>> spotType,
        ZLinkSpotContext context) {
        try {
            return (ZLinkSpot<?>) ZLinkHandlerActivator.services(handlerFactory)
                .add(ZLinkSpotContext.class, context)
                .create(spotType);
        } catch (RuntimeException error) {
            throw new ZLinkConfigurationException(
                "failed to create spot: " + spotType.getName(),
                error);
        }
    }

    private ZLinkEntrySpot<?> createEntrySpot(
        Class<? extends ZLinkEntrySpot<?>> entrySpotType,
        ZLinkEntrySpotContext context) {
        try {
            return (ZLinkEntrySpot<?>) ZLinkHandlerActivator.services(handlerFactory)
                .add(ZLinkEntrySpotContext.class, context)
                .create(entrySpotType);
        } catch (RuntimeException error) {
            throw new ZLinkConfigurationException(
                "failed to create entry spot: " + entrySpotType.getName(),
                error);
        }
    }
}
