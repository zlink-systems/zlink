package systems.zlink.framework.locations;

import java.util.Objects;

public record ZLinkSpotTypeCapacity(
    ZLinkPlacementObjectKind objectKind,
    String stableType,
    ZLinkCapacityUsage usage) {
    public ZLinkSpotTypeCapacity {
        Objects.requireNonNull(objectKind, "objectKind");
        if (objectKind == ZLinkPlacementObjectKind.ACTOR) {
            throw new IllegalArgumentException(
                "spot type capacity cannot describe Actor capacity");
        }
        Objects.requireNonNull(stableType, "stableType");
        Objects.requireNonNull(usage, "usage");
    }
}
