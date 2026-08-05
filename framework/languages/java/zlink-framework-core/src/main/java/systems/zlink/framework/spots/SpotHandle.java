package systems.zlink.framework.spots;

import systems.zlink.contracts.core.RoutingId;

public sealed interface SpotHandle permits FrameworkSpotHandle {
    String meshName();

    String spotId();
}
