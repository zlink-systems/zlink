package systems.zlink.framework.runtime.spots;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;

final class ZLinkActorAcceptedJournal {
    private ZLinkActorAcceptedJournal() {
    }

    static byte[] encode(
        String actorId,
        ZLinkStreamHeader header,
        Message payload,
        byte[] canonical) {
        if (canonical == null || canonical.length == 0) {
            throw new IllegalStateException(
                "canonical accepted Actor journal record is unavailable");
        }
        return canonical.clone();
    }

    static Record decode(byte[] encoded) {
        if (systems.zlink.framework.runtime.internal.service
            .ZLinkServiceFrozenRecordCodec.isCanonical(encoded)) {
            var decoded = systems.zlink.framework.runtime.internal.service
                .ZLinkServiceFrozenRecordCodec.decodeActor(encoded);
            return new Record(
                decoded.actorId(),
                new ZLinkStreamHeader(
                    decoded.replyRouteId().isPresent()
                        ? systems.zlink.framework.streams
                            .ZLinkStreamMessageKind.REQUEST
                        : systems.zlink.framework.streams
                            .ZLinkStreamMessageKind.SEND,
                    decoded.contentType().contains("json")
                        ? systems.zlink.framework.streams.ZLinkStreamCodec.JSON
                        : systems.zlink.framework.streams.ZLinkStreamCodec.RAW,
                    java.util.EnumSet.noneOf(
                        systems.zlink.framework.runtime.streams
                            .ZLinkStreamHeaderFlag.class),
                    decoded.replyRouteId(),
                    decoded.packetName(),
                    decoded.metadata()),
                decoded.payload(),
                decoded.sourceNodeRid(),
                decoded.sourceNodeGeneration(),
                decoded.sourceOwnerId(),
                decoded.sourceOwnerLeaseGeneration(),
                decoded.sourceSessionRid(),
                decoded.sourceBindingGeneration(),
                decoded.sourceSessionSequence(),
                decoded.operationHigh(),
                decoded.operationLow(),
                decoded.replyRouteId(),
                decoded.objectGeneration());
        }
        throw new IllegalArgumentException(
            "accepted Actor journal record is not canonical service-wire-v1");
    }

    private static void write(
        DataOutputStream output,
        byte[] value) throws IOException {
        output.writeInt(value.length);
        output.write(value);
    }

    private static byte[] read(DataInputStream input) throws IOException {
        int length = input.readInt();
        if (length < 0 || length > 64 * 1024 * 1024) {
            throw new IllegalArgumentException(
                "invalid accepted Actor journal byte length");
        }
        byte[] value = input.readNBytes(length);
        if (value.length != length) {
            throw new IllegalArgumentException(
                "truncated accepted Actor journal record");
        }
        return value;
    }

    record Record(
        String actorId,
        ZLinkStreamHeader header,
        byte[] payload,
        systems.zlink.contracts.core.RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        systems.zlink.contracts.core.RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        long operationHigh,
        long operationLow,
        java.util.Optional<Long> replyRouteId,
        long objectGeneration) {
        Record {
            if (sourceNodeRid == null
                || sourceOwnerId == null || sourceOwnerId.isBlank()
                || sourceNodeGeneration <= 0
                || sourceOwnerLeaseGeneration <= 0
                || sourceSessionRid == null
                    != (sourceBindingGeneration == 0)
                || sourceSessionRid == null
                    != (sourceSessionSequence == 0)
                || objectGeneration <= 0
                || operationHigh == 0 && operationLow == 0) {
                throw new IllegalArgumentException(
                    "accepted Actor source fence is invalid");
            }
            payload = payload.clone();
        }

        @Override
        public byte[] payload() {
            return payload.clone();
        }
    }
}
