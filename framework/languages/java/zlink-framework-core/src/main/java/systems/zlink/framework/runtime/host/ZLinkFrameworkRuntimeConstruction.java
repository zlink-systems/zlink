package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.internal.drain.ZLinkMeshDrainCoordinator;
import systems.zlink.framework.runtime.internal.locations.ZLinkLocationRepository;
import systems.zlink.framework.runtime.internal.spots.SpotTransportAddressResolver;
import systems.zlink.framework.runtime.locations.ZLinkLocationRuntime;
import systems.zlink.framework.runtime.locations.ZLinkStoreLocationResolvers;

import java.util.function.Supplier;

/** Transient collaborator injection for package-owned runtime construction and its tests. */
record ZLinkFrameworkRuntimeConstruction(
        Supplier<ZLinkMeshDrainCoordinator> meshDrains,
        SpotTransportAddressResolver channelSpotAddressResolver,
        ZLinkLocationRuntime locationRuntime,
        java.util.function.UnaryOperator<ZLinkLocationRepository> spotOperationAuthorityStore,
        ZLinkLocationRepository spotLocationStore,
        ZLinkLocationRuntime spotLocationRuntime,
        ZLinkStoreLocationResolvers spotLocationResolvers) {
    static ZLinkFrameworkRuntimeConstruction defaults() {
        return new ZLinkFrameworkRuntimeConstruction(null, null, null, null, null, null, null);
    }
}
