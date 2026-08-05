package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkAuthorityConflict(ZLinkAuthorityReadResult current)
    implements ZLinkAuthorityWriteResult {
    public ZLinkAuthorityConflict {
        Objects.requireNonNull(current, "current");
    }
}
