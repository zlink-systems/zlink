package systems.zlink.framework.configuration;

import systems.zlink.contracts.core.RoutingId;

import java.util.Optional;

public record ZLinkMeshPeerConnection(String endpoint, Optional<RoutingId> expectedRoutingId) {}
