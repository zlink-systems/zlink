package systems.zlink.framework.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.*;

import java.time.Instant;

public record ZLinkLocationTopologyEntry(
        String meshName,
        RoutingId nodeRid,
        String endpoint,
        boolean draining,
        ZLinkLocationTopologyState state,
        Instant updatedAt) {}
