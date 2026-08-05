package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import java.util.Objects;

public record ZLinkOwnerLeaseRenewed(
    Instant leaseExpiresAt,
    Instant storeNow)
    implements ZLinkOwnerLeaseRenewResult {
    public ZLinkOwnerLeaseRenewed {
        Objects.requireNonNull(leaseExpiresAt, "leaseExpiresAt");
        Objects.requireNonNull(storeNow, "storeNow");
    }
}
