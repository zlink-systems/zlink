package systems.zlink.framework.monitoring;

import systems.zlink.contracts.core.RoutingId;

import java.util.Optional;

public record ZLinkMeshPeerSnapshot(
        RoutingId nodeRid, ZLinkPeerState state, Optional<ZLinkTopologyReason> unavailableReason) {}
