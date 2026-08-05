package systems.zlink.framework.runtime.internal.service;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Optional;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

/** Canonical service-wire-v1 frozen Spot and Actor operation encoder. */
public final class ZLinkServiceFrozenRecordCodec {
    private static final int RECORD_SPOT_SEND = 5;
    private static final int RECORD_SPOT_REQUEST = 6;
    private static final int RECORD_ACTOR_SEND = 9;
    private static final int RECORD_ACTOR_REQUEST = 10;
    private static final int SOURCE_NODE = 1;
    private static final int SOURCE_SPOT = 2;
    private static final int SOURCE_ACTOR = 3;
    private static final int SOURCE_BOUND_SESSION = 4;
    private static final int OPERATION_NONE = 0;
    private static final int OPERATION_SPOT_REQUEST = 3;
    private static final int OPERATION_ACTOR_REQUEST = 4;

    private ZLinkServiceFrozenRecordCodec() {
    }

    public static boolean isCanonical(byte[] encoded) {
        return encoded != null
            && encoded.length != 0
            && (encoded[0] == RECORD_SPOT_SEND
                || encoded[0] == RECORD_SPOT_REQUEST
                || encoded[0] == RECORD_ACTOR_SEND
                || encoded[0] == RECORD_ACTOR_REQUEST);
    }

    public static DecodedSpot decodeSpot(byte[] encoded) {
        Reader reader = new Reader(encoded);
        int kind = reader.u8();
        if (kind != RECORD_SPOT_SEND && kind != RECORD_SPOT_REQUEST) {
            throw new IllegalArgumentException(
                "frozen record is not a Spot operation");
        }
        Source source = reader.source();
        Metadata metadata = reader.metadata();
        OperationId operation = reader.operationId();
        int operationKind = reader.u32();
        Optional<Long> replyRoute = reader.replyRoute();
        if ((kind == RECORD_SPOT_REQUEST)
            != (operationKind == OPERATION_SPOT_REQUEST
                && replyRoute.isPresent())) {
            throw new IllegalArgumentException(
                "frozen Spot operation kind does not match record kind");
        }
        String targetSpotId = reader.text8();
        long objectGeneration = reader.nonzeroU64();
        reader.rid();
        reader.nonzeroU64();
        reader.nonzeroU64();
        reader.nonzeroU64();
        ApplicationPayload payload = reader.applicationPayload();
        reader.end();
        return new DecodedSpot(
            source.nodeRid(),
            source.nodeGeneration(),
            source.ownerId(),
            source.ownerLeaseGeneration(),
            source.spotId(),
            operation.high(),
            operation.low(),
            replyRoute,
            metadata.encoded(),
            payload.packetName(),
            payload.payload(),
            targetSpotId,
            objectGeneration);
    }

    public static DecodedActor decodeActor(byte[] encoded) {
        Reader reader = new Reader(encoded);
        int kind = reader.u8();
        if (kind != RECORD_ACTOR_SEND && kind != RECORD_ACTOR_REQUEST) {
            throw new IllegalArgumentException(
                "frozen record is not an Actor operation");
        }
        Source source = reader.source();
        Metadata metadata = reader.metadata();
        OperationId operation = reader.operationId();
        int operationKind = reader.u32();
        Optional<Long> replyRoute = reader.replyRoute();
        if ((kind == RECORD_ACTOR_REQUEST)
            != (operationKind == OPERATION_ACTOR_REQUEST
                && replyRoute.isPresent())) {
            throw new IllegalArgumentException(
                "frozen Actor operation kind does not match record kind");
        }
        String targetActorId = reader.text8();
        long objectGeneration = reader.nonzeroU64();
        reader.rid();
        reader.nonzeroU64();
        reader.nonzeroU64();
        reader.nonzeroU64();
        ApplicationPayload payload = reader.applicationPayload();
        reader.end();
        return new DecodedActor(
            targetActorId,
            source.nodeRid(),
            source.nodeGeneration(),
            source.ownerId(),
            source.ownerLeaseGeneration(),
            source.sessionRid(),
            source.bindingGeneration(),
            source.sessionSequence(),
            operation.high(),
            operation.low(),
            replyRoute,
            metadata.entries(),
            payload.packetName(),
            payload.contentType(),
            payload.payload(),
            objectGeneration);
    }

