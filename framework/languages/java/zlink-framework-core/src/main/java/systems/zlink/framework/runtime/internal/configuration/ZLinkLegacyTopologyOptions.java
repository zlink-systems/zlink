package systems.zlink.framework.runtime.internal.configuration;

/** Internal bridge for tests that still exercise the pre-11 split runtimes. */
public interface ZLinkLegacyTopologyOptions {
    RouteMeshChannelBuilder addLegacyRouteMeshChannel(String channelName);

    systems.zlink.framework.runtime.spots.SpotBuilders.Mesh addLegacySpotMesh(
        String meshName);
}
