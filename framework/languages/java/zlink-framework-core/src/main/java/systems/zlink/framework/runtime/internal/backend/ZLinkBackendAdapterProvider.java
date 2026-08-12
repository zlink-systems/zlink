package systems.zlink.framework.runtime.internal.backend;
import java.time.Duration;
import java.util.function.BiConsumer;
import java.util.function.Consumer;
import java.util.function.Function;
import java.util.function.ToIntFunction;

import systems.zlink.framework.runtime.internal.backend.ZLinkBackendAdapterOptions;
import systems.zlink.framework.runtime.internal.backend.ZLinkChannelBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMonitoringBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkSpotBackendAdapter;
import systems.zlink.framework.runtime.internal.backend.ZLinkStreamBackendAdapter;

public interface ZLinkBackendAdapterProvider {
    ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options);

    ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options);

    default ZLinkMeshBackendAdapter createMeshAdapter(ZLinkBackendAdapterOptions options) {
        throw new UnsupportedOperationException("MeshNode backend is not available");
    }

    ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options);

    ZLinkMonitoringBackendAdapter createMonitoringAdapter(ZLinkBackendAdapterOptions options);

    default Function<
        ZLinkBackendObject,
        ZLinkBackendObject> admissionSource() {
        return backend -> backend;
    }

    default Function<
        ZLinkBackendObject,
        Duration> admissionTimeout() {
        return ignored -> Duration.ofSeconds(1);
    }

    default ToIntFunction<
        ZLinkBackendObject>
        admissionPendingCapacity() {
        return ignored -> 4096;
    }

    default BiConsumer<
        ZLinkBackendObject,
        Consumer<
            ZLinkBackendAdmissionKey>>
        admissionReadyRegistrar() {
        return (ignored, handler) -> { };
    }

    default BiConsumer<
        ZLinkBackendObject,
        Runnable> admissionShutdownRegistrar() {
        return (ignored, handler) -> { };
    }
}
