package systems.zlink.framework.runtime.host;

import java.time.Duration;
import java.util.Objects;

public record ZLinkFrameworkRelocationOptions(
    ZLinkFrameworkRelocationMode mode,
    Long targetApplicationVersion,
    Duration deadline) {
    public ZLinkFrameworkRelocationOptions {
        Objects.requireNonNull(mode, "mode");
        if (deadline != null && (deadline.isZero() || deadline.isNegative())) {
            throw new IllegalArgumentException("relocation deadline must be positive");
        }
    }
}
