package systems.zlink.framework.runtime.internal.service;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.internal.backend.ZLinkInternalMeshNode;

import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Optional;

/** Canonical service-wire-v1 frozen Spot and Actor operation encoder. */
public final class ZLinkServiceFrozenRecordCodec {
    public static final int ACCEPTED_RECORD_OVERHEAD_BYTES = 512;
    private static final int MAX_UTF8_BYTES_PER_UTF16_CODE_UNIT = 3;

    private ZLinkServiceFrozenRecordCodec() {}

    public static boolean isCanonical(byte[] encoded) {
        return encoded != null
                && encoded.length != 0
                && (encoded[0] == (int) ServiceWireCodec.MeshRecordKind.SPOT_SEND.wire
                        || encoded[0] == (int) ServiceWireCodec.MeshRecordKind.SPOT_REQUEST.wire
                        || encoded[0]
                                == (int)
                                        ServiceWireCodec.MeshRecordKind.INSTANCE_SPOT_ACTIVATION
                                                .wire
                        || isActorKind(encoded[0]));
    }

    public static boolean isActorRecord(byte[] encoded) {
        return encoded != null && encoded.length != 0 && isActorKind(encoded[0]);
    }

    private static boolean isActorKind(int kind) {
        return kind == ServiceWireCodec.MeshRecordKind.ACTOR_SEND.wire
                || kind == ServiceWireCodec.MeshRecordKind.ACTOR_REQUEST.wire;
    }

