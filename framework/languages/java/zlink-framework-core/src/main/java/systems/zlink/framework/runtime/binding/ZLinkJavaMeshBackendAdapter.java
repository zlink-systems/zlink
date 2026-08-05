package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

final class ZLinkJavaMeshBackendAdapter implements ZLinkMeshBackendAdapter {
    @Override
    public ZLinkInternalMeshNode createMeshNode(
        ZLinkBackendContext context,
        String meshName) {
        return new ZLinkJavaRawMeshNode(
            ((ZLinkJavaContext) context).nativeContext(),
            meshName);
    }
}
