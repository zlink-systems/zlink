package systems.zlink.framework.locations;

import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;

public record ZLinkLocationObjectEntry(
    String globalId,
    long objectGeneration,
    String meshName,
    RoutingId nodeRid,
    ZLinkLocationObjectState state,
    String stableType) {
    public ZLinkLocationObjectEntry {
        if (globalId == null || globalId.isBlank())
            throw new IllegalArgumentException("globalId must not be blank");
        if (objectGeneration <= 0)
            throw new IllegalArgumentException("objectGeneration must be positive");
        Objects.requireNonNull(meshName, "meshName");
        Objects.requireNonNull(nodeRid, "nodeRid");
        Objects.requireNonNull(state, "state");
        if (stableType == null || stableType.isBlank())
            throw new IllegalArgumentException("stableType must not be blank");
    }
}
