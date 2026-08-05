package systems.zlink.framework.runtime.host;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkLocationRuntimeQuery;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.internal.handlers.ZLinkHandlerActivator;
import systems.zlink.framework.runtime.locations.ZLinkLiveLocationRows;
import systems.zlink.framework.runtime.locations.ZLinkLocationAutoConnectHost;
import systems.zlink.framework.runtime.locations.ZLinkLocationLifecycle;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntimeQueryService;
import systems.zlink.framework.runtime.locations.ZLinkLocationStoreResolver;
import systems.zlink.framework.runtime.locations.ZLinkRegisteredLocationStores;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;
import systems.zlink.framework.runtime.internal.channels.ZLinkClientServerRuntimeConfiguration;
import systems.zlink.framework.runtime.internal.channels.ZLinkFanoutRuntimeConfiguration;
import systems.zlink.framework.spots.SpotHandleResolver;
import systems.zlink.framework.spots.ActorSpotHandleResolver;
import systems.zlink.framework.spots.ZLinkStoreSpotHandleResolver;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;

final class ZLinkFrameworkLocationSubsystem {
    private final ZLinkRegisteredLocationStores locationStores;
    private final ZLinkLocationRuntime locationRuntime;
    private final ZLinkLocationRuntimeQuery locationRuntimeQuery;
    private final ZLinkLocationLifecycle locationLifecycle;
    private final ZLinkLocationAutoConnectHost locationAutoConnectHost;
    private final SpotTransportAddressResolver spotTransportAddressResolver;
    private final ZLinkStoreLocationResolvers storeLocationResolvers;
    private final java.util.concurrent.CompletionStage<Void> startup;

    private ZLinkFrameworkLocationSubsystem(
        ZLinkRegisteredLocationStores locationStores,
        ZLinkLocationRuntime locationRuntime,
        ZLinkLocationRuntimeQuery locationRuntimeQuery,
        ZLinkLocationLifecycle locationLifecycle,
        ZLinkLocationAutoConnectHost locationAutoConnectHost,
        SpotTransportAddressResolver spotTransportAddressResolver,
        ZLinkStoreLocationResolvers storeLocationResolvers,
        java.util.concurrent.CompletionStage<Void> startup) {
        this.locationStores = locationStores;
        this.locationRuntime = locationRuntime;
        this.locationRuntimeQuery = locationRuntimeQuery;
        this.locationLifecycle = locationLifecycle;
        this.locationAutoConnectHost = locationAutoConnectHost;
        this.spotTransportAddressResolver = spotTransportAddressResolver;
        this.storeLocationResolvers = storeLocationResolvers;
        this.startup = startup;
    }

