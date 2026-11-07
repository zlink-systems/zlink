package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;

import java.time.Instant;
import java.util.Objects;
import java.util.Optional;

public record ZLinkAuthoritySnapshot(
        String storeVersion,
        byte[] payload,
        long objectGeneration,
        long authorityOwnerGeneration,
        String ownerId,
        long ownerLeaseGeneration,
        ZLinkPlacementAllocation allocation,
        Optional<ZLinkPendingObjectCreation> pendingCreation,
        Instant storeNow)
        implements ZLinkAuthorityReadResult {
    public ZLinkAuthoritySnapshot {
        Objects.requireNonNull(storeVersion, "storeVersion");
        payload = Objects.requireNonNull(payload, "payload").clone();
        Objects.requireNonNull(ownerId, "ownerId");
        Objects.requireNonNull(allocation, "allocation");
        pendingCreation = Objects.requireNonNull(pendingCreation, "pendingCreation");
        Objects.requireNonNull(storeNow, "storeNow");
    }

    public ZLinkAuthoritySnapshot(
            String storeVersion,
            byte[] payload,
            long objectGeneration,
            long authorityOwnerGeneration,
            String ownerId,
            long ownerLeaseGeneration,
            ZLinkPlacementAllocation allocation,
            Instant storeNow) {
        this(
                storeVersion,
                payload,
                objectGeneration,
                authorityOwnerGeneration,
                ownerId,
                ownerLeaseGeneration,
                allocation,
                Optional.empty(),
                storeNow);
    }

    /** Identity of an incarnation; a later lease token and StoreVersion do not replace it. */
    public boolean sameIncarnation(ZLinkAuthoritySnapshot other) {
        return ownerId.equals(other.ownerId)
                && objectGeneration == other.objectGeneration
                && authorityOwnerGeneration == other.authorityOwnerGeneration
                && allocation.descriptor().equals(other.allocation.descriptor())
                && allocation.descriptorLifecycleGeneration()
                        == other.allocation.descriptorLifecycleGeneration();
    }

    public boolean steadyReady() {
        return pendingCreation.isEmpty() && steadyReady(payload, allocation);
    }

    public static boolean steadyReady(byte[] payload, ZLinkPlacementAllocation allocation) {
        if (allocation.state() != ZLinkPlacementAllocationState.ACTIVE
                || ZLinkCanonicalRelocationAuthorityStateCodec.decode(payload) != null)
            return false;
        if (allocation.objectKind() == ZLinkPlacementObjectKind.ACTOR) {
            return new ZLinkActorAuthorityPayloadCodec()
                    .decode(payload)
                    .filter(actor -> actor.state() == ZLinkActorAuthorityPayloadCodec.State.READY)
                    .isPresent();
        }
        return new ZLinkServiceAuthorityPayloadCodec()
                .decode(payload)
                .filter(
                        spot ->
                                spot.state() == ZLinkServiceAuthorityPayloadCodec.State.READY
                                        && spot.activationRecoveryState().isEmpty())
                .isPresent();
    }

    @Override
    public byte[] payload() {
        return payload.clone();
    }
}
