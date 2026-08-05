package systems.zlink.framework.runtime.internal.backend;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.spots.ZLinkSpotKind;

public record ZLinkBackendSpotRoute(
    RoutingId nodeRid,
    String spotId,
    ZLinkSpotKind spotKind) {
}
