package systems.zlink.framework.runtime.internal.locations;

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
        pendingCreation = Objects.requireNonNull(
            pendingCreation, "pendingCreation");
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

    @Override
    public byte[] payload() {
        return payload.clone();
    }
}