    public static byte[] encodeSpot(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkInternalMeshNode.PeerAuthorityFence targetOwner,
        ZLinkServiceM6BWireCodec.SpotMessage operation,
        byte[] metadataFrame,
        byte[] applicationPayloadEnvelope) {
        return encode(
            operation.request() ? RECORD_SPOT_REQUEST : RECORD_SPOT_SEND,
            sourceIdentity(source, operation.sourceSpotId(), null, null),
            metadataFrame,
            operation.operationHigh(),
            operation.operationLow(),
            operation.request()
                ? OPERATION_SPOT_REQUEST
                : OPERATION_NONE,
            operation.request() ? operation.correlation() : null,
            output -> {
                writeText8(output, operation.target().spotId());
                output.writeLong(operation.target().spotGeneration());
                writeRid(output, operation.target().targetNodeRid());
                output.writeLong(
                    operation.target().targetNodeGeneration());
                output.writeLong(
                    operation.target().authorityOwnerGeneration());
                output.writeLong(targetOwner.ownerLeaseGeneration());
                output.write(applicationPayloadEnvelope);
            });
    }

    public static byte[] encodeActor(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        ZLinkInternalMeshNode.PeerAuthorityFence targetOwner,
        ZLinkServiceM6BWireCodec.ActorMessage operation,
        byte[] metadataFrame,
        byte[] applicationPayloadEnvelope) {
        return encode(
            operation.request() ? RECORD_ACTOR_REQUEST : RECORD_ACTOR_SEND,
            sourceIdentity(
                source,
                null,
                operation.sourceActor() != null
                    ? operation.sourceActor()
                    : operation.boundSession() == null
                        ? null
                        : new ZLinkServiceM6BWireCodec.ActorIdentity(
                            operation.target().actor().actorId(),
                            operation.target().actor().generation()),
                operation.boundSession()),
            metadataFrame,
            operation.operationHigh(),
            operation.operationLow(),
            operation.request()
                ? OPERATION_ACTOR_REQUEST
                : OPERATION_NONE,
            operation.request() ? operation.correlation() : null,
            output -> {
                writeText8(
                    output, operation.target().actor().actorId());
                output.writeLong(
                    operation.target().actor().generation());
                writeRid(
                    output, operation.target().actor().nodeRid());
                output.writeLong(
                    operation.target().targetNodeGeneration());
                output.writeLong(
                    operation.target().authorityOwnerGeneration());
                output.writeLong(targetOwner.ownerLeaseGeneration());
                output.write(applicationPayloadEnvelope);
            });
    }

    private static byte[] encode(
        int recordKind,
        byte[] source,
        byte[] metadataFrame,
        long operationHigh,
        long operationLow,
        int operationKind,
        Long replyRouteId,
        Writer body) {
        if (operationHigh == 0 && operationLow == 0) {
            throw new IllegalArgumentException(
                "accepted operationId must not be zero");
        }
        byte[] metadata = metadataFrame == null
            ? new byte[0]
            : metadataFrame.clone();
        byte[] replyRoute = encodeSection(output -> {
            if (replyRouteId != null) {
                if (replyRouteId <= 0) {
                    throw new IllegalArgumentException(
                        "replyRouteId must be positive");
                }
                output.writeLong(replyRouteId);
            }
        });
        return encodeSection(output -> {
            output.writeByte(recordKind);
            output.write(source);
            output.writeByte(metadata.length == 0 ? 0 : 1);
            if (metadata.length != 0) {
                output.write(metadata);
            }
            output.writeLong(operationHigh);
            output.writeLong(operationLow);
            output.writeInt(operationKind);
            writeU16Section(output, replyRoute);
            body.write(output);
        });
    }

    private static byte[] sourceIdentity(
        ZLinkInternalMeshNode.PeerAuthorityFence source,
        String sourceSpotId,
        ZLinkServiceM6BWireCodec.ActorIdentity sourceActor,
        ZLinkServiceM6BWireCodec.BoundSessionTail boundSession) {
        int sourceKind = boundSession != null
            ? SOURCE_BOUND_SESSION
            : sourceActor != null
                ? SOURCE_ACTOR
                : sourceSpotId != null && !sourceSpotId.isBlank()
                    ? SOURCE_SPOT
                    : SOURCE_NODE;
        byte[] body = encodeSection(output -> {
            writeRid(output, source.sourceNodeRid());
            output.writeLong(source.sourceNodeGeneration());
            writeText8(output, source.ownerId());
            output.writeLong(source.ownerLeaseGeneration());
            if (sourceKind == SOURCE_SPOT) {
                writeText8(output, sourceSpotId);
            } else if (sourceKind == SOURCE_ACTOR
                || sourceKind == SOURCE_BOUND_SESSION) {
                writeText8(output, sourceActor.actorId());
                output.writeLong(sourceActor.generation());
                if (sourceKind == SOURCE_BOUND_SESSION) {
                    writeRid(output, boundSession.sourceSessionRid());
                    output.writeLong(
                        boundSession.sourceBindingGeneration());
                    output.writeLong(
                        boundSession.sourceSessionSequence());
                }
            }
        });
        return encodeSection(output -> {
            output.writeByte(sourceKind);
            writeU16Section(output, body);
        });
    }

