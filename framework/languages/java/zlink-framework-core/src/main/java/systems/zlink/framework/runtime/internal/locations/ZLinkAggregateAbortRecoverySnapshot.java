package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

/**
 * Durable aggregate-abort tombstone retained until an external relocation
 * terminal has been acknowledged.
 */
public record ZLinkAggregateAbortRecoverySnapshot(
    ZLinkAggregateFence fence,
    String storeVersion,
    ZLinkAggregatePrepareRequest request) {
    public ZLinkAggregateAbortRecoverySnapshot {
        Objects.requireNonNull(fence, "fence");
        Objects.requireNonNull(storeVersion, "storeVersion");
        Objects.requireNonNull(request, "request");
        if (storeVersion.isBlank()
            || !request.aggregateId().equals(fence.aggregateId())
            || request.aggregateGeneration() != fence.aggregateGeneration()) {
            throw new IllegalArgumentException(
                "aggregate abort recovery fence is invalid");
        }
    }
}
