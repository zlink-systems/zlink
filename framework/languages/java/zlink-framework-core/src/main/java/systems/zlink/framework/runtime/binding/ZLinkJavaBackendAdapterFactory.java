package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterProvider;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;

public final class ZLinkJavaBackendAdapterFactory implements ZLinkBackendAdapterProvider {
    private static ZLinkJavaAdmissionBacked admission(
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject backend) {
        return (ZLinkJavaAdmissionBacked) backend;
    }

    @Override
    public java.util.function.Function<
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject> admissionSource() {
        return backend -> admission(backend).admissionSource();
    }

    @Override
    public java.util.function.Function<
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
        java.time.Duration> admissionTimeout() {
        return backend -> admission(backend).admissionTimeout();
    }

    @Override
    public java.util.function.ToIntFunction<
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject>
        admissionPendingCapacity() {
        return backend -> admission(backend).admissionPendingCapacity();
    }

    @Override
    public java.util.function.BiConsumer<
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
        java.util.function.Consumer<
            systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey>>
        admissionReadyRegistrar() {
        return (backend, handler) ->
            admission(backend).setAdmissionReadyHandler(handler);
    }

    @Override
    public java.util.function.BiConsumer<
        systems.zlink.framework.runtime.internal.backend.ZLinkBackendObject,
        Runnable> admissionShutdownRegistrar() {
        return (backend, handler) ->
            admission(backend).setAdmissionShutdownHandler(handler);
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
