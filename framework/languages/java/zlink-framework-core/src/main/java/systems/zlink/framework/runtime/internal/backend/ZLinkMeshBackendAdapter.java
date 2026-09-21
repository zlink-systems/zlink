package systems.zlink.framework.runtime.internal.backend;

public interface ZLinkMeshBackendAdapter {
    ZLinkInternalMeshNode createMeshNode(ZLinkBackendContext context, String meshName);
}
