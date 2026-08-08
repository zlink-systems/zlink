package systems.zlink.framework.channels;

public interface ZLinkRouteMeshRuntimeOptions {
    ZLinkMeshChannelRuntimeOptions channel(String meshName, String channelName);

    ZLinkMeshPlacementRuntimeOptions mesh(String meshName);

    ZLinkMeshChannelRuntimeOptions channel(String channelName);
}
