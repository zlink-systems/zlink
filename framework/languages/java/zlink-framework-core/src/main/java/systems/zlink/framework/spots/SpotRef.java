package systems.zlink.framework.spots;

import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;

public record SpotRef(
    String spotId,
    long objectGeneration,
    String meshName,
    RoutingId nodeRid) {
    public SpotRef {
        systems.zlink.framework.runtime.internal.spots.ZLinkSpotIdValidator
            .requireValid(spotId);
        Objects.requireNonNull(nodeRid, "nodeRid");
        if (objectGeneration <= 0) {
            throw new IllegalArgumentException(
                "objectGeneration must be positive");
        }
        if (meshName == null || meshName.isBlank()) {
            throw new IllegalArgumentException("meshName must not be blank");
        }
    }
}
