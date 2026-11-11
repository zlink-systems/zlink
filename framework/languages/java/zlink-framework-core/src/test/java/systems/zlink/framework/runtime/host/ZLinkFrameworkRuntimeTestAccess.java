package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.binding.ZLinkJavaBackendAdapterFactory;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;

import java.util.List;
import java.util.concurrent.CompletionStage;
import java.util.concurrent.Executor;
import java.util.concurrent.ExecutorService;
import java.util.concurrent.ScheduledExecutorService;
import java.util.concurrent.ThreadPoolExecutor;
import java.util.function.Function;
import java.util.function.Supplier;
import java.util.function.UnaryOperator;

/** Test-only access to the package-owned runtime bootstrap. */
public final class ZLinkFrameworkRuntimeTestAccess {
    private ZLinkFrameworkRuntimeTestAccess() {}

    public static final class Construction {
        private java.util.function.Consumer<Runnable> callServicesPublication = Runnable::run;

        public Construction callServicesPublication(java.util.function.Consumer<Runnable> value) {
            callServicesPublication = value;
            return this;
        }

        private Supplier<ZLinkMeshDrainCoordinator> meshDrains;
        private ExecutorService channelInfrastructureExecutor;
        private SpotTransportAddressResolver channelSpotAddressResolver;
        private ZLinkLocationRuntime locationRuntime;
        private ZLinkRegisteredLocationStores locationStores;
        private ScheduledExecutorService spotTimerExecutor;
        private ThreadPoolExecutor spotMulticastExecutor;
        private Executor spotHandlerExecutor;
        private Supplier<ZLinkStateLane> spotTimerStateLaneFactory;
        private java.util.function.UnaryOperator<ZLinkLocationRepository>
                spotOperationAuthorityStore;
        private ZLinkLocationRepository spotLocationStore;
        private ZLinkLocationRuntime spotLocationRuntime;
        private ZLinkStoreLocationResolvers spotLocationResolvers;
        private List<?> relocationStateMachines;
        private java.util.function.UnaryOperator<ZLinkLocationRepository>
                aggregateRelocationAuthorityStoreDecorator;
        private UnaryOperator<Function<Object, CompletionStage<Void>>> targetNormalizerDecorator;

        public Construction meshDrains(Supplier<ZLinkMeshDrainCoordinator> value) {
            meshDrains = value;
            return this;
        }

        public Construction channelInfrastructureExecutor(ExecutorService value) {
            channelInfrastructureExecutor = value;
            return this;
        }

        public Construction channelSpotAddressResolver(SpotTransportAddressResolver value) {
            channelSpotAddressResolver = value;
            return this;
        }

        public Construction locationRuntime(ZLinkLocationRuntime value) {
            locationRuntime = value;
            return this;
        }

        public Construction locationStores(ZLinkRegisteredLocationStores value) {
            locationStores = value;
            return this;
        }

        public Construction spotTimerExecutor(ScheduledExecutorService value) {
            spotTimerExecutor = value;
            return this;
        }

        public Construction spotMulticastExecutor(ThreadPoolExecutor value) {
            spotMulticastExecutor = value;
            return this;
        }

        public Construction spotHandlerExecutor(Executor value) {
            spotHandlerExecutor = value;
            return this;
        }

        public Construction spotTimerStateLaneFactory(Supplier<ZLinkStateLane> value) {
            spotTimerStateLaneFactory = value;
            return this;
        }

        public Construction spotOperationAuthorityStore(
                java.util.function.UnaryOperator<ZLinkLocationRepository> value) {
            spotOperationAuthorityStore = value;
            return this;
        }

        public Construction spotOperationAuthorityStore(ZLinkLocationRepository value) {
            spotOperationAuthorityStore = ignored -> value;
            return this;
        }

        public Construction spotLocationStore(ZLinkLocationRepository value) {
            spotLocationStore = value;
            return this;
        }

        public Construction spotLocationRuntime(ZLinkLocationRuntime value) {
            spotLocationRuntime = value;
            return this;
        }

        public Construction spotLocationResolvers(ZLinkStoreLocationResolvers value) {
            spotLocationResolvers = value;
            return this;
        }

