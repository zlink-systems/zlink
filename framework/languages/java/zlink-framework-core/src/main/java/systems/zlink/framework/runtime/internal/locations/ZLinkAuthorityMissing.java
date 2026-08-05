package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import java.util.Objects;

public record ZLinkAuthorityMissing(Instant storeNow)
    implements ZLinkAuthorityReadResult {
    public ZLinkAuthorityMissing {
        Objects.requireNonNull(storeNow, "storeNow");
    }
}
