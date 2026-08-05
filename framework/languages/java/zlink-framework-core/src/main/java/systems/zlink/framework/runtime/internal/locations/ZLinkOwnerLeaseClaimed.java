package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import java.util.Objects;

public record ZLinkOwnerLeaseClaimed(
    ZLinkLocationOwnerToken token,
    Instant leaseExpiresAt,
    Instant storeNow)
    implements ZLinkOwnerLeaseClaimResult {
    public ZLinkOwnerLeaseClaimed {
        Objects.requireNonNull(token, "token");
        Objects.requireNonNull(leaseExpiresAt, "leaseExpiresAt");
        Objects.requireNonNull(storeNow, "storeNow");
    }
}
