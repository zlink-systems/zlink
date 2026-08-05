package systems.zlink.framework.monitoring;

import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;

public record ZLinkMeshPeerSnapshot(
    RoutingId nodeRid,
    ZLinkPeerState state,
    Optional<ZLinkTopologyReason> unavailableReason) {
}
