package systems.zlink.framework.runtime.host;

import java.util.Objects;

record ZLinkTerminationResult(
    ZLinkTerminationIntent effectiveIntent,
    ZLinkTerminationOutcome outcome,
    ZLinkTerminationReason reason) {
    public ZLinkTerminationResult {
        Objects.requireNonNull(effectiveIntent, "effectiveIntent");
        Objects.requireNonNull(outcome, "outcome");
        Objects.requireNonNull(reason, "reason");
    }
}
