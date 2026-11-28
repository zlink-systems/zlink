package systems.zlink.framework.runtime.binding;

import systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryOwner;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendContext;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;
import systems.zlink.framework.runtime.internal.backend.ZLinkMeshBackendAdapter;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;

import java.util.Objects;
import java.util.function.Supplier;

final class ZLinkJavaMeshBackendAdapter implements ZLinkMeshBackendAdapter {
    private final ZLinkTelemetryOwner.Host telemetryHost;
    private final Supplier<ZLinkStateLane> rawSpotStateLaneFactory;

    ZLinkJavaMeshBackendAdapter(
            Supplier<ZLinkStateLane> rawSpotStateLaneFactory,
            ZLinkTelemetryOwner.Host telemetryHost) {
        this.telemetryHost = telemetryHost;
        this.rawSpotStateLaneFactory =
                Objects.requireNonNull(rawSpotStateLaneFactory, "rawSpotStateLaneFactory");
    }

    @Override
    public ZLinkInternalMeshNode createMeshNode(ZLinkBackendContext context, String meshName) {
        var source = (ZLinkJavaContext) context;
        return new ZLinkJavaRawMeshNode(
                source.nativeContext(), meshName, rawSpotStateLaneFactory.get(), telemetryHost);
    }
}
