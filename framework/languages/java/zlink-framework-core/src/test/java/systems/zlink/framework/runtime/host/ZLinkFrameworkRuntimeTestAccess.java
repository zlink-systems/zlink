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
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;
import systems.zlink.framework.runtime.locations.ZLinkStatefulAuthorityRouteRuntime;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;

import java.util.concurrent.CompletionStage;
import java.util.concurrent.ExecutorService;
import java.util.function.Supplier;

/** Test-only access to the package-owned runtime bootstrap. */
public final class ZLinkFrameworkRuntimeTestAccess {
    private ZLinkFrameworkRuntimeTestAccess() {}

    public static final class Construction {
        private Supplier<ZLinkMeshDrainCoordinator> meshDrains;
        private SpotTransportAddressResolver channelSpotAddressResolver;
        private ZLinkLocationRuntime locationRuntime;
        private java.util.function.UnaryOperator<ZLinkLocationRepository>
                spotOperationAuthorityStore;
        private ZLinkLocationRepository spotLocationStore;
        private ZLinkLocationRuntime spotLocationRuntime;
        private ZLinkStoreLocationResolvers spotLocationResolvers;

        public Construction meshDrains(Supplier<ZLinkMeshDrainCoordinator> value) {
            meshDrains = value;
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

        private ZLinkFrameworkRuntimeConstruction toRuntimeConstruction() {
            return new ZLinkFrameworkRuntimeConstruction(
                    meshDrains,
                    channelSpotAddressResolver,
                    locationRuntime,
                    spotOperationAuthorityStore,
                    spotLocationStore,
                    spotLocationRuntime,
                    spotLocationResolvers);
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

    public static ZLinkChannelRuntime channels(ZLinkFrameworkRuntime runtime) {
        return runtime.channels;
    }

    public static ZLinkMeshDrainCoordinator meshDrains(ZLinkFrameworkRuntime runtime) {
        return runtime.meshDrains;
    }

    public static ZLinkStatefulAuthorityRouteRuntime authorityRouteRuntime(
            ZLinkFrameworkRuntime runtime) {
        return runtime.authorityRouteRuntime;
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

    public static java.util.Map<
                    String, systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode>
            meshNodes(ZLinkFrameworkRuntime runtime) {
        return runtime.meshNodes.nodesByName();
    }

    public static void replaceSpotRetire(
            ZLinkFrameworkRuntime runtime,
            systems.zlink.framework.runtime.spots.ZLinkUserSpotRetireRuntime spotRetire) {
        runtime.spotRetire = java.util.Objects.requireNonNull(spotRetire, "spotRetire");
    }

    public static ZLinkStoreLocationResolvers storeLocationResolvers(
            ZLinkFrameworkRuntime runtime) {
        return runtime.storeLocationResolvers;
    }

    public static ZLinkLocationRepository authorityStore(ZLinkFrameworkRuntime runtime) {
        return runtime.authorityStoreForRuntimeConstruction();
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

    public static ZLinkFrameworkRuntime startWithChannelInfrastructureExecutor(
            DefaultZLinkFrameworkOptions options,
            ZLinkBackendAdapterProvider backendProvider,
            ExecutorService infrastructureExecutor) {
        var runtime = start(options, backendProvider);
        var adapterOptions = new ZLinkBackendAdapterOptions(options.defaultRequestTimeout());
        var backend = backendProvider.createChannelAdapter(adapterOptions);
        var context = runtime.backendContextForRuntimeConstruction();
        var replacement =
                systems.zlink.framework.runtime.channels.ZLinkChannelRuntimeTestAccess
                        .withInfrastructureExecutor(
                                backend,
                                context,
                                backendProvider,
                                adapterOptions,
                                options.registration(),
                                ZLinkFrameworkRuntime.serializerFor(options),
                                ZLinkHandlerActivator.reflection(),
                                null,
                                infrastructureExecutor);
        replacement.setHostStateSupplier(() -> runtime.status().state());
        var previous = runtime.channels;
        runtime.channels = replacement;
        previous.close();
        return runtime;
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
