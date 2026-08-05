package systems.zlink.framework.spots;

import java.time.Instant;

public record ZLinkSpotClosingContext(
    ZLinkSpotCloseReason reason,
    Instant deadline) {
}
