package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

/** A committed aggregate marker together with its provider StoreVersion. */
public record ZLinkAggregateProgressSnapshot(
    ZLinkAggregateFence fence,
    String storeVersion,
    ZLinkAggregatePrepareRequest request) {
    public ZLinkAggregateProgressSnapshot {
        Objects.requireNonNull(fence, "fence");
        if (storeVersion == null || storeVersion.isBlank()) {
            throw new IllegalArgumentException(
                "aggregate progress StoreVersion must be non-blank");
        }
        Objects.requireNonNull(request, "request");
        if (!request.aggregateId().equals(fence.aggregateId())
            || request.aggregateGeneration() != fence.aggregateGeneration()) {
            throw new IllegalArgumentException(
                "aggregate progress fence differs from its request");
        }
    }
}
