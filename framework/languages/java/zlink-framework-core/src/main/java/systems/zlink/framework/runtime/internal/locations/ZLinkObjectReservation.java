package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

public record ZLinkObjectReservation(
    String authorityKey,
    String storeVersion,
    long objectGeneration,
    long authorityOwnerGeneration,
    String reservationVersion,
    ZLinkMeshNodeDescriptorKey targetDescriptor,
    long targetDescriptorLifecycleGeneration,
    ZLinkLocationOwnerToken targetOwner) {
    public ZLinkObjectReservation {
        Objects.requireNonNull(authorityKey, "authorityKey");
        Objects.requireNonNull(storeVersion, "storeVersion");
        Objects.requireNonNull(reservationVersion, "reservationVersion");
        Objects.requireNonNull(targetDescriptor, "targetDescriptor");
        if (targetDescriptorLifecycleGeneration <= 0) {
            throw new IllegalArgumentException(
                "targetDescriptorLifecycleGeneration must be positive");
        }
        Objects.requireNonNull(targetOwner, "targetOwner");
    }
}
