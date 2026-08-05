package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkRelocationCapacityReserved(
    ZLinkRelocationCapacityFence fence)
    implements ZLinkRelocationCapacityReserveResult {
    public ZLinkRelocationCapacityReserved {
        Objects.requireNonNull(fence, "fence");
    }
}
