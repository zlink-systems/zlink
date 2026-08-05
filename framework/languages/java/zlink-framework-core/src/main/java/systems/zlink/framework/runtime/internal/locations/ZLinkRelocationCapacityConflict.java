package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkRelocationCapacityConflict(
    ZLinkAuthorityReadResult current)
    implements ZLinkRelocationCapacityReserveResult {
    public ZLinkRelocationCapacityConflict {
        Objects.requireNonNull(current, "current");
    }
}
