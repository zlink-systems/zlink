package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import java.util.Objects;

public record ZLinkOwnerLeaseFound(
    ZLinkLocationOwnerToken token,
    Instant leaseExpiresAt,
    Instant storeNow)
    implements ZLinkOwnerLeaseReadResult {
    public ZLinkOwnerLeaseFound {
        Objects.requireNonNull(token, "token");
        Objects.requireNonNull(leaseExpiresAt, "leaseExpiresAt");
        Objects.requireNonNull(storeNow, "storeNow");
    }
}
