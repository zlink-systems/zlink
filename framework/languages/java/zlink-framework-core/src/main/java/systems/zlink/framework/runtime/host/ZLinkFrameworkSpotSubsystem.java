package systems.zlink.framework.runtime.host;

import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.runtime.internal.monitoring.ZLinkRuntimeEventDispatcher;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.DefaultZLinkFrameworkOptions;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.locations.ZLinkLocationLifecycle;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;
import systems.zlink.framework.spots.ZLinkSpotManager;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;

final class ZLinkFrameworkSpotSubsystem {
    private final ZLinkSpotRuntime spots;
    private final SpotTransportAddressResolver remoteAddressResolver;
    private final java.util.concurrent.CompletionStage<Void> startup;

    private ZLinkFrameworkSpotSubsystem(
        ZLinkSpotRuntime spots,
        SpotTransportAddressResolver remoteAddressResolver,
        java.util.concurrent.CompletionStage<Void> startup) {
        this.spots = spots;
        this.remoteAddressResolver = remoteAddressResolver;
        this.startup = startup;
    }

    static ZLinkFrameworkSpotSubsystem create(
        DefaultZLinkFrameworkOptions options,
        ZLinkBackendAdapterProvider backendFactory,
        ZLinkBackendAdapterOptions adapterOptions,
        ZLinkMessageSerializer serializer,
        ZLinkHandlerActivator.MutableServices runtimeHandlers,
        ZLinkRuntimeEventDispatcher eventDispatcher,
        ZLinkChannelRuntime channels,
        ZLinkBackendContext backendContext,
        ZLinkLocationLifecycle locationLifecycle,
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository authorityStore,
        systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository locationStore,
        systems.zlink.framework.runtime.locations.ZLinkLocationRuntime
            locationRuntime,
        SpotTransportAddressResolver locationTransportResolver,
        java.util.Map<String, systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode>
            meshNodes) {
        SpotTransportAddressResolver remoteAddressResolver = locationTransportResolver;
        boolean hasMeshServices = options.registration().meshNodes().stream()
            .anyMatch(node -> node.objectRoleEnabled()
                || !node.spotFactories().isEmpty()
                || !node.entrySpots().isEmpty()
                || !node.actorFactories().isEmpty()
                || !node.channelNames().isEmpty());
        if (options.registration().spotNodes().isEmpty() && !hasMeshServices) {
            return new ZLinkFrameworkSpotSubsystem(
                null, remoteAddressResolver,
                java.util.concurrent.CompletableFuture.completedFuture(null));
        }

        ZLinkSpotRuntime spots = new ZLinkSpotRuntime(
            backendFactory,
            adapterOptions,
            options.registration(),
            channels,
            backendContext,
            serializer,
            runtimeHandlers,
            eventDispatcher,
            meshNodes,
            ZLinkAdmissionRuntime.factory(backendFactory));
        spots.setLocationLifecycle(locationLifecycle);
        if (channels != null) {
            channels.registerInstanceSpotCallRuntime(
                spots.instanceSpotCalls());
        }
        if (authorityStore != null
            && locationStore != null
            && locationRuntime != null) {
            spots.installUserSpotOperationHandlers(
                authorityStore,
                locationStore,
                locationRuntime);
        }
        java.util.concurrent.CompletionStage<Void> startup = spots.claimEntrySpotLocations();
        runtimeHandlers.add(ZLinkSpotManager.class, spots);
        if (!options.registration().spotNodes().isEmpty()) {
            channels.registerSpotRouteBridgeOwner(spots::primaryNode);
            channels.registerSpotRouteBridgeDispatchDrainer(spots::drainRoutedDispatchQueues);
        }
        return new ZLinkFrameworkSpotSubsystem(spots, remoteAddressResolver, startup);
    }

    ZLinkSpotRuntime spots() {
        return spots;
    }

    SpotTransportAddressResolver remoteAddressResolver() {
        return remoteAddressResolver;
    }

    java.util.concurrent.CompletionStage<Void> startup() {
        return startup;
    }

}
