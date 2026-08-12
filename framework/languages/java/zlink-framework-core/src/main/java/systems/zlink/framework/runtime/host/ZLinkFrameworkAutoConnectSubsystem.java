package systems.zlink.framework.runtime.host;
import java.util.Map;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionStage;

import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.configuration.ZLinkFrameworkRegistration;
import systems.zlink.framework.runtime.locations.ZLinkLocationAutoConnectHost;
import systems.zlink.framework.runtime.mesh.ZLinkMeshNodesRuntime;
import systems.zlink.framework.runtime.spots.ZLinkSpotRuntime;

final class ZLinkFrameworkAutoConnectSubsystem {
    private ZLinkFrameworkAutoConnectSubsystem() {
    }

    static CompletionStage<Void> start(
        ZLinkLocationAutoConnectHost locationAutoConnectHost,
        ZLinkFrameworkRegistration registration,
        ZLinkChannelRuntime channels,
        ZLinkMeshNodesRuntime meshNodes,
        ZLinkSpotRuntime spots) {
        if (locationAutoConnectHost == null) {
            return CompletableFuture.completedFuture(null);
        }

        return locationAutoConnectHost.start(
            registration,
            channels,
            meshNodes == null ? Map.of() : meshNodes.nodesByName(),
            spots == null ? Map.of() : spots.nodesByName(),
            spots);
    }
}
