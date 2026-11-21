package systems.zlink.framework.runtime.internal.locations;

import systems.zlink.contracts.core.RoutingId;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.UUID;
import java.util.zip.CRC32C;

public final class ZLinkCanonicalRelocationAuthorityStateCodecTestAccess {
    private ZLinkCanonicalRelocationAuthorityStateCodecTestAccess() {}

    public static byte[] publish(
            byte[] authorityPayload,
            ZLinkAggregateRelocationCoordinator.Request request,
            ZLinkAuthorityGenerationTransition transition) {
        return ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                authorityPayload, request, transition);
    }

    /**
     * Builds an authority payload carrying the record a coordinator leaves before publishing: the
     * source and coordinator are the same participant and no target is named yet. The record is
     * encoded by the production codec and spliced into the empty relocation slot that precedes the
     * empty activation-recovery slot at the end of an actor authority envelope.
     */
    public static byte[] withCapturedRecord(
            byte[] emptySlotAuthorityPayload,
            UUID relocationId,
            int phase,
            String coordinatorOwnerId,
            long coordinatorLeaseGeneration,
            RoutingId coordinatorNodeRid,
            long coordinatorNodeGeneration,
            String coordinatorExpectedStoreVersion) {
        byte[] state =
                ZLinkCanonicalRelocationAuthorityStateCodec.encodeState(
                        new ZLinkCanonicalRelocationAuthorityStateCodec.State(
                                relocationId.getMostSignificantBits(),
                                relocationId.getLeastSignificantBits(),
                                phase == 1 ? 0 : 1,
                                0,
                                "relocation-reference",
                                0,
                                coordinatorNodeRid,
                                coordinatorNodeGeneration,
                                coordinatorOwnerId,
                                coordinatorLeaseGeneration,
                                RoutingId.from(new byte[0]),
                                0,
                                "",
                                0,
                                coordinatorOwnerId,
                                coordinatorLeaseGeneration,
                                coordinatorNodeRid,
                                coordinatorNodeGeneration,
                                coordinatorExpectedStoreVersion,
                                phase,
                                0,
                                0));
        int envelopeHeader = 11;
        int slotOffset = emptySlotAuthorityPayload.length - 4 - 10;
        for (int i = slotOffset; i < slotOffset + 5; i++) {
            if (emptySlotAuthorityPayload[i] != 0) {
                throw new IllegalArgumentException(
                        "authority payload has no empty relocation slot");
            }
        }
        int bodyLength = emptySlotAuthorityPayload.length - envelopeHeader - 4 - 5 + state.length;
        ByteBuffer envelope =
                ByteBuffer.allocate(envelopeHeader + bodyLength + 4).order(ByteOrder.BIG_ENDIAN);
        envelope.put(emptySlotAuthorityPayload, 0, 7);
        envelope.putInt(bodyLength);
        envelope.put(emptySlotAuthorityPayload, envelopeHeader, slotOffset - envelopeHeader);
        envelope.put(state);
        envelope.put(
                emptySlotAuthorityPayload,
                slotOffset + 5,
                emptySlotAuthorityPayload.length - 4 - (slotOffset + 5));
        CRC32C checksum = new CRC32C();
        checksum.update(envelope.array(), 0, envelope.position());
        envelope.putInt((int) checksum.getValue());
        return envelope.array();
    }
}
