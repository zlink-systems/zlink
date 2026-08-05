package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkAggregateAlreadyPrepared(ZLinkAggregateFence fence)
    implements ZLinkAggregatePrepareResult {
    public ZLinkAggregateAlreadyPrepared {
        Objects.requireNonNull(fence, "fence");
    }
}
