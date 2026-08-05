package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkRelocationCapacityAlreadyReserved(
    ZLinkRelocationCapacityFence fence)
    implements ZLinkRelocationCapacityReserveResult {
    public ZLinkRelocationCapacityAlreadyReserved {
        Objects.requireNonNull(fence, "fence");
    }
}
