package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Context;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;

final class ZLinkJavaStreamBackendAdapter implements ZLinkStreamBackendAdapter {
    @Override
    public ZLinkBackendStreamSocket createStreamSocket(
        ZLinkBackendContext context,
        systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode meshNode) {
        return new ZLinkJavaStreamSocket(
            ZLinkJavaSocketOptions.configureFrameworkSocket(
                nativeContext(context).createStreamSocket()),
            meshNode == null ? null : (ZLinkJavaRawMeshNode) meshNode);
    }

    private static Context nativeContext(ZLinkBackendContext context) {
        return ((ZLinkJavaContext) context).nativeContext();
    }
}
