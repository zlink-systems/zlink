package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.framework.runtime.diagnostics.ZLinkTelemetryOwner;

import java.time.Duration;

public record ZLinkBackendAdapterOptions(
        Duration defaultRequestTimeout, ZLinkTelemetryOwner.Host telemetryHost) {
    public ZLinkBackendAdapterOptions(Duration defaultRequestTimeout) {
        this(defaultRequestTimeout, null);
    }
}
