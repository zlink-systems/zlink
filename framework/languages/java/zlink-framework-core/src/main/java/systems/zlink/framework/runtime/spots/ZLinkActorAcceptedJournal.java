package systems.zlink.framework.runtime.spots;
import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceM6AWireCodec;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;
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
            DecodedApplication application = decodeApplication(
                decoded.packetName(),
                decoded.contentType(),
                decoded.payload(),
                decoded.replyRouteId(),
                decoded.metadata());
            return new Record(
                decoded.actorId(),
                application.header(),
                application.payload(),
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

    private static DecodedApplication decodeApplication(
        String packetName,
        String contentType,
        byte[] payload,
        Optional<Long> replyRouteId,
        Map<String, String> metadata) {
        boolean request = replyRouteId.isPresent();
        if (!ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME.equals(
                packetName)) {
            return new DecodedApplication(
                new ZLinkStreamHeader(
                    request
                        ? ZLinkStreamMessageKind.REQUEST
                        : ZLinkStreamMessageKind.SEND,
                    contentType.contains("json")
                        ? ZLinkStreamCodec.JSON
                        : ZLinkStreamCodec.RAW,
                    EnumSet.noneOf(
                        systems.zlink.framework.runtime.streams
                            .ZLinkStreamHeaderFlag.class),
                    replyRouteId,
                    packetName,
                    metadata),
                payload);
        }

        List<Message> parts = ZLinkServiceM6AWireCodec
            .decodeFrameworkMultipart(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    packetName, contentType, payload));
        try {
            if (parts.size() != 2) {
                throw new IllegalArgumentException(
                    "accepted Actor framework multipart must contain two parts");
            }
            ZLinkStreamHeader header = ZLinkStreamHeaderCodec.decodeOrPlain(
                parts.getFirst().toByteArray());
            if (request != (header.kind() == ZLinkStreamMessageKind.REQUEST)
                || header.kind() != ZLinkStreamMessageKind.REQUEST
                    && header.kind() != ZLinkStreamMessageKind.SEND) {
                throw new IllegalArgumentException(
                    "accepted Actor message kind does not match record kind");
            }
            return new DecodedApplication(
                header, parts.get(1).toByteArray());
        } finally {
            parts.forEach(Message::close);
        }
    }

    private record DecodedApplication(
        ZLinkStreamHeader header,
        byte[] payload) {
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
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        RoutingId sourceSessionRid,
        long sourceBindingGeneration,
        long sourceSessionSequence,
        long operationHigh,
        long operationLow,
        Optional<Long> replyRouteId,
        long objectGeneration) {
        Record {
            if (sourceNodeRid == null
                || sourceOwnerId == null || sourceOwnerId.isBlank()
                || sourceNodeGeneration == 0
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
