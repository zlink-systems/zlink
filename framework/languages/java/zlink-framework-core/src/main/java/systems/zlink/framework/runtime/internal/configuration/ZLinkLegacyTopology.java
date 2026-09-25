package systems.zlink.framework.runtime.internal.configuration;

import systems.zlink.framework.configuration.ZLinkFrameworkOptions;
import systems.zlink.framework.runtime.channels.ZLinkChannelRuntime;
import systems.zlink.framework.runtime.host.ZLinkFrameworkRuntime;
import systems.zlink.framework.runtime.spots.SpotBuilders;

/** Internal compatibility entry point; application configuration must use addRouteMesh. */
public final class ZLinkLegacyTopology {
    private ZLinkLegacyTopology() {}

    public static RouteMeshChannelBuilder addRouteMeshChannel(
            ZLinkFrameworkOptions options, String channelName) {
        return access(options).addLegacyRouteMeshChannel(channelName);
    }

    public static SpotBuilders.Mesh addSpotMesh(ZLinkFrameworkOptions options, String meshName) {
        return access(options).addLegacySpotMesh(meshName);
    }

    /**
     * Internal test access: the endpoint a legacy route channel ROUTER bound. Legacy route channels
     * are not RouteMesh listeners, so the public listener status does not report them.
     */
    public static String routeBoundEndpoint(ZLinkFrameworkRuntime runtime, String channelName) {
        return ((ZLinkChannelRuntime) runtime.client()).legacyRouteBoundEndpoint(channelName);
    }

    private static ZLinkLegacyTopologyOptions access(ZLinkFrameworkOptions options) {
        if (options instanceof ZLinkLegacyTopologyOptions legacy) {
            return legacy;
        }
        throw new IllegalArgumentException(
                "framework options do not expose the internal legacy topology bridge");
    }
}
