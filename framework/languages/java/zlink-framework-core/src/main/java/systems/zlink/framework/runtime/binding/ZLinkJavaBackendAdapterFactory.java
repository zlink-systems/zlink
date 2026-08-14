package systems.zlink.framework.runtime.binding;
import java.time.Duration;
import java.util.function.Function;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;

public final class ZLinkJavaBackendAdapterFactory implements ZLinkBackendAdapterProvider {
    private static ZLinkJavaAdmissionBacked admission(
        ZLinkBackendObject backend) {
        return (ZLinkJavaAdmissionBacked) backend;
    }

    @Override
    public Function<
        ZLinkBackendObject,
        Duration> admissionTimeout() {
        return backend -> admission(backend).admissionTimeout();
    }

    @Override
    public ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options) {
        return new ZLinkJavaChannelBackendAdapter();
    }

    @Override
    public ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options) {
        throw new UnsupportedOperationException(
            "legacy SpotNode backend was removed; configure RouteMesh with addRouteMesh");
    }

    @Override
    public ZLinkMeshBackendAdapter createMeshAdapter(ZLinkBackendAdapterOptions options) {
        return new ZLinkJavaMeshBackendAdapter();
    }

    @Override
    public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
        return new ZLinkJavaStreamBackendAdapter();
    }

    @Override
    public ZLinkMonitoringBackendAdapter createMonitoringAdapter(ZLinkBackendAdapterOptions options) {
        return socket -> new ZLinkJavaSocketMonitor(((ZLinkJavaSocketBacked) socket).nativeSocket().monitorOpen());
    }
}
