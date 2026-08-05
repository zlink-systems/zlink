package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;

public record ZLinkPlacementAllocation(
    ZLinkPlacementAllocationState state,
    ZLinkPlacementObjectKind objectKind,
    String stableType,
    ZLinkMeshNodeDescriptorKey descriptor,
    long descriptorLifecycleGeneration,
    ZLinkPlacementCapacityBundle capacityBundle) {
    public ZLinkPlacementAllocation {
        Objects.requireNonNull(state, "state");
        Objects.requireNonNull(objectKind, "objectKind");
        Objects.requireNonNull(stableType, "stableType");
        Objects.requireNonNull(descriptor, "descriptor");
        if (stableType.isBlank()) {
            throw new IllegalArgumentException(
                "stableType must not be blank");
        }
        if (descriptorLifecycleGeneration <= 0) {
            throw new IllegalArgumentException(
                "descriptorLifecycleGeneration must be positive");
        }
        Objects.requireNonNull(capacityBundle, "capacityBundle");
    }

}
