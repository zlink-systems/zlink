package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;
import systems.zlink.contracts.core.RoutingId;

public record ZLinkMeshNodeDescriptorKey(
    String meshName,
    RoutingId rid) {
    public ZLinkMeshNodeDescriptorKey {
        Objects.requireNonNull(meshName, "meshName");
        Objects.requireNonNull(rid, "rid");
    }
}
