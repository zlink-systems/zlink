package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.Objects;
import java.util.function.Supplier;

public final class ZLinkJavaBackendAdapterFactory implements ZLinkBackendAdapterProvider {
    private final Supplier<ZLinkStateLane> rawSpotStateLaneFactory;

    public ZLinkJavaBackendAdapterFactory() {
        this(ZLinkStateLane::new);
    }

    ZLinkJavaBackendAdapterFactory(Supplier<ZLinkStateLane> rawSpotStateLaneFactory) {
        this.rawSpotStateLaneFactory =
                Objects.requireNonNull(rawSpotStateLaneFactory, "rawSpotStateLaneFactory");
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
        return new ZLinkJavaMeshBackendAdapter(rawSpotStateLaneFactory);
    }

    @Override
    public ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options) {
        return new ZLinkJavaStreamBackendAdapter();
    }

    @Override
    public ZLinkMonitoringBackendAdapter createMonitoringAdapter(
            ZLinkBackendAdapterOptions options) {
        return socket ->
                new ZLinkJavaSocketMonitor(
                        ((ZLinkJavaSocketBacked) socket).nativeSocket().monitorOpen());
    }
}
