package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;

public record ZLinkOwnerLeaseRenewal(
    Instant leaseExpiresAt,
    Instant storeNow) {
}
