package systems.zlink.framework.channels;

public interface ZLinkRouteMeshRuntimeOptions {
    ZLinkMeshNodeRuntimeOptions meshNode(String meshName);

    ZLinkMeshChannelRuntimeOptions channel(String meshName, String channelName);

    ZLinkMeshPlacementRuntimeOptions mesh(String meshName);

    ZLinkMeshChannelRuntimeOptions channel(String channelName);
}
