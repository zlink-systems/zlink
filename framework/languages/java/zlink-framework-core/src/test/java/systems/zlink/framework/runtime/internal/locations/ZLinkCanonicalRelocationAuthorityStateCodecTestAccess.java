package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;

import java.util.UUID;

public final class ZLinkCanonicalRelocationAuthorityStateCodecTestAccess {
    private ZLinkCanonicalRelocationAuthorityStateCodecTestAccess() {}

    public static byte[] publish(
            byte[] authorityPayload,
            ZLinkAggregateRelocationCoordinator.Request request,
            ZLinkAuthorityGenerationTransition transition) {
        return ZLinkLegacyRelocationAuthorityFixture.publish(authorityPayload, request, transition);
    }

    public static ZLinkAuthorityPut sourcePhaseMutation(
            ZLinkAuthoritySnapshot snapshot,
            UUID relocationId,
            String phase,
            long applicationVersion,
            String expectedStoreVersion,
            boolean legacy) {
        if (!legacy) {
            var record =
                    ZLinkAuthorityRelocation.preparing(
                                    snapshot,
                                    snapshot,
                                    relocationId,
                                    "0".repeat(64),
                                    applicationVersion)
                            .json();
            record.put("phase", phase);
            record.withObjectProperty("coordinator")
                    .put("expectedStoreVersion", expectedStoreVersion);
            return ZLinkAuthorityPut.relocation(
                    snapshot.payload(), new ZLinkAuthorityRelocation(record));
        }
        var allocation = snapshot.allocation();
        var nodeRid = allocation.descriptor().rid();
        long nodeGeneration = allocation.descriptorLifecycleGeneration();
        boolean preparing = phase.equals("preparing");
        byte[] state =
                ZLinkLegacyRelocationAuthorityFixture.encodeState(
                        new ZLinkCanonicalRelocationAuthorityStateCodec.State(
                                relocationId.getMostSignificantBits(),
                                relocationId.getLeastSignificantBits(),
                                preparing ? 0 : 1,
                                0,
                                "relocation-reference",
                                0,
                                nodeRid,
                                nodeGeneration,
                                snapshot.ownerId(),
                                snapshot.ownerLeaseGeneration(),
                                RoutingId.from(new byte[0]),
                                0,
                                "",
                                0,
                                snapshot.ownerId(),
                                snapshot.ownerLeaseGeneration(),
                                nodeRid,
                                nodeGeneration,
                                expectedStoreVersion,
                                preparing ? 1 : 2,
                                applicationVersion,
                                0));
        return new ZLinkAuthorityPut(
                ZLinkLegacyRelocationAuthorityFixture.withRelocationSlot(
                        snapshot.payload(), state));
    }
}
