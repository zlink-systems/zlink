package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.framework.runtime.locations.ZLinkActorAuthorityPayloadCodec;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;

import java.util.Objects;

public record ZLinkAuthorityPut(
        byte[] payload, ZLinkAuthoritySnapshot preserved, ZLinkLocationOwnerToken replacementOwner)
        implements ZLinkAuthorityMutation {
    public ZLinkAuthorityPut(byte[] payload) {
        this(payload, null, null);
    }

    /** Location §5.1 recovery Preserve, using the confirmed pre-outage snapshot. */
    public static ZLinkAuthorityPut preserve(
            ZLinkAuthoritySnapshot preserved, ZLinkLocationOwnerToken replacementOwner) {
        Objects.requireNonNull(preserved, "preserved");
        Objects.requireNonNull(replacementOwner, "replacementOwner");
        byte[] payload = preserved.payload();
        var actorCodec = new ZLinkActorAuthorityPayloadCodec();
        var actor = actorCodec.decode(payload);
        var spotCodec = new ZLinkServiceAuthorityPayloadCodec();
        var spot = spotCodec.decode(payload);
        if (actor.isPresent()) {
            var a = actor.orElseThrow();
            payload =
                    actorCodec.encode(
                            a.state(),
                            a.stableType(),
                            a.actorId(),
                            a.currentSpotId(),
                            a.currentSpotGeneration(),
                            a.currentSpotKind(),
                            replacementOwner.ownerId(),
                            replacementOwner.leaseGeneration(),
                            a.meshName(),
                            a.nodeRid(),
                            a.nodeGeneration());
        } else if (spot.isPresent()) {
            var a = spot.orElseThrow();
            payload =
                    a.user().isPresent()
                            ? spotCodec.encodeUser(
                                    a.state(),
                                    a.stableType(),
                                    a.spotId(),
                                    replacementOwner.ownerId(),
                                    replacementOwner.leaseGeneration(),
                                    a.meshName(),
                                    a.nodeRid(),
                                    a.nodeGeneration())
                            : spotCodec.encodeInstance(
                                    a.state(),
                                    a.stableType(),
                                    a.spotId(),
                                    replacementOwner.ownerId(),
                                    replacementOwner.leaseGeneration(),
                                    a.meshName(),
                                    a.nodeRid(),
                                    a.nodeGeneration(),
                                    a.activationRecoveryState());
        }
        return new ZLinkAuthorityPut(payload, preserved, replacementOwner);
    }

    public ZLinkAuthorityPut {
        payload = Objects.requireNonNull(payload, "payload").clone();
        if ((preserved == null) != (replacementOwner == null)) {
            throw new IllegalArgumentException("Recovery Preserve requires snapshot and token");
        }
    }

    /** Compares the immutable recovery input; the repository decides transition eligibility. */
    public boolean matchesPreserved(ZLinkAuthoritySnapshot current) {
        return preserved != null
                && preserved.pendingCreation().equals(current.pendingCreation())
                && preserved.sameIncarnation(current)
                && preserved.storeVersion().equals(current.storeVersion())
                && preserved.ownerLeaseGeneration() == current.ownerLeaseGeneration()
                && preserved.allocation().equals(current.allocation())
                && java.util.Arrays.equals(preserved.payload(), current.payload())
                && java.util.Arrays.equals(
                        payload, preserve(preserved, replacementOwner).payload());
    }

    @Override
    public byte[] payload() {
        return payload.clone();
    }
}