    private static void writeRid(
        DataOutputStream output,
        RoutingId routingId) throws IOException {
        writeU8Bytes(output, routingId.toBytes(), "RoutingId");
    }

    private static void writeText8(
        DataOutputStream output,
        String value) throws IOException {
        if (value == null
            || value.isBlank()
            || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException(
                "text8 must be non-blank and contain no NUL");
        }
        writeU8Bytes(
            output,
            value.getBytes(StandardCharsets.UTF_8),
            "text8");
    }

    private static void writeU8Bytes(
        DataOutputStream output,
        byte[] value,
        String name) throws IOException {
        if (value.length == 0 || value.length > 255) {
            throw new IllegalArgumentException(
                name + " length must be in 1..255");
        }
        output.writeByte(value.length);
        output.write(value);
    }

    private static void writeU16Section(
        DataOutputStream output,
        byte[] value) throws IOException {
        if (value.length > 0xffff) {
            throw new IllegalArgumentException(
                "conditional body exceeds u16 length");
        }
        output.writeShort(value.length);
        output.write(value);
    }

    private static byte[] encodeSection(Writer writer) {
        try {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            DataOutputStream output = new DataOutputStream(bytes);
            writer.write(output);
            output.flush();
            return bytes.toByteArray();
        } catch (IOException failure) {
            throw new IllegalStateException(
                "failed to encode canonical frozen record", failure);
        }
    }

    @FunctionalInterface
    private interface Writer {
        void write(DataOutputStream output) throws IOException;
    }

    public record DecodedSpot(
        RoutingId sourceNodeRid,
        long sourceNodeGeneration,
        String sourceOwnerId,
        long sourceOwnerLeaseGeneration,
        Optional<String> sourceSpotId,
        long operationHigh,
        long operationLow,
        Optional<Long> replyRouteId,
        byte[] metadataFrame,
        String packetName,
        byte[] payload,
        String targetSpotId,
        long objectGeneration) {
        public DecodedSpot {
            metadataFrame = metadataFrame.clone();
            payload = payload.clone();
        }
    }

    public record DecodedActor(
        String actorId,
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
        Map<String, String> metadata,
        String packetName,
        String contentType,
        byte[] payload,
        long objectGeneration) {
        public DecodedActor {
            metadata = Map.copyOf(metadata);
            payload = payload.clone();
        }
    }

    private record Source(
        RoutingId nodeRid,
        long nodeGeneration,
        String ownerId,
        long ownerLeaseGeneration,
        Optional<String> spotId,
        RoutingId sessionRid,
        long bindingGeneration,
        long sessionSequence) {
    }

    private record OperationId(long high, long low) {
    }

    private record Metadata(byte[] encoded, Map<String, String> entries) {
    }

    private record ApplicationPayload(
        String packetName,
        String contentType,
        byte[] payload) {
    }

    private static final class Reader {
        private final byte[] bytes;
        private int offset;

        private Reader(byte[] bytes) {
            this.bytes = bytes == null ? new byte[0] : bytes.clone();
        }

        private Source source() {
            int kind = u8();
            int end = sectionEnd(u16());
            RoutingId nodeRid = rid();
            long nodeGeneration = nonzeroU64();
            String ownerId = text8();
            long ownerLeaseGeneration = nonzeroU64();
            Optional<String> spotId = Optional.empty();
            RoutingId sessionRid = null;
            long bindingGeneration = 0;
            long sessionSequence = 0;
            if (kind == SOURCE_SPOT) {
                spotId = Optional.of(text8());
            } else if (kind == SOURCE_ACTOR
                || kind == SOURCE_BOUND_SESSION) {
                text8();
                nonzeroU64();
                if (kind == SOURCE_BOUND_SESSION) {
                    sessionRid = rid();
                    bindingGeneration = nonzeroU64();
                    sessionSequence = nonzeroU64();
                }
            } else if (kind != SOURCE_NODE) {
                throw invalid("unknown frozen source kind");
            }
            requireOffset(end, "frozen source identity");
            return new Source(
                nodeRid,
                nodeGeneration,
                ownerId,
                ownerLeaseGeneration,
                spotId,
                sessionRid,
                bindingGeneration,
                sessionSequence);
        }

