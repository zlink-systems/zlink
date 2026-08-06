package systems.zlink.framework.monitoring;

import java.time.Instant;
import java.util.Objects;

/** The advertised endpoint confirmed for one local listener. */
public record ZLinkListenerStatus(
    ZLinkListenerKind kind,
    String name,
    String endpoint,
    Instant observedAt) {
    public ZLinkListenerStatus {
        Objects.requireNonNull(kind, "kind");
        if (name == null || name.isBlank()) {
            throw new IllegalArgumentException("name is required");
        }
        if (endpoint == null || endpoint.isBlank()) {
            throw new IllegalArgumentException("endpoint is required");
        }
        Objects.requireNonNull(observedAt, "observedAt");
    }
}
