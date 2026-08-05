package systems.zlink.framework.runtime.spots;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendSpot;
import systems.zlink.framework.spots.ZLinkEntrySpot;
import java.util.List;
record EntrySpotInitialization(
    RoutingId nodeRid,
    ZLinkBackendSpot backendSpot,
    List<Class<? extends ZLinkEntrySpot<?>>> entrySpots) {
}
