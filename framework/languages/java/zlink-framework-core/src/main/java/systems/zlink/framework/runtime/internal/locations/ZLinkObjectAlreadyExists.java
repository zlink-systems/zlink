package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkObjectAlreadyExists(ZLinkAuthoritySnapshot current)
    implements ZLinkObjectReserveResult {
    public ZLinkObjectAlreadyExists {
        Objects.requireNonNull(current, "current");
    }
}