    static ZLinkFrameworkLocationSubsystem create(
        ZLinkFrameworkRegistration registration,
        ZLinkHandlerActivator.MutableServices runtimeHandlers) {
        ZLinkRegisteredLocationStores locationStores = ZLinkLocationStoreResolver.resolve(
            registration.locations(),
            runtimeHandlers);
        if (locationStores == null) {
            return disabled();
        }

        locationStores.addTo(runtimeHandlers);
        ZLinkLocationRuntime locationRuntime = new ZLinkLocationRuntime(
            locationStores,
            registration.locations().options().ownerLeaseTtl(),
            registration.locations().options().ownerLeaseRenewInterval());
        java.util.concurrent.CompletionStage<Void> startup =
            locationRuntime.start(RoutingId.from(locationRuntime.ownerId()));

        ZLinkLiveLocationRows liveLocationRows = ZLinkLiveLocationRows.create(
            locationStores,
            registration.locations().options());
        ZLinkLocationRuntimeQuery locationRuntimeQuery = new ZLinkLocationRuntimeQueryService(
            locationStores,
            locationRuntime,
            registration.locations().options(),
            liveLocationRows,
            registration.meshNodes().stream()
                .map(systems.zlink.framework.runtime.mesh.MeshNodeRegistration::meshName)
                .distinct()
                .toList());
        ZLinkLocationLifecycle locationLifecycle = new ZLinkLocationLifecycle(locationRuntime);
        ZLinkStoreLocationResolvers storeLocationResolvers =
            new ZLinkStoreLocationResolvers(locationStores, liveLocationRows);
        ZLinkClientServerRuntimeConfiguration clientServerConfiguration =
            new ZLinkClientServerRuntimeConfiguration(
                locationStores.clientServerStore(),
                registration.locations().options());
        runtimeHandlers.add(
            ZLinkClientServerRuntimeConfiguration.class,
            clientServerConfiguration);
        ZLinkFanoutRuntimeConfiguration fanoutConfiguration =
            new ZLinkFanoutRuntimeConfiguration(
                locationStores.fanoutStore(),
                registration.locations().options());
        runtimeHandlers.add(
            ZLinkFanoutRuntimeConfiguration.class,
            fanoutConfiguration);
        ZLinkLocationAutoConnectHost locationAutoConnectHost = new ZLinkLocationAutoConnectHost(
            locationRuntime,
            storeLocationResolvers,
            registration.locations().options(),
            clientServerConfiguration,
            fanoutConfiguration);
        ZLinkStoreLocationResolvers.AddressResolvers locationAddressResolvers =
            new ZLinkStoreLocationResolvers.AddressResolvers(
                spotMeshNames(registration),
                storeLocationResolvers);
        ZLinkStoreSpotHandleResolver locationSpotHandleResolver =
            new ZLinkStoreSpotHandleResolver(
                locationAddressResolvers,
                locationStores.authorityStore());

        runtimeHandlers.add(ZLinkLocationRuntime.class, locationRuntime);
        runtimeHandlers.add(ZLinkLocationRuntimeQuery.class, locationRuntimeQuery);
        runtimeHandlers.add(ZLinkLocationLifecycle.class, locationLifecycle);
        runtimeHandlers.add(ZLinkLocationAutoConnectHost.class, locationAutoConnectHost);
        runtimeHandlers.add(ZLinkStoreLocationResolvers.class, storeLocationResolvers);
        runtimeHandlers.add(ZLinkStoreLocationResolvers.AddressResolvers.class, locationAddressResolvers);
        runtimeHandlers.add(ZLinkStoreSpotHandleResolver.class, locationSpotHandleResolver);
        runtimeHandlers.add(SpotHandleResolver.class, locationSpotHandleResolver);
        runtimeHandlers.add(ActorSpotHandleResolver.class, locationSpotHandleResolver);
        runtimeHandlers.add(SpotTransportAddressResolver.class, locationSpotHandleResolver);

        return new ZLinkFrameworkLocationSubsystem(
            locationStores,
            locationRuntime,
            locationRuntimeQuery,
            locationLifecycle,
            locationAutoConnectHost,
            locationSpotHandleResolver,
            storeLocationResolvers,
            startup);
    }

    boolean enabled() {
        return locationStores != null;
    }

    ZLinkRegisteredLocationStores locationStores() {
        return locationStores;
    }

    ZLinkLocationRuntime locationRuntime() {
        return locationRuntime;
    }

    ZLinkLocationRuntimeQuery locationRuntimeQuery() {
        return locationRuntimeQuery;
    }

    ZLinkLocationLifecycle locationLifecycle() {
        return locationLifecycle;
    }

    ZLinkLocationAutoConnectHost locationAutoConnectHost() {
        return locationAutoConnectHost;
    }

    SpotTransportAddressResolver spotTransportAddressResolver() {
        return spotTransportAddressResolver;
    }

    ZLinkStoreLocationResolvers storeLocationResolvers() {
        return storeLocationResolvers;
    }

    java.util.concurrent.CompletionStage<Void> startup() {
        return startup;
    }

    private static ZLinkFrameworkLocationSubsystem disabled() {
        return new ZLinkFrameworkLocationSubsystem(
            null, null, null, null, null, null, null,
            java.util.concurrent.CompletableFuture.completedFuture(null));
    }

    private static java.util.List<String> spotMeshNames(ZLinkFrameworkRegistration registration) {
        return java.util.stream.Stream.concat(
                registration.spotNodes().stream()
                    .map(systems.zlink.framework.runtime.spots.SpotNodeRegistration::meshName),
                registration.meshNodes().stream()
                    .filter(node -> !node.spotFactories().isEmpty()
                        || !node.entrySpots().isEmpty()
                        || !node.actorFactories().isEmpty()
                        || !node.channelNames().isEmpty())
                    .map(systems.zlink.framework.runtime.mesh.MeshNodeRegistration::meshName))
            .distinct()
            .toList();
    }
}
