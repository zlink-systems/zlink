package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;

/**
 * Restores a steady opaque payload after matching the exact store version and
 * owner token. The owner's lease does not need to remain live.
 */
public record ZLinkAuthorityRestore(
    byte[] payload,
    ZLinkLocationOwnerToken expectedOwner)
    implements ZLinkAuthorityMutation {
    public ZLinkAuthorityRestore {
        payload = Objects.requireNonNull(payload, "payload").clone();
        Objects.requireNonNull(expectedOwner, "expectedOwner");
    }

    @Override
    public byte[] payload() {
        return payload.clone();
    }
}
