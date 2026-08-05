package systems.zlink.framework.configuration;

import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;

public record ZLinkMeshPeerConnection(
    String endpoint,
    Optional<RoutingId> expectedRoutingId) {
}
