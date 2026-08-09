package systems.zlink.framework.runtime.internal.configuration;
import systems.zlink.framework.runtime.spots.SpotBuilders;

/** Internal bridge for tests that still exercise the pre-11 split runtimes. */
public interface ZLinkLegacyTopologyOptions {
    RouteMeshChannelBuilder addLegacyRouteMeshChannel(String channelName);

    SpotBuilders.Mesh addLegacySpotMesh(
        String meshName);
}
