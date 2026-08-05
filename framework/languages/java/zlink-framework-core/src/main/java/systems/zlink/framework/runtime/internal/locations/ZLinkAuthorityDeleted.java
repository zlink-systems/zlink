package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import java.util.Objects;

public record ZLinkAuthorityDeleted(
    String storeVersion,
    Instant storeNow)
    implements ZLinkAuthorityWriteResult {
    public ZLinkAuthorityDeleted {
        Objects.requireNonNull(storeVersion, "storeVersion");
        Objects.requireNonNull(storeNow, "storeNow");
    }
}
