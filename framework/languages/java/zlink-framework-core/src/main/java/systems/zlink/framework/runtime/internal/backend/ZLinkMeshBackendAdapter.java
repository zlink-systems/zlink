package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

public interface ZLinkMeshBackendAdapter {
    ZLinkInternalMeshNode createMeshNode(ZLinkBackendContext context, String meshName);
}
