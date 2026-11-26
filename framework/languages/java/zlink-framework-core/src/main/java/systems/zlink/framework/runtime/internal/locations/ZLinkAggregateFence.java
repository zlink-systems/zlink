package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;
import java.util.UUID;

public record ZLinkAggregateFence(UUID aggregateId, long aggregateGeneration) {
    public ZLinkAggregateFence {
        Objects.requireNonNull(aggregateId, "aggregateId");
    }

    /** Markerless relocation key identity; target attempt generation remains a separate fence. */
    public static ZLinkAggregateFence forRelocation(UUID relocationId) {
        return new ZLinkAggregateFence(relocationId, 1);
    }
}
