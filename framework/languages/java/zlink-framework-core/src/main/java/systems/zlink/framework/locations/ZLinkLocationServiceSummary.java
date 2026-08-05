package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

import java.time.Instant;

public record ZLinkLocationServiceSummary(
    String meshName,
    long totalCount,
    long readyCount,
    long errorCount,
    long stoppedCount,
    Instant lastUpdatedAt) {
}
