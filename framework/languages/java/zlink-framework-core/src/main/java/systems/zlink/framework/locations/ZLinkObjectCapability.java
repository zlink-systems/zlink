package systems.zlink.framework.locations;

import systems.zlink.framework.runtime.internal.locations.*;

import java.nio.charset.StandardCharsets;
import java.util.Objects;

public record ZLinkObjectCapability(
        ZLinkPlacementObjectKind objectKind,
        String stableType,
        ZLinkObjectMaintenancePolicyKind policy,
        boolean hasSnapshotAdapter,
        int spotLimit,
        boolean entryAccepted) {
    public ZLinkObjectCapability {
        Objects.requireNonNull(objectKind, "objectKind");
        Objects.requireNonNull(stableType, "stableType");
        requireBoundedText(stableType, "stableType");
        Objects.requireNonNull(policy, "policy");
        if ((policy == ZLinkObjectMaintenancePolicyKind.SNAPSHOT) != hasSnapshotAdapter) {
            throw new IllegalArgumentException("Snapshot policy and adapter presence must match");
        }
        if (spotLimit < 0 || objectKind == ZLinkPlacementObjectKind.ACTOR && spotLimit != 0) {
            throw new IllegalArgumentException("spotLimit must be non-negative and zero for Actor");
        }
        if (entryAccepted && objectKind != ZLinkPlacementObjectKind.ACTOR) {
            throw new IllegalArgumentException("entryAccepted is only valid for Actor");
        }
    }

    /** A capability whose node registers no Entry Spot type that accepts it. */
    public ZLinkObjectCapability(
            ZLinkPlacementObjectKind objectKind,
            String stableType,
            ZLinkObjectMaintenancePolicyKind policy,
            boolean hasSnapshotAdapter,
            int spotLimit) {
        this(objectKind, stableType, policy, hasSnapshotAdapter, spotLimit, false);
    }

    private static void requireBoundedText(String value, String field) {
        Objects.requireNonNull(value, field);
        int size = value.getBytes(StandardCharsets.UTF_8).length;
        if (value.isBlank() || value.indexOf('\0') >= 0 || size > 255) {
            throw new IllegalArgumentException(field + " must be 1..255 UTF-8 bytes without NUL");
        }
    }
}
