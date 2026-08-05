package systems.zlink.framework.runtime.host;

import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.locations.ZLinkLocationAutoConnectHost;
import systems.zlink.framework.runtime.mesh.ZLinkMeshNodesRuntime;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;

final class ZLinkFrameworkAutoConnectSubsystem {
    private ZLinkFrameworkAutoConnectSubsystem() {
    }

    static java.util.concurrent.CompletionStage<Void> start(
        ZLinkLocationAutoConnectHost locationAutoConnectHost,
        ZLinkFrameworkRegistration registration,
        ZLinkChannelRuntime channels,
        ZLinkMeshNodesRuntime meshNodes,
        ZLinkSpotRuntime spots) {
        if (locationAutoConnectHost == null) {
            return java.util.concurrent.CompletableFuture.completedFuture(null);
        }

        return locationAutoConnectHost.start(
            registration,
            channels,
            meshNodes == null ? java.util.Map.of() : meshNodes.nodesByName(),
            spots == null ? java.util.Map.of() : spots.nodesByName(),
            spots);
    }
}
