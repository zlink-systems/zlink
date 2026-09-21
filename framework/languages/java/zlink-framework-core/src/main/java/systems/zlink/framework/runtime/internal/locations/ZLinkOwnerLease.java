package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;

import java.time.Instant;

public record ZLinkOwnerLease(
        String ownerId, RoutingId nodeRid, Instant leaseExpiresAt, Instant updatedAt) {}
