package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

import systems.zlink.contracts.core.RoutingId;

public record ZLinkLocationTopologyFilter(
    String meshName,
    RoutingId nodeRid,
    ZLinkLocationTopologyState state) {

    public static ZLinkLocationTopologyFilter all() {
        return new ZLinkLocationTopologyFilter(null, null, null);
    }
}
