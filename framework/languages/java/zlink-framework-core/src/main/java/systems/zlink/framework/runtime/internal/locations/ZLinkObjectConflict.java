package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkObjectConflict(ZLinkAuthorityReadResult current)
    implements ZLinkObjectReserveResult {
    public ZLinkObjectConflict {
        Objects.requireNonNull(current, "current");
    }
}
