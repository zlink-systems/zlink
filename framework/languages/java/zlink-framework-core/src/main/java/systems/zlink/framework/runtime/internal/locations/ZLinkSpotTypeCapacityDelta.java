package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.locations.ZLinkPlacementObjectKind;

import java.util.Objects;

public record ZLinkSpotTypeCapacityDelta(
        ZLinkPlacementObjectKind objectKind, String stableType, int slots) {
    public ZLinkSpotTypeCapacityDelta {
        Objects.requireNonNull(objectKind, "objectKind");
        Objects.requireNonNull(stableType, "stableType");
        if (objectKind == ZLinkPlacementObjectKind.ACTOR) {
            throw new IllegalArgumentException("spot type capacity cannot describe Actor capacity");
        }
        if (stableType.isBlank()) {
            throw new IllegalArgumentException("stableType must not be blank");
        }
        if (slots <= 0) {
            throw new IllegalArgumentException("slots must be in 1..Integer.MAX_VALUE");
        }
    }
}
