package systems.zlink.framework.runtime.host;

import java.util.Objects;

public record ZLinkFrameworkTerminationResult(
    ZLinkFrameworkTerminationOutcome outcome,
    ZLinkFrameworkTerminationReason reason) {
    public ZLinkFrameworkTerminationResult {
        Objects.requireNonNull(outcome, "outcome");
        Objects.requireNonNull(reason, "reason");
    }
}
