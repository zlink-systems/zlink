package systems.zlink.framework.runtime.internal.configuration;

import systems.zlink.framework.configuration.ZLinkFrameworkOptions;

/** Internal compatibility entry point; application configuration must use addRouteMesh. */
public final class ZLinkLegacyTopology {
    private ZLinkLegacyTopology() {
    }

    public static RouteMeshChannelBuilder addRouteMeshChannel(
        ZLinkFrameworkOptions options,
        String channelName) {
        return access(options).addLegacyRouteMeshChannel(channelName);
    }

    public static systems.zlink.framework.runtime.spots.SpotBuilders.Mesh addSpotMesh(
        ZLinkFrameworkOptions options,
        String meshName) {
        return access(options).addLegacySpotMesh(meshName);
    }

    private static ZLinkLegacyTopologyOptions access(
        ZLinkFrameworkOptions options) {
        if (options instanceof ZLinkLegacyTopologyOptions legacy) {
            return legacy;
        }
        throw new IllegalArgumentException(
            "framework options do not expose the internal legacy topology bridge");
    }
}
