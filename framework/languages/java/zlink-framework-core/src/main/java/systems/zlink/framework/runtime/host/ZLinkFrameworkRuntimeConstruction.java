package systems.zlink.framework.runtime.host;

import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.locations.ZLinkLocationOptions;
import systems.zlink.framework.runtime.actors.ZLinkActorRuntime;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.mesh.MeshNodeRegistration;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;
import systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireRuntime;

import java.time.Duration;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.function.Function;
import java.util.function.Predicate;
import java.util.function.Supplier;
import java.util.function.UnaryOperator;

/** Transient collaborator injection for package-owned runtime construction and its tests. */
record ZLinkFrameworkRuntimeConstruction(
        Supplier<ZLinkMeshDrainCoordinator> meshDrains,
        ExecutorService channelInfrastructureExecutor,
        SpotTransportAddressResolver channelSpotAddressResolver,
        ZLinkLocationRuntime locationRuntime,
        ZLinkRegisteredLocationStores locationStores,
        ScheduledExecutorService spotTimerExecutor,
        ThreadPoolExecutor spotMulticastExecutor,
        Executor spotHandlerExecutor,
        Supplier<ZLinkStateLane> spotTimerStateLaneFactory,
        UnaryOperator<ZLinkLocationRepository> spotOperationAuthorityStore,
        ZLinkLocationRepository spotLocationStore,
        ZLinkLocationRuntime spotLocationRuntime,
        ZLinkStoreLocationResolvers spotLocationResolvers,
        List<?> relocationStateMachines,
        UnaryOperator<ZLinkLocationRepository> aggregateRelocationAuthorityStoreDecorator,
        UnaryOperator<Function<Object, CompletionStage<Void>>> targetNormalizerDecorator,
        ChannelRuntimeFactory channelRuntimeFactory,
        SpotRuntimeFactory spotRuntimeFactory,
        SpotRetireRuntimeFactory spotRetireRuntimeFactory,
        java.util.function.Consumer<Runnable> callServicesPublication) {

    static ZLinkFrameworkRuntimeConstruction defaults() {
        return new ZLinkFrameworkRuntimeConstruction(
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                null,
                (backend,
                        context,
                        backendFactory,
                        adapterOptions,
                        registration,
                        serializer,
                        handlerFactory,
                        eventDispatcher,
                        infrastructureExecutor) ->
                        new ZLinkChannelRuntime(
                                backend,
                                context,
                                backendFactory,
                                adapterOptions,
                                registration,
                                serializer,
                                handlerFactory,
                                eventDispatcher),
                (backendFactory,
                        adapterOptions,
                        registration,
                        channels,
                        context,
                        serializer,
                        handlerFactory,
                        eventDispatcher,
                        meshNodes,
                        timerExecutor,
                        multicastExecutor,
                        spotHandlerExecutor,
                        timerStateLaneFactory) ->
                        new ZLinkSpotRuntime(
                                backendFactory,
                                adapterOptions,
                                registration,
                                channels,
                                context,
                                serializer,
                                handlerFactory,
                                eventDispatcher,
                                meshNodes),
                (spots,
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
                        stateMachines,
                        aggregateRelocationAuthorityStoreDecorator,
                        targetNormalizerDecorator) ->
                        new ZLinkUserSpotRetireRuntime(
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
                                targetAdmissionSealed),
                Runnable::run);
    }

    @FunctionalInterface
    interface ChannelRuntimeFactory {
        ZLinkChannelRuntime create(
                ZLinkChannelBackendAdapter backend,
                ZLinkBackendContext context,
                ZLinkBackendAdapterProvider backendFactory,
                ZLinkBackendAdapterOptions adapterOptions,
                ZLinkFrameworkRegistration registration,
                ZLinkMessageSerializer serializer,
                ZLinkHandlerActivator handlerFactory,
                ZLinkRuntimeEventDispatcher eventDispatcher,
                ExecutorService infrastructureExecutor);
    }

    @FunctionalInterface
    interface SpotRuntimeFactory {
        ZLinkSpotRuntime create(
                ZLinkBackendAdapterProvider backendFactory,
                ZLinkBackendAdapterOptions adapterOptions,
                ZLinkFrameworkRegistration registration,
                ZLinkChannelRuntime channels,
                ZLinkBackendContext context,
                ZLinkMessageSerializer serializer,
                ZLinkHandlerActivator handlerFactory,
                ZLinkRuntimeEventDispatcher eventDispatcher,
                Map<String, ZLinkInternalMeshNode> meshNodes,
                ScheduledExecutorService timerExecutor,
                ThreadPoolExecutor multicastExecutor,
                Executor handlerExecutor,
                Supplier<ZLinkStateLane> timerStateLaneFactory);
    }

    @FunctionalInterface
    interface SpotRetireRuntimeFactory {
        ZLinkUserSpotRetireRuntime create(
                ZLinkSpotRuntime spots,
                ZLinkActorRuntime actors,
                List<MeshNodeRegistration> registrations,
                Map<String, ZLinkInternalMeshNode> nodes,
                ZLinkLocationRepository locations,
                ZLinkStoreLocationResolvers locationResolvers,
                ZLinkLocationRepository authorities,
                ZLinkLocationOptions options,
                systems.zlink.framework.runtime.internal.relocation.ZLinkRelocationAdapterRegistry
                        adapters,
                long applicationVersion,
                Optional<String> maintenanceWave,
                Duration sessionRelocationSealTimeout,
                Predicate<String> targetAdmissionSealed,
                List<?> stateMachines,
                UnaryOperator<ZLinkLocationRepository> aggregateRelocationAuthorityStoreDecorator,
                UnaryOperator<Function<Object, CompletionStage<Void>>> targetNormalizerDecorator);
    }
}
