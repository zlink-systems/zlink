package systems.zlink.framework.monitoring;

import java.util.Objects;
import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;

public record ZLinkClientServerTargetStatus(
    RoutingId nodeRid,
    int weight,
    ZLinkPeerState state,
    Optional<ZLinkTopologyReason> unavailableReason) {
    public ZLinkClientServerTargetStatus {
        Objects.requireNonNull(nodeRid, "nodeRid");
        Objects.requireNonNull(state, "state");
        unavailableReason =
            unavailableReason == null ? Optional.empty() : unavailableReason;
    }
}
