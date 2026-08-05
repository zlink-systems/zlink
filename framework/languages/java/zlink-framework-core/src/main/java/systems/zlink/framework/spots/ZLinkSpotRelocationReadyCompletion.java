package systems.zlink.framework.spots;

import java.util.Objects;

public record ZLinkSpotRelocationReadyCompletion(
    ZLinkSpotRelocationReadyOutcome outcome) {
    public ZLinkSpotRelocationReadyCompletion {
        Objects.requireNonNull(outcome, "outcome");
    }
}
