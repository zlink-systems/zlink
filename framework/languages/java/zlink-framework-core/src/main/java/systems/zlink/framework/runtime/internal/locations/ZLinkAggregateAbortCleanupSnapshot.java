package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

/**
 * Durable aggregate-abort terminal cleanup state. The aggregate tombstone
 * retains this root identity until every earlier cleanup step has completed.
 */
public record ZLinkAggregateAbortCleanupSnapshot(
    ZLinkAggregateFence fence,
    String storeVersion,
    String reference,
    long checksumCrc32c) {
    public ZLinkAggregateAbortCleanupSnapshot {
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(storeVersion, "storeVersion");
        Objects.requireNonNull(reference, "reference");
        if (storeVersion.isBlank() || reference.isBlank()) {
            throw new IllegalArgumentException(
                "aggregate abort cleanup state is invalid");
        }
    }
}
