package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkAggregatePrepared(ZLinkAggregateFence fence)
    implements ZLinkAggregatePrepareResult {
    public ZLinkAggregatePrepared {
        Objects.requireNonNull(fence, "fence");
    }
}
