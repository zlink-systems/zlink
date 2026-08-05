package systems.zlink.framework.runtime.host;

import java.util.Objects;

public record ZLinkFrameworkRelocationResult(
    ZLinkFrameworkRelocationMode mode,
    long effectiveTargetApplicationVersion,
    ZLinkFrameworkRelocationOutcome outcome,
    ZLinkFrameworkRelocationReason reason) {
    public ZLinkFrameworkRelocationResult {
        Objects.requireNonNull(mode, "mode");
        Objects.requireNonNull(outcome, "outcome");
        Objects.requireNonNull(reason, "reason");
    }
}