        private Metadata metadata() {
            int present = u8();
            if (present == 0) {
                return new Metadata(new byte[0], Map.of());
            }
            if (present != 1) {
                throw invalid("invalid frozen metadata presence");
            }
            int start = offset;
            if (u8() != 1) {
                throw invalid("unsupported metadata frame version");
            }
            int count = u8();
            Map<String, String> entries = new LinkedHashMap<>();
            for (int index = 0; index < count; index++) {
                String key = text8();
                int length = u16();
                String value = utf8(take(length));
                if (entries.putIfAbsent(key, value) != null) {
                    throw invalid("duplicate metadata key");
                }
            }
            return new Metadata(
                Arrays.copyOfRange(bytes, start, offset),
                entries);
        }

        private OperationId operationId() {
            long high = u64();
            long low = u64();
            if (high == 0 && low == 0) {
                throw invalid("zero frozen operationId");
            }
            return new OperationId(high, low);
        }

        private Optional<Long> replyRoute() {
            int end = sectionEnd(u16());
            Optional<Long> value = offset == end
                ? Optional.empty()
                : Optional.of(nonzeroU64());
            requireOffset(end, "frozen reply route");
            return value;
        }

        private ApplicationPayload applicationPayload() {
            if (u8() != 1) {
                throw invalid("unsupported application payload version");
            }
            int end = sectionEnd(u32());
            String packetName = text8();
            String contentType = text8();
            byte[] payload = take(u32());
            requireOffset(end, "application payload envelope");
            return new ApplicationPayload(packetName, contentType, payload);
        }

        private RoutingId rid() {
            return RoutingId.from(take(u8()));
        }

        private String text8() {
            return utf8(take(u8()));
        }

        private String utf8(byte[] value) {
            try {
                return StandardCharsets.UTF_8.newDecoder()
                    .onMalformedInput(
                        java.nio.charset.CodingErrorAction.REPORT)
                    .onUnmappableCharacter(
                        java.nio.charset.CodingErrorAction.REPORT)
                    .decode(java.nio.ByteBuffer.wrap(value))
                    .toString();
            } catch (java.nio.charset.CharacterCodingException failure) {
                throw invalid("invalid UTF-8");
            }
        }

        private long nonzeroU64() {
            long value = u64();
            if (value == 0) {
                throw invalid("expected nonzero u64");
            }
            return value;
        }

        private long u64() {
            require(8);
            long value = java.nio.ByteBuffer.wrap(bytes, offset, 8)
                .getLong();
            offset += 8;
            return value;
        }

        private int u32() {
            require(4);
            long value = Integer.toUnsignedLong(
                java.nio.ByteBuffer.wrap(bytes, offset, 4).getInt());
            offset += 4;
            if (value > Integer.MAX_VALUE) {
                throw invalid("u32 exceeds JVM buffer bound");
            }
            return (int) value;
        }

        private int u16() {
            require(2);
            int value = Short.toUnsignedInt(
                java.nio.ByteBuffer.wrap(bytes, offset, 2).getShort());
            offset += 2;
            return value;
        }

        private int u8() {
            require(1);
            return Byte.toUnsignedInt(bytes[offset++]);
        }

        private byte[] take(int length) {
            require(length);
            byte[] value = Arrays.copyOfRange(
                bytes, offset, offset + length);
            offset += length;
            return value;
        }

        private int sectionEnd(int length) {
            require(length);
            return offset + length;
        }

        private void requireOffset(int expected, String section) {
            if (offset != expected) {
                throw invalid(section + " length mismatch");
            }
        }

        private void require(int length) {
            if (length < 0 || offset > bytes.length - length) {
                throw invalid("truncated frozen record");
            }
        }

        private void end() {
            if (offset != bytes.length) {
                throw invalid("frozen record has trailing bytes");
            }
        }

        private IllegalArgumentException invalid(String message) {
            return new IllegalArgumentException(message);
        }
    }
}