    public static DecodedSpot decodeSpot(byte[] encoded) {
        Reader reader = new Reader(encoded);
        int kind = reader.u8();
        boolean instance = kind == ServiceWireCodec.MeshRecordKind.INSTANCE_SPOT_ACTIVATION.wire;
        if (!instance
                && kind != (int) ServiceWireCodec.MeshRecordKind.SPOT_SEND.wire
                && kind != (int) ServiceWireCodec.MeshRecordKind.SPOT_REQUEST.wire) {
            throw new IllegalArgumentException("frozen record is not a Spot operation");
        }
        Source source = reader.source();
        Metadata metadata = reader.metadata();
        OperationId operation =
                instance ? new OperationId(reader.u64(), reader.u64()) : reader.operationId();
        int operationKind = reader.u32();
        Optional<Long> replyRoute = reader.replyRoute();
        String targetSpotId;
        long objectGeneration;
        ApplicationPayload payload;
        if (instance) {
            ServiceWireCodec.FrozenRecordBodyInstanceSpotActivation body;
            try {
                body =
                        (ServiceWireCodec.FrozenRecordBodyInstanceSpotActivation)
                                ServiceWireCodec.decodeFrozenRecordBody(
                                        reader.take(reader.bytes.length - reader.offset),
                                        ZLinkServiceM6BWireCodec.generatedContext(),
                                        ServiceWireCodec.MeshRecordKind.INSTANCE_SPOT_ACTIVATION);
            } catch (IOException failure) {
                throw new IllegalArgumentException("invalid frozen Instance Spot body", failure);
            }
            validateInstanceOperation(
                    body.operationKind(),
                    operationKind,
                    operation.high(),
                    operation.low(),
                    replyRoute.isPresent());
            if (!(body.route() instanceof ServiceWireCodec.InstanceRouteV1Ready route)) {
                throw new IllegalArgumentException(
                        "accepted Instance Spot record has no Ready fence");
            }
            targetSpotId = route.targetSpotId().value();
            objectGeneration = route.authority().objectGeneration().value();
            payload =
                    new ApplicationPayload(
                            body.payload().packetName().value(),
                            body.payload().contentType().value(),
                            body.payload().payload().value());
        } else {
            if ((kind == (int) ServiceWireCodec.MeshRecordKind.SPOT_REQUEST.wire)
                    != (operationKind == (int) ServiceWireCodec.MeshOperationKind.SPOT_REQUEST.wire
                            && replyRoute.isPresent())) {
                throw new IllegalArgumentException(
                        "frozen Spot operation kind does not match record kind");
            }
            targetSpotId = reader.text8();
            objectGeneration = reader.nonzeroU64();
            reader.rid();
            reader.nonzeroU64();
            reader.nonzeroU64();
            reader.nonzeroU64();
            payload = reader.applicationPayload();
            reader.end();
        }
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
                payload.contentType(),
                payload.payload(),
                targetSpotId,
                objectGeneration);
    }

    public static DecodedActor decodeActor(byte[] encoded) {
        Reader reader = new Reader(encoded);
        int kind = reader.u8();
        if (!isActorKind(kind)) {
            throw new IllegalArgumentException("frozen record is not an Actor operation");
        }
        Source source = reader.source();
        Metadata metadata = reader.metadata();
        OperationId operation = reader.operationId();
        int operationKind = reader.u32();
        Optional<Long> replyRoute = reader.replyRoute();
        if ((kind == (int) ServiceWireCodec.MeshRecordKind.ACTOR_REQUEST.wire)
                != (operationKind == (int) ServiceWireCodec.MeshOperationKind.ACTOR_REQUEST.wire
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
                operation.request()
                        ? (int) ServiceWireCodec.MeshRecordKind.SPOT_REQUEST.wire
                        : (int) ServiceWireCodec.MeshRecordKind.SPOT_SEND.wire,
                sourceIdentity(source, operation.sourceSpotId(), null, null),
                metadataFrame,
                operation.operationHigh(),
                operation.operationLow(),
                operation.request()
                        ? (int) ServiceWireCodec.MeshOperationKind.SPOT_REQUEST.wire
                        : (int) ServiceWireCodec.MeshOperationKind.NONE.wire,
                operation.request() ? operation.correlation() : null,
                output -> {
                    writeText8(output, operation.target().spotId());
                    output.writeLong(operation.target().spotGeneration());
                    writeRid(output, operation.target().targetNodeRid());
                    output.writeLong(operation.target().targetNodeGeneration());
                    output.writeLong(operation.target().authorityOwnerGeneration());
                    output.writeLong(targetOwner.ownerLeaseGeneration());
                    output.write(applicationPayloadEnvelope);
                });
    }

    public static int instanceAcceptedRecordOverhead(
            ZLinkServiceM6BWireCodec.InstanceRouteFence target) {
        // These Ready authority fields add UTF-8 text to the ordinary Spot record overhead.
        return ACCEPTED_RECORD_OVERHEAD_BYTES
                + MAX_UTF8_BYTES_PER_UTF16_CODE_UNIT
                        * (target.ownerId().length() + target.storeVersion().length());
    }

    public static byte[] encodeInstanceSpot(
            ZLinkInternalMeshNode.PeerAuthorityFence source,
            ZLinkServiceM6BWireCodec.InstanceSpotMessage operation,
            ZLinkServiceM6BWireCodec.InstanceRouteFence target,
            byte[] metadataFrame,
            byte[] applicationPayloadEnvelope) {
        var kind = ServiceWireCodec.MeshRecordKind.INSTANCE_SPOT_ACTIVATION;
        var instanceOperation =
                operation.request()
                        ? ServiceWireCodec.InstanceOperationKind.REQUEST
                        : ServiceWireCodec.InstanceOperationKind.SEND;
        var operationKind = instanceMeshOperation(instanceOperation);
        validateInstanceOperation(
                instanceOperation,
                operationKind.wire,
                operation.operationHigh(),
                operation.operationLow(),
                operation.replyRouteId() != null);
        byte[] body;
        try {
            var context = ZLinkServiceM6BWireCodec.generatedContext();
            body =
                    ServiceWireCodec.encodeFrozenRecordBody(
                            new ServiceWireCodec.FrozenRecordBodyInstanceSpotActivation(
                                    ZLinkServiceM6BWireCodec.generatedReadyInstanceRoute(
                                            target, operation.instanceIntent()),
                                    new ServiceWireCodec.NonzeroU64(
                                            operation.sourceNodeGeneration()),
                                    instanceOperation,
                                    ServiceWireCodec.decodeApplicationPayloadEnvelopeV1(
                                            applicationPayloadEnvelope, context)),
                            context,
                            kind);
        } catch (IOException failure) {
            throw new IllegalArgumentException("invalid accepted Instance Spot body", failure);
        }
        return encode(
                (int) kind.wire,
                sourceIdentity(source, operation.sourceSpotId(), null, null),
                metadataFrame,
                operation.operationHigh(),
                operation.operationLow(),
                (int) operationKind.wire,
                operation.replyRouteId(),
                output -> output.write(body));
    }

    static void validateInstanceOperation(ServiceWireCodec.FrozenRecord record) {
        if (record.body() instanceof ServiceWireCodec.FrozenRecordBodyInstanceSpotActivation body) {
            validateInstanceOperation(
                    body.operationKind(),
                    record.operationKind().wire,
                    record.operationId().high().value(),
                    record.operationId().low().value(),
                    record.replyRoute()
                            instanceof ServiceWireCodec.FrozenReplyRouteInstanceSpotRequest);
        }
    }

    private static void validateInstanceOperation(
            ServiceWireCodec.InstanceOperationKind instanceOperation,
            long operationKind,
            long operationHigh,
            long operationLow,
            boolean hasReply) {
        boolean request = instanceOperation == ServiceWireCodec.InstanceOperationKind.REQUEST;
        if (operationKind != instanceMeshOperation(instanceOperation).wire
                || request != (operationHigh != 0 || operationLow != 0)
                || request != hasReply) {
            throw new IllegalArgumentException(
                    "invalid frozen Instance Spot operation identity or reply route");
        }
    }

    private static ServiceWireCodec.MeshOperationKind instanceMeshOperation(
            ServiceWireCodec.InstanceOperationKind operation) {
        return operation == ServiceWireCodec.InstanceOperationKind.REQUEST
                ? ServiceWireCodec.MeshOperationKind.INSTANCE_SPOT_REQUEST
                : ServiceWireCodec.MeshOperationKind.NONE;
    }

    public static byte[] encodeActor(
            ZLinkInternalMeshNode.PeerAuthorityFence source,
            ZLinkInternalMeshNode.PeerAuthorityFence targetOwner,
            ZLinkServiceM6BWireCodec.ActorMessage operation,
            byte[] metadataFrame,
            byte[] applicationPayloadEnvelope) {
        return encode(
                operation.request()
                        ? (int) ServiceWireCodec.MeshRecordKind.ACTOR_REQUEST.wire
                        : (int) ServiceWireCodec.MeshRecordKind.ACTOR_SEND.wire,
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
                        ? (int) ServiceWireCodec.MeshOperationKind.ACTOR_REQUEST.wire
                        : (int) ServiceWireCodec.MeshOperationKind.NONE.wire,
                operation.request() ? operation.correlation() : null,
                output -> {
                    writeText8(output, operation.target().actor().actorId());
                    output.writeLong(operation.target().actor().generation());
                    writeRid(output, operation.target().actor().nodeRid());
                    output.writeLong(operation.target().targetNodeGeneration());
                    output.writeLong(operation.target().authorityOwnerGeneration());
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
        if (recordKind != ServiceWireCodec.MeshRecordKind.INSTANCE_SPOT_ACTIVATION.wire
                && operationHigh == 0
                && operationLow == 0) {
            throw new IllegalArgumentException("accepted operationId must not be zero");
        }
        byte[] metadata = metadataFrame == null ? new byte[0] : metadataFrame.clone();
        byte[] replyRoute =
                encodeSection(
                        output -> {
                            if (replyRouteId != null) {
                                if (replyRouteId == 0) {
                                    throw new IllegalArgumentException(
                                            "replyRouteId must be non-zero");
                                }
                                output.writeLong(replyRouteId);
                            }
                        });
        return encodeSection(
                output -> {
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
        int sourceKind =
                boundSession != null
                        ? (int) ServiceWireCodec.FrozenSourceKind.BOUND_SESSION.wire
                        : sourceActor != null
                                ? (int) ServiceWireCodec.FrozenSourceKind.ACTOR.wire
                                : sourceSpotId != null && !sourceSpotId.isBlank()
                                        ? (int) ServiceWireCodec.FrozenSourceKind.SPOT.wire
                                        : (int) ServiceWireCodec.FrozenSourceKind.NODE.wire;
        byte[] body =
                encodeSection(
                        output -> {
                            writeRid(output, source.sourceNodeRid());
                            output.writeLong(source.sourceNodeGeneration());
                            writeText8(output, source.ownerId());
                            output.writeLong(source.ownerLeaseGeneration());
                            if (sourceKind == (int) ServiceWireCodec.FrozenSourceKind.SPOT.wire) {
                                writeText8(output, sourceSpotId);
                            } else if (sourceKind
                                            == (int) ServiceWireCodec.FrozenSourceKind.ACTOR.wire
                                    || sourceKind
                                            == (int)
                                                    ServiceWireCodec.FrozenSourceKind.BOUND_SESSION
                                                            .wire) {
                                writeText8(output, sourceActor.actorId());
                                output.writeLong(sourceActor.generation());
                                if (sourceKind
                                        == (int)
                                                ServiceWireCodec.FrozenSourceKind.BOUND_SESSION
                                                        .wire) {
                                    writeRid(output, boundSession.sourceSessionRid());
                                    output.writeLong(boundSession.sourceBindingGeneration());
                                    output.writeLong(boundSession.sourceSessionSequence());
                                }
                            }
                        });
        return encodeSection(
                output -> {
                    output.writeByte(sourceKind);
                    writeU16Section(output, body);
                });
    }

    private static void writeRid(DataOutputStream output, RoutingId routingId) throws IOException {
        writeU8Bytes(output, routingId.toBytes(), "RoutingId");
    }

    private static void writeText8(DataOutputStream output, String value) throws IOException {
        if (value == null || value.isBlank() || value.indexOf('\0') >= 0) {
            throw new IllegalArgumentException("text8 must be non-blank and contain no NUL");
        }
        writeU8Bytes(output, value.getBytes(StandardCharsets.UTF_8), "text8");
    }

    private static void writeU8Bytes(DataOutputStream output, byte[] value, String name)
            throws IOException {
        if (value.length == 0 || value.length > 255) {
            throw new IllegalArgumentException(name + " length must be in 1..255");
        }
        output.writeByte(value.length);
        output.write(value);
    }

    private static void writeU16Section(DataOutputStream output, byte[] value) throws IOException {
        if (value.length > 0xffff) {
            throw new IllegalArgumentException("conditional body exceeds u16 length");
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
            throw new IllegalStateException("failed to encode canonical frozen record", failure);
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
            String contentType,
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
            long sessionSequence) {}

    private record OperationId(long high, long low) {}

    private record Metadata(byte[] encoded, Map<String, String> entries) {}

    private record ApplicationPayload(String packetName, String contentType, byte[] payload) {}

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
            if (kind == (int) ServiceWireCodec.FrozenSourceKind.SPOT.wire) {
                spotId = Optional.of(text8());
            } else if (kind == (int) ServiceWireCodec.FrozenSourceKind.ACTOR.wire
                    || kind == (int) ServiceWireCodec.FrozenSourceKind.BOUND_SESSION.wire) {
                text8();
                nonzeroU64();
                if (kind == (int) ServiceWireCodec.FrozenSourceKind.BOUND_SESSION.wire) {
                    sessionRid = rid();
                    bindingGeneration = nonzeroU64();
                    sessionSequence = nonzeroU64();
                }
            } else if (kind != (int) ServiceWireCodec.FrozenSourceKind.NODE.wire) {
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
            return new Metadata(Arrays.copyOfRange(bytes, start, offset), entries);
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
            Optional<Long> value = offset == end ? Optional.empty() : Optional.of(nonzeroU64());
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
                return StandardCharsets.UTF_8
                        .newDecoder()
                        .onMalformedInput(CodingErrorAction.REPORT)
                        .onUnmappableCharacter(CodingErrorAction.REPORT)
                        .decode(ByteBuffer.wrap(value))
                        .toString();
            } catch (CharacterCodingException failure) {
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
            long value = ByteBuffer.wrap(bytes, offset, 8).getLong();
            offset += 8;
            return value;
        }

        private int u32() {
            require(4);
            long value = Integer.toUnsignedLong(ByteBuffer.wrap(bytes, offset, 4).getInt());
            offset += 4;
            if (value > Integer.MAX_VALUE) {
                throw invalid("u32 exceeds JVM buffer bound");
            }
            return (int) value;
        }

        private int u16() {
            require(2);
            int value = Short.toUnsignedInt(ByteBuffer.wrap(bytes, offset, 2).getShort());
            offset += 2;
            return value;
        }

        private int u8() {
            require(1);
            return Byte.toUnsignedInt(bytes[offset++]);
        }

        private byte[] take(int length) {
            require(length);
            byte[] value = Arrays.copyOfRange(bytes, offset, offset + length);
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
