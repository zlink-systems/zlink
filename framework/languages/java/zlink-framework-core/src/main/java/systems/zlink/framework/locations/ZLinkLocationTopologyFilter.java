package systems.zlink.framework.locations;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.locations.*;

public record ZLinkLocationTopologyFilter(
        String meshName, RoutingId nodeRid, ZLinkLocationTopologyState state) {

    public static ZLinkLocationTopologyFilter all() {
        return new ZLinkLocationTopologyFilter(null, null, null);
    }
}
