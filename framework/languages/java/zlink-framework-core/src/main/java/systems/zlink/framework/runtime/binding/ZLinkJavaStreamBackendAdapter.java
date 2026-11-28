package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.core.Context;
import systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryOwner;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamSocket;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;

final class ZLinkJavaStreamBackendAdapter implements ZLinkStreamBackendAdapter {
    private final ZLinkTelemetryOwner.Host telemetryHost;

    ZLinkJavaStreamBackendAdapter(ZLinkTelemetryOwner.Host telemetryHost) {
        this.telemetryHost = telemetryHost;
    }

    @Override
    public ZLinkBackendStreamSocket createStreamSocket(
            ZLinkBackendContext context, ZLinkInternalMeshNode meshNode) {
        return new ZLinkJavaStreamSocket(
                ZLinkJavaSocketOptions.configureFrameworkSocket(
                        nativeContext(context).createStreamSocket()),
                meshNode == null ? null : (ZLinkJavaRawMeshNode) meshNode,
                telemetryHost);
    }

    private static Context nativeContext(ZLinkBackendContext context) {
        return ((ZLinkJavaContext) context).nativeContext();
    }
}
