package systems.zlink.framework.runtime.binding;
import java.time.Duration;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.ToIntFunction;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdmissionKey;
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
        ZLinkBackendObject> admissionSource() {
        return backend -> admission(backend).admissionSource();
    }

    @Override
    public Function<
        ZLinkBackendObject,
        Duration> admissionTimeout() {
        return backend -> admission(backend).admissionTimeout();
    }

    @Override
    public ToIntFunction<
        ZLinkBackendObject>
        admissionPendingCapacity() {
        return backend -> admission(backend).admissionPendingCapacity();
    }

    @Override
    public BiConsumer<
        ZLinkBackendObject,
        Consumer<
            ZLinkBackendAdmissionKey>>
        admissionReadyRegistrar() {
        return (backend, handler) ->
            admission(backend).setAdmissionReadyHandler(handler);
    }

    @Override
    public BiConsumer<
        ZLinkBackendObject,
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
