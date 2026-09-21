package systems.zlink.framework.runtime.internal.backend;

import java.time.Duration;
import java.util.function.Function;

public interface ZLinkBackendAdapterProvider {
    ZLinkChannelBackendAdapter createChannelAdapter(ZLinkBackendAdapterOptions options);

    ZLinkSpotBackendAdapter createSpotAdapter(ZLinkBackendAdapterOptions options);

    default ZLinkMeshBackendAdapter createMeshAdapter(ZLinkBackendAdapterOptions options) {
        throw new UnsupportedOperationException("MeshNode backend is not available");
    }

    ZLinkStreamBackendAdapter createStreamAdapter(ZLinkBackendAdapterOptions options);

    ZLinkMonitoringBackendAdapter createMonitoringAdapter(ZLinkBackendAdapterOptions options);

    default Function<ZLinkBackendObject, Duration> admissionTimeout() {
        return ignored -> Duration.ofSeconds(1);
    }
}
