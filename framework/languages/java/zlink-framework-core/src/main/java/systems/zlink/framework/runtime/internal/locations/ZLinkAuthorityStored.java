package systems.zlink.framework.runtime.internal.locations;

import java.time.Instant;
import java.util.Objects;

public record ZLinkAuthorityStored(
    String storeVersion,
    byte[] payload,
    long objectGeneration,
    long authorityOwnerGeneration,
    String ownerId,
    long ownerLeaseGeneration,
    ZLinkPlacementAllocation allocation,
    Instant storeNow)
    implements ZLinkAuthorityWriteResult {
    public ZLinkAuthorityStored {
        Objects.requireNonNull(storeVersion, "storeVersion");
        payload = Objects.requireNonNull(payload, "payload").clone();
        Objects.requireNonNull(ownerId, "ownerId");
        Objects.requireNonNull(allocation, "allocation");
        Objects.requireNonNull(storeNow, "storeNow");
    }

    @Override
    public byte[] payload() {
        return payload.clone();
    }
}
