package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import systems.zlink.contracts.core.RoutingId;

public record ZLinkOwnerLease(
    String ownerId,
    RoutingId nodeRid,
    Instant leaseExpiresAt,
    Instant updatedAt) {
}
