package systems.zlink.framework.spots;

import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;

record FrameworkSpotHandle(
    String meshName,
    String spotId,
    RoutingId ownerNodeRid,
    long spotGeneration) implements SpotHandle {
    FrameworkSpotHandle {
        Objects.requireNonNull(meshName, "meshName");
        Objects.requireNonNull(spotId, "spotId");
        Objects.requireNonNull(ownerNodeRid, "ownerNodeRid");
        if (spotGeneration <= 0) {
            throw new IllegalArgumentException("spotGeneration must be positive");
        }
    }
}
