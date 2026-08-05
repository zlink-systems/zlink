package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;
import java.util.UUID;

public record ZLinkAggregateFence(
    UUID aggregateId,
    long aggregateGeneration) {
    public ZLinkAggregateFence {
        Objects.requireNonNull(aggregateId, "aggregateId");
    }
}
