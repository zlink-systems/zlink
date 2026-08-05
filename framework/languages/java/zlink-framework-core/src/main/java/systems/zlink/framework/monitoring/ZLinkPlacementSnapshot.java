package systems.zlink.framework.monitoring;

import java.util.Optional;

public record ZLinkPlacementSnapshot(
    boolean isAvailable,
    int activeActorCount,
    int activeSpotCount,
    Optional<ZLinkTopologyReason> unavailableReason) {
}