        public Construction relocationStateMachines(List<?> value) {
            relocationStateMachines = value;
            return this;
        }

        public Construction aggregateRelocationAuthorityStoreDecorator(
                java.util.function.UnaryOperator<ZLinkLocationRepository> value) {
            aggregateRelocationAuthorityStoreDecorator = value;
            return this;
        }

        public Construction targetNormalizerDecorator(
                UnaryOperator<Function<Object, CompletionStage<Void>>> value) {
            targetNormalizerDecorator = value;
            return this;
        }

        private ZLinkFrameworkRuntimeConstruction toRuntimeConstruction() {
            return new ZLinkFrameworkRuntimeConstruction(
                    meshDrains,
                    channelInfrastructureExecutor,
                    channelSpotAddressResolver,
                    locationRuntime,
                    locationStores,
                    spotTimerExecutor,
                    spotMulticastExecutor,
                    spotHandlerExecutor,
                    spotTimerStateLaneFactory,
                    spotOperationAuthorityStore,
                    spotLocationStore,
                    spotLocationRuntime,
                    spotLocationResolvers,
                    relocationStateMachines,
                    aggregateRelocationAuthorityStoreDecorator,
                    targetNormalizerDecorator,
                    (backend,
                            context,
                            backendFactory,
                            adapterOptions,
                            registration,
                            serializer,
                            handlerFactory,
                            eventDispatcher,
                            infrastructureExecutor) ->
                            systems.zlink.framework.runtime.channels.ZLinkChannelRuntimeTestAccess
                                    .withInfrastructureExecutor(
                                            backend,
                                            context,
                                            backendFactory,
                                            adapterOptions,
                                            registration,
                                            serializer,
                                            handlerFactory,
                                            eventDispatcher,
                                            infrastructureExecutor),
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
                            handlerExecutor,
                            timerStateLaneFactory) ->
                            systems.zlink.framework.runtime.spots.ZLinkSpotRuntimeTestAccess
                                    .withTimerCollaborators(
                                            backendFactory,
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
                                            handlerExecutor,
                                            timerStateLaneFactory),
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
                            systems.zlink.framework.runtime.spots.ZLinkSpotRuntimeTestAccess
                                    .userSpotRetireRuntimeWithStateMachines(
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
                                            stateMachines,
                                            aggregateRelocationAuthorityStoreDecorator,
                                            targetNormalizerDecorator),
                    callServicesPublication);
        }
    }

    public static ZLinkFrameworkRuntime start(DefaultZLinkFrameworkOptions options) {
        return ZLinkFrameworkRuntime.startHost(
                options,
                new ZLinkJavaBackendAdapterFactory(),
                ZLinkHandlerActivator.reflection(),
                null);
    }

    public static ZLinkFrameworkRuntime start(
            DefaultZLinkFrameworkOptions options, ZLinkBackendAdapterProvider backendProvider) {
        return ZLinkFrameworkRuntime.start(options, backendProvider);
    }

    public static ZLinkFrameworkRuntime startHost(
            DefaultZLinkFrameworkOptions options, ZLinkBackendAdapterProvider backendProvider) {
        return ZLinkFrameworkRuntime.startHost(
                options, backendProvider, ZLinkHandlerActivator.reflection(), null);
    }

    public static CompletionStage<Void> startupCompletion(ZLinkFrameworkRuntime runtime) {
        return runtime.startupCompletion();
    }

    public static ZLinkFrameworkRuntimeState state(ZLinkFrameworkRuntime runtime) {
        return runtime.state();
    }

    public static ZLinkChannelRuntime channels(ZLinkFrameworkRuntime runtime) {
        return runtime.channels();
    }

    public static systems.zlink.framework.runtime.spots.ZLinkSpotRuntime spots(
            ZLinkFrameworkRuntime runtime) {
        return runtime.spots();
    }

    public static systems.zlink.framework.runtime.actors.ZLinkActorRuntime actors(
            ZLinkFrameworkRuntime runtime) {
        return runtime.actors();
    }

    public static ZLinkMeshDrainCoordinator meshDrains(ZLinkFrameworkRuntime runtime) {
        return runtime.meshDrains;
    }

    public static ZLinkLocationRuntime locationRuntime(ZLinkFrameworkRuntime runtime) {
        return runtime.locationRuntime;
    }

    public static Object locationAutoConnectHost(ZLinkFrameworkRuntime runtime) {
        return runtime.locationAutoConnectHost;
    }

    public static Object spotRetire(ZLinkFrameworkRuntime runtime) {
        return runtime.spotRetire;
    }

    public static ZLinkStoreLocationResolvers storeLocationResolvers(
            ZLinkFrameworkRuntime runtime) {
        return runtime.storeLocationResolvers;
    }

    public static boolean ownerAdmissionOpen(ZLinkFrameworkRuntime runtime) {
        return runtime.ownerAdmissionOpen();
    }

    /** Runs one owner lease renewal now; completes after any republication it triggers. */
    public static CompletionStage<Boolean> renewOwnerLeaseOnce(ZLinkFrameworkRuntime runtime) {
        return runtime.renewOwnerLeaseOnce();
    }

    public static ZLinkFrameworkRuntime start(
            DefaultZLinkFrameworkOptions options,
            ZLinkBackendAdapterProvider backendProvider,
            ZLinkHandlerActivator handlerActivator,
            ZLinkRuntimeEventDispatcher eventDispatcher) {
        return ZLinkFrameworkRuntime.start(
                options, backendProvider, handlerActivator, eventDispatcher);
    }

    public static ZLinkFrameworkRuntime startWithConstruction(
            DefaultZLinkFrameworkOptions options,
            ZLinkBackendAdapterProvider backendProvider,
            ZLinkHandlerActivator handlerActivator,
            ZLinkRuntimeEventDispatcher eventDispatcher,
            Construction construction) {
        return ZLinkFrameworkRuntime.start(
                options,
                backendProvider,
                handlerActivator,
                eventDispatcher,
                construction.toRuntimeConstruction());
    }

    public static ZLinkFrameworkRuntime prepareWithConstruction(
            DefaultZLinkFrameworkOptions options, Construction construction) {
        return ZLinkFrameworkRuntime.prepareHost(
                options, null, construction.toRuntimeConstruction());
    }

    public static void startPreparedWithConstruction(
            ZLinkFrameworkRuntime runtime,
            DefaultZLinkFrameworkOptions options,
            ZLinkBackendAdapterProvider backend,
            Construction construction) {
        ZLinkFrameworkRuntime.startPreparedHost(
                runtime,
                options,
                backend,
                ZLinkHandlerActivator.reflection(),
                construction.toRuntimeConstruction());
    }

    public static ZLinkFrameworkRuntime startHostWithConstruction(
            DefaultZLinkFrameworkOptions options,
            ZLinkBackendAdapterProvider backendProvider,
            ZLinkHandlerActivator handlerActivator,
            ZLinkRuntimeEventDispatcher eventDispatcher,
            Construction construction) {
        var runtime =
                startWithConstruction(
                        options, backendProvider, handlerActivator, eventDispatcher, construction);
        runtime.startupCompletion().toCompletableFuture().join();
        return runtime;
    }

    public static ZLinkBackendAdapterProvider withMeshNodeDecorator(
            ZLinkBackendAdapterProvider delegate,
            java.util.function.UnaryOperator<ZLinkInternalMeshNode> decorator) {
        return new ZLinkBackendAdapterProvider() {
            @Override
            public ZLinkChannelBackendAdapter createChannelAdapter(
                    ZLinkBackendAdapterOptions options) {
                return delegate.createChannelAdapter(options);
            }

            @Override
            public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
                return delegate.createSpotAdapter(options);
            }

            @Override
            public ZLinkMeshBackendAdapter createMeshAdapter(ZLinkBackendAdapterOptions options) {
                ZLinkMeshBackendAdapter adapter = delegate.createMeshAdapter(options);
                return (context, meshName) ->
                        decorator.apply(adapter.createMeshNode(context, meshName));
            }

            @Override
            public ZLinkStreamBackendAdapter createStreamAdapter(
                    ZLinkBackendAdapterOptions options) {
                return delegate.createStreamAdapter(options);
            }

            @Override
            public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
                    ZLinkBackendAdapterOptions options) {
                return delegate.createMonitoringAdapter(options);
            }
        };
    }
}
