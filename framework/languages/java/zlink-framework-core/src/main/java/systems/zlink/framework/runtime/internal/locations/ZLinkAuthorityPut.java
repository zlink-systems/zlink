package systems.zlink.framework.runtime.internal.locations;

import java.util.Objects;
import java.util.Optional;

public record ZLinkAuthorityPut(
    byte[] payload,
    ZLinkAuthorityGenerationTransition generationTransition,
    Optional<ZLinkLocationOwnerToken> targetOwner,
    Optional<ZLinkRelocationCapacityFence> relocationCapacityFence)
    implements ZLinkAuthorityMutation {
    public ZLinkAuthorityPut {
        payload = Objects.requireNonNull(payload, "payload").clone();
        Objects.requireNonNull(generationTransition, "generationTransition");
        targetOwner = Objects.requireNonNull(targetOwner, "targetOwner");
        relocationCapacityFence = Objects.requireNonNull(
            relocationCapacityFence,
            "relocationCapacityFence");
        boolean targetRequired =
            generationTransition == ZLinkAuthorityGenerationTransition.NEW_OWNER;
        if (targetRequired != targetOwner.isPresent()) {
            throw new IllegalArgumentException(
                targetRequired
                    ? "NEW_OWNER requires targetOwner"
                    : "PRESERVE must not include targetOwner");
        }
        boolean capacityRequired =
            generationTransition == ZLinkAuthorityGenerationTransition.NEW_OWNER;
        if (capacityRequired != relocationCapacityFence.isPresent()) {
            throw new IllegalArgumentException(
                capacityRequired
                    ? "NEW_OWNER requires relocationCapacityFence"
                    : "PRESERVE must not include relocationCapacityFence");
        }
    }

    @Override
    public byte[] payload() {
        return payload.clone();
    }
}
