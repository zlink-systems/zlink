package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;

import java.util.Objects;

public record ZLinkMeshNodeDescriptorKey(String meshName, RoutingId rid) {
    public ZLinkMeshNodeDescriptorKey {
        Objects.requireNonNull(meshName, "meshName");
        Objects.requireNonNull(rid, "rid");
    }
}
