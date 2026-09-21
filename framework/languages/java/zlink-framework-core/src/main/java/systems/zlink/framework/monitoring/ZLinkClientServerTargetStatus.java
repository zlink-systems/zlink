package systems.zlink.framework.monitoring;

import systems.zlink.contracts.core.RoutingId;

import java.util.Objects;
import java.util.Optional;

public record ZLinkClientServerTargetStatus(
        RoutingId nodeRid,
        int weight,
        ZLinkPeerState state,
        Optional<ZLinkTopologyReason> unavailableReason) {
    public ZLinkClientServerTargetStatus {
        Objects.requireNonNull(nodeRid, "nodeRid");
        Objects.requireNonNull(state, "state");
        unavailableReason = unavailableReason == null ? Optional.empty() : unavailableReason;
    }
}
