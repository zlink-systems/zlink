package systems.zlink.framework.locations;

import java.util.Objects;

public record ZLinkLocationObjectFilter(
    ZLinkPlacementObjectKind objectKind,
    String stableType,
    String meshName) {
    public ZLinkLocationObjectFilter {
        Objects.requireNonNull(objectKind, "objectKind");
        if (stableType != null && stableType.isBlank())
            throw new IllegalArgumentException("stableType must not be blank");
        if (meshName != null && meshName.isBlank())
            throw new IllegalArgumentException("meshName must not be blank");
    }

    public static ZLinkLocationObjectFilter of(ZLinkPlacementObjectKind objectKind) {
        return new ZLinkLocationObjectFilter(objectKind, null, null);
    }
}
