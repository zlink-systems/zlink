package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.internal.configuration.ZLinkObjectFactoryRegistration.RelocationPolicy;

import java.util.Objects;

public record ZLinkObjectReservationRequest(
        ZLinkPlacementObjectKind objectKind,
        String authorityKey,
        String stableType,
        String creationIntentReference,
        byte[] creationIntentHash,
        int creationIntentEncodedSize,
        ZLinkMeshNodeDescriptorKey targetDescriptor,
        long targetDescriptorLifecycleGeneration,
        ZLinkLocationOwnerToken targetOwner,
        byte[] creatingPayload,
        ZLinkPlacementCapacityBundle capacityBundle,
        RelocationPolicy actorRelocationPolicy) {
    public ZLinkObjectReservationRequest(
            ZLinkPlacementObjectKind objectKind,
            String authorityKey,
            String stableType,
            String creationIntentReference,
            byte[] creationIntentHash,
            int creationIntentEncodedSize,
            ZLinkMeshNodeDescriptorKey targetDescriptor,
            long targetDescriptorLifecycleGeneration,
            ZLinkLocationOwnerToken targetOwner,
            byte[] creatingPayload,
            ZLinkPlacementCapacityBundle capacityBundle) {
        this(
                objectKind,
                authorityKey,
                stableType,
                creationIntentReference,
                creationIntentHash,
                creationIntentEncodedSize,
                targetDescriptor,
                targetDescriptorLifecycleGeneration,
                targetOwner,
                creatingPayload,
                capacityBundle,
                null);
    }

    public ZLinkObjectReservationRequest {
        Objects.requireNonNull(objectKind, "objectKind");
        Objects.requireNonNull(authorityKey, "authorityKey");
        Objects.requireNonNull(stableType, "stableType");
        if (authorityKey.isBlank()) {
            throw new IllegalArgumentException("authorityKey must not be blank");
        }
        if (stableType.isBlank()) {
            throw new IllegalArgumentException("stableType must not be blank");
        }
        Objects.requireNonNull(creationIntentReference, "creationIntentReference");
        creationIntentHash =
                Objects.requireNonNull(creationIntentHash, "creationIntentHash").clone();
        Objects.requireNonNull(targetDescriptor, "targetDescriptor");
        if (targetDescriptorLifecycleGeneration == 0) {
            throw new IllegalArgumentException(
                    "targetDescriptorLifecycleGeneration must be positive");
        }
        Objects.requireNonNull(targetOwner, "targetOwner");
        creatingPayload = Objects.requireNonNull(creatingPayload, "creatingPayload").clone();
        Objects.requireNonNull(capacityBundle, "capacityBundle");
        if (capacityBundle.actorSlots() == 0 && capacityBundle.spotSlots() == 0) {
            throw new IllegalArgumentException("object reservation must reserve at least one slot");
        }
    }

    @Override
    public byte[] creationIntentHash() {
        return creationIntentHash.clone();
    }

    @Override
    public byte[] creatingPayload() {
        return creatingPayload.clone();
    }
}
