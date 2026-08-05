package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import java.util.UUID;

public record ZLinkRelocationCapacityReservationRequest(
    UUID reservationId,
    String authorityKey,
    String expectedStoreVersion,
    ZLinkPlacementObjectKind objectKind,
    String stableType,
    ZLinkMeshNodeDescriptorKey sourceDescriptor,
    long sourceDescriptorLifecycleGeneration,
    ZLinkLocationOwnerToken sourceOwner,
    ZLinkMeshNodeDescriptorKey targetDescriptor,
    long targetDescriptorLifecycleGeneration,
    ZLinkLocationOwnerToken targetOwner,
    ZLinkPlacementCapacityBundle capacityBundle) {
    public ZLinkRelocationCapacityReservationRequest {
        Objects.requireNonNull(reservationId, "reservationId");
        Objects.requireNonNull(authorityKey, "authorityKey");
        Objects.requireNonNull(expectedStoreVersion, "expectedStoreVersion");
        Objects.requireNonNull(objectKind, "objectKind");
        Objects.requireNonNull(stableType, "stableType");
        if (stableType.isBlank()) {
            throw new IllegalArgumentException(
                "stableType must not be blank");
        }
        Objects.requireNonNull(sourceDescriptor, "sourceDescriptor");
        if (sourceDescriptorLifecycleGeneration <= 0) {
            throw new IllegalArgumentException(
                "sourceDescriptorLifecycleGeneration must be positive");
        }
        Objects.requireNonNull(sourceOwner, "sourceOwner");
        Objects.requireNonNull(targetDescriptor, "targetDescriptor");
        if (targetDescriptorLifecycleGeneration <= 0) {
            throw new IllegalArgumentException(
                "targetDescriptorLifecycleGeneration must be positive");
        }
        Objects.requireNonNull(targetOwner, "targetOwner");
        if (reservationId.getMostSignificantBits() == 0L
            && reservationId.getLeastSignificantBits() == 0L) {
            throw new IllegalArgumentException(
                "reservationId must not be zero");
        }
        Objects.requireNonNull(capacityBundle, "capacityBundle");
        if (capacityBundle.actorSlots() == 0
            && capacityBundle.spotSlots() == 0) {
            throw new IllegalArgumentException(
                "relocation capacity reservation must reserve at least one slot");
        }
    }
}
