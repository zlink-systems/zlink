package systems.zlink.framework.runtime.internal.service;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.ByteArrayOutputStream;
import java.io.DataOutputStream;
import java.io.IOException;
import java.math.BigInteger;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.ArrayList;
import java.util.Base64;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.Optional;
import java.util.UUID;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.runtime.internal.locations
    .ZLinkServiceRelocationEnvelopeCodec;

/** Byte-stable ZLJR saved-work codec shared by canonical Actor Join recovery. */
public final class ZLinkActorJoinRecoveryCodec {
    public static final String PACKET_NAME =
        "__zlink.actor.routed_join.recovery";
    public static final String CONTENT_TYPE =
        "application/x-zlink-actor-routed-join-recovery-v1";
    public static final String RECREATE_CONTENT_TYPE =
        "application/vnd.zlink.actor-relocation.recreate";
    public static final String SNAPSHOT_CONTENT_TYPE =
        "application/vnd.zlink.actor-relocation.snapshot";

    private static final int MAGIC = 0x5a4c4a52;
    private static final int VERSION = 1;
    private static final int MAXIMUM_METADATA_BYTES = 256 * 1024;
    private static final int MAXIMUM_MESSAGE_BYTES = 1024 * 1024;
    private static final long FRAMEWORK_METADATA_RESERVATION_BYTES = 64L * 1024;
    private static final long ACCEPTED_JOURNAL_RESERVATION_BYTES = 16L * 1024 * 1024;
    private static final long SNAPSHOT_STATE_RESERVATION_BYTES = 64L * 1024 * 1024;
    private static final BigInteger MAXIMUM_U64 =
        BigInteger.ONE.shiftLeft(Long.SIZE).subtract(BigInteger.ONE);
    private static final ObjectMapper JSON = new ObjectMapper();

    private ZLinkActorJoinRecoveryCodec() {
    }

    public static UUID canonicalHandoffId(
        byte[] sourceActorNodeRid,
        String actorId,
        long actorGeneration,
        long sourceActorNodeGeneration,
        long correlation) {
        byte[] source = Objects.requireNonNull(
            sourceActorNodeRid, "sourceActorNodeRid").clone();
        byte[] actor = requireText(actorId, "actorId")
            .getBytes(StandardCharsets.UTF_8);
        if (actor.length > 0xffff) {
            throw new IllegalArgumentException("Actor id exceeds u16 bytes");
        }
        ByteBuffer material = ByteBuffer.allocate(Math.addExact(
            Math.addExact(source.length, Short.BYTES + actor.length),
            Long.BYTES * 3));
        material.put(source);
        material.putShort((short) actor.length);
        material.put(actor);
        material.putLong(actorGeneration);
        material.putLong(sourceActorNodeGeneration);
        material.putLong(correlation);
        byte[] hash;
        try {
            hash = MessageDigest.getInstance("SHA-256").digest(material.array());
        } catch (NoSuchAlgorithmException impossible) {
            throw new IllegalStateException("SHA-256 is unavailable", impossible);
        }
        // Guid(byte[16]) uses little endian for the first three components.
        byte[] guid = new byte[] {
            hash[3], hash[2], hash[1], hash[0],
            hash[5], hash[4], hash[7], hash[6],
            hash[8], hash[9], hash[10], hash[11],
            hash[12], hash[13], hash[14], hash[15]
        };
        ByteBuffer bytes = ByteBuffer.wrap(guid);
        return new UUID(bytes.getLong(), bytes.getLong());
    }

    public static long reservedPayloadBytes(
        int requestBytes,
        String relocationContentType) {
        if (requestBytes < 0) {
            throw new IllegalArgumentException("requestBytes must not be negative");
        }
        boolean snapshot = SNAPSHOT_CONTENT_TYPE.equals(relocationContentType);
        if (!snapshot && !RECREATE_CONTENT_TYPE.equals(relocationContentType)) {
            throw new IllegalArgumentException(
                "Actor Join relocation content type is invalid");
        }
        long result = Math.addExact(
            FRAMEWORK_METADATA_RESERVATION_BYTES,
            ACCEPTED_JOURNAL_RESERVATION_BYTES);
        result = Math.addExact(result, requestBytes);
        return snapshot
            ? Math.addExact(result, SNAPSHOT_STATE_RESERVATION_BYTES)
            : result;
    }

    public static byte[] encodeSavedWork(Recovery value) {
        validate(value);
        byte[] request = value.request();
        byte[] reply = value.reply();
        if (request.length > MAXIMUM_MESSAGE_BYTES
            || reply.length > MAXIMUM_MESSAGE_BYTES) {
            throw new IllegalArgumentException(
                "Actor Join recovery message exceeds 1 MiB");
        }
        byte[] metadata = encodeMetadata(value);
        if (metadata.length > MAXIMUM_METADATA_BYTES) {
            throw new IllegalArgumentException(
                "Actor Join recovery metadata exceeds 256 KiB");
        }
        byte[] zljr = section(output -> {
            output.writeInt(MAGIC);
            output.writeByte(VERSION);
            output.writeInt(metadata.length);
            output.writeInt(request.length);
            output.writeInt(reply.length);
            output.write(metadata);
            output.write(request);
            output.write(reply);
        });
        byte[] source = section(output -> {
            text8(output, value.sourceNodeRid().toHex());
            output.writeLong(value.actorNodeGeneration());
            text8(output, value.coordinator().ownerId());
            output.writeLong(value.coordinator().leaseGeneration());
        });
        byte[] payload = section(output -> {
            text8(output, PACKET_NAME);
            text8(output, CONTENT_TYPE);
            output.writeInt(zljr.length);
            output.write(zljr);
        });
        return section(output -> {
            output.writeByte(1); // node application send
            output.writeByte(1); // node source
            body16(output, source);
            output.writeByte(0); // no application metadata
            output.writeLong(0L);
            output.writeLong(0L);
            output.writeInt(0);
            output.writeShort(0);
            output.writeByte(1); // application payload present
            output.writeInt(payload.length);
            output.write(payload);
        });
    }

    /** Returns empty only when the frozen record is not a ZLJR record. */
    public static Optional<Recovery> decodeSavedWork(byte[] encoded) {
        Reader reader = new Reader(encoded);
        if (reader.remaining() < 2 || reader.u8() != 1 || reader.u8() != 1) {
            return Optional.empty();
        }
        Reader source = reader.body16();
        String sourceRidHex = source.text8();
        long sourceNodeGeneration = source.nonzeroU64("source node generation");
        String sourceOwnerId = source.text8();
        long sourceOwnerLeaseGeneration = source.nonzeroU64(
            "source owner lease generation");
        source.end("Actor Join recovery source");
        if (reader.u8() != 0
            || reader.u64() != 0 || reader.u64() != 0
            || reader.u32() != 0 || reader.u16() != 0
            || reader.u8() != 1) {
            return Optional.empty();
        }
        Reader payload = reader.body32();
        String packetName = payload.text8();
        String contentType = payload.text8();
        if (!PACKET_NAME.equals(packetName) || !CONTENT_TYPE.equals(contentType)) {
            return Optional.empty();
        }
        byte[] zljr = payload.bytes(payload.u32Length());
        payload.end("Actor Join recovery application payload");
        reader.end("Actor Join recovery saved work");
        Recovery recovery = decodeZljr(zljr);
        RoutingId sourceNodeRid = RoutingId.fromHex(sourceRidHex);
        if (!sourceNodeRid.equals(recovery.sourceNodeRid())
            || sourceNodeGeneration != recovery.actorNodeGeneration()
            || !sourceOwnerId.equals(recovery.coordinator().ownerId())
            || sourceOwnerLeaseGeneration
                != recovery.coordinator().leaseGeneration()) {
            throw invalid("Actor Join recovery source fence changed");
        }
        return Optional.of(recovery);
    }

    public static Optional<Recovery> decodeFromEnvelope(byte[] root) {
        ZLinkServiceRelocationEnvelopeCodec.Envelope envelope =
            ZLinkServiceRelocationEnvelopeCodec.decode(root);
        Recovery found = null;
        for (var work : envelope.savedWork()) {
            Optional<Recovery> candidate = decodeSavedWork(work.frozenRecord());
            if (candidate.isEmpty()) {
                continue;
            }
            if (found != null) {
                throw invalid("Actor Join recovery saved work is duplicated");
            }
            found = candidate.orElseThrow();
        }
        return Optional.ofNullable(found);
    }

    public static boolean isRecoverySavedWork(byte[] encoded) {
        return decodeSavedWork(encoded).isPresent();
    }

    private static Recovery decodeZljr(byte[] encoded) {
        Reader reader = new Reader(encoded);
        if (reader.u32() != MAGIC || reader.u8() != VERSION) {
            throw invalid("Actor Join recovery header is invalid");
        }
        int metadataLength = reader.boundedLength(
            MAXIMUM_METADATA_BYTES, "metadata");
        int requestLength = reader.boundedLength(
            MAXIMUM_MESSAGE_BYTES, "request");
        int replyLength = reader.boundedLength(
            MAXIMUM_MESSAGE_BYTES, "reply");
        byte[] metadata = reader.bytes(metadataLength);
        byte[] requestBytes = reader.bytes(requestLength);
        byte[] replyBytes = reader.bytes(replyLength);
        reader.end("Actor Join recovery payload");
        try {
            JsonNode root = JSON.readTree(metadata);
            JsonNode request = object(root, "Request");
            if (!text(request, "Request").isEmpty()
                || !array(request, "HandoffFrames").isEmpty()) {
                throw invalid("Actor Join recovery embedded bodies are not empty");
            }
            UUID relocationId = UUID.fromString(
                requiredText(request, "RelocationAggregateId"));
            String handoffId = requiredText(request, "HandoffId");
            if (!compact(relocationId).equals(handoffId)
                || !handoffId.equals(requiredText(request, "ReservationToken"))) {
                throw invalid("Actor Join recovery reservation identity changed");
            }
            byte[] sourceRid = base64(request, "SourceNodeRid");
            byte[] targetRid = base64(root, "TargetNodeRid");
            if (!java.util.Arrays.equals(
                    targetRid, base64(request, "TargetNodeRid"))) {
                throw invalid("Actor Join recovery target routing id changed");
            }
            long actorGeneration = nonzeroU64(request, "ActorGeneration");
            long actorAuthority = nonzeroU64(
                request, "ActorAuthorityOwnerGeneration");
            long targetAuthority = nonzeroU64(
                root, "TargetAuthorityOwnerGeneration");
            long operationHigh = u64(root, "OperationIdHigh");
            long operationLow = u64(root, "OperationIdLow");
            if (operationHigh == 0 && operationLow == 0) {
                throw invalid("Actor Join recovery operation identity is missing");
            }
            Recovery value = new Recovery(
                requiredText(request, "ActorId"),
                requiredText(request, "ActorType"),
                relocationId,
                requiredText(request, "SourceSpotId"),
                RoutingId.from(sourceRid),
                actorGeneration,
                actorAuthority,
                nonzeroU64(request, "ActorNodeGeneration"),
                nonzeroU64(request, "ExpectedOwnerLeaseGeneration"),
                requiredText(request, "RelocationContentType"),
                requiredText(request, "RequestContentType"),
                requestBytes,
                requiredText(root, "TargetSpotId"),
                RoutingId.from(targetRid),
                nonzeroU64(root, "TargetNodeGeneration"),
                positiveU64(root, "TargetSpotGeneration"),
                targetAuthority,
                positiveU64(request, "TargetSpotAuthorityOwnerGeneration"),
                new Coordinator(
                    requiredText(request, "RelocationCoordinatorOwnerId"),
                    nonzeroU64(request, "RelocationCoordinatorLeaseGeneration"),
                    RoutingId.from(base64(
                        request, "RelocationCoordinatorNodeRid")),
                    nonzeroU64(
                        request, "RelocationCoordinatorNodeGeneration"),
                    requiredText(
                        request,
                        "RelocationCoordinatorExpectedAuthorityStoreVersion")),
                new ZLinkActorJoinOperationId(operationHigh, operationLow),
                requiredText(root, "ReplyContentType"),
                replyBytes);
            if (u64(request, "RelocationAggregateGeneration") != 1L
                || u64(request, "RelocationChecksumCrc32c") != 0L
                || !"pending".equals(requiredText(
                    request, "RelocationReference"))
                || !allZero(base64(request, "RelocationInventoryDigest"), 32)
                || positiveU64(request, "ReservedPayloadBytes")
                    != reservedPayloadBytes(
                        requestBytes.length, value.relocationContentType())
                || nonzeroU64(request, "TargetNodeGeneration")
                    != value.targetNodeGeneration()
                || positiveU64(request, "TargetSpotGeneration")
                    != value.targetSpotGeneration()
                || nonzeroU64(request, "TargetAuthorityOwnerGeneration")
                    != targetAuthority) {
                throw invalid("Actor Join recovery canonical fields changed");
            }
            validate(value);
            return value;
        } catch (IOException | IllegalArgumentException failure) {
            if (failure instanceof IllegalArgumentException invalid) {
                throw invalid;
            }
            throw invalid("Actor Join recovery metadata is malformed", failure);
        }
    }

    private static byte[] encodeMetadata(Recovery value) {
        Map<String, Object> request = new LinkedHashMap<>();
        request.put("ActorId", value.actorId());
        request.put("ActorType", value.actorType());
        request.put("HandoffId", compact(value.relocationId()));
        request.put("BoundSessionNodeRid", null);
        request.put("BoundSessionRid", null);
        request.put("RelocationContentType", value.relocationContentType());
        request.put("RelocationReference", "pending");
        request.put("RelocationChecksumCrc32c", 0);
        request.put("RelocationAggregateId", value.relocationId().toString());
        request.put("RelocationAggregateGeneration", 1);
        request.put("RelocationInventoryDigest",
            Base64.getEncoder().encodeToString(new byte[32]));
        request.put("RequestContentType", value.requestContentType());
        request.put("Request", "");
        request.put("HandoffFrames", List.of());
        request.put("SourceSpotId", value.sourceSpotId());
        request.put("SourceNodeRid", base64(value.sourceNodeRid()));
        request.put("ActorGeneration", unsigned(value.actorGeneration()));
        request.put("ActorAuthorityOwnerGeneration",
            unsigned(value.actorAuthorityOwnerGeneration()));
        request.put("BoundSessionBindingToken", null);
        request.put("BoundSessionBindingGeneration", 0);
        request.put("BoundSessionObjectGeneration", 0);
        request.put("BoundSessionAuthorityOwnerGeneration", 0);
        request.put("BoundSessionMeshName", null);
        request.put("BoundSessionTargetNodeGeneration", 0);
        request.put("BoundSessionOwnerLeaseGeneration", 0);
        request.put("BoundSessionOwnerNodeGeneration", 0);
        request.put("BoundSessionAcceptedHighWater", 0);
        request.put("BoundSessionSessionOwnerId", null);
        request.put("BoundSessionSessionOwnerLeaseGeneration", 0);
        request.put("ReservationToken", compact(value.relocationId()));
        request.put("ReservedPayloadBytes", unsigned(reservedPayloadBytes(
            value.request().length, value.relocationContentType())));
        request.put("TargetNodeRid", base64(value.targetNodeRid()));
        request.put("TargetNodeGeneration", unsigned(value.targetNodeGeneration()));
        request.put("TargetSpotGeneration", unsigned(value.targetSpotGeneration()));
        request.put("TargetAuthorityOwnerGeneration",
            unsigned(value.targetAuthorityOwnerGeneration()));
        request.put("TargetSpotAuthorityOwnerGeneration",
            unsigned(value.targetSpotAuthorityOwnerGeneration()));
        request.put("RelocationCoordinatorOwnerId", value.coordinator().ownerId());
        request.put("RelocationCoordinatorLeaseGeneration",
            unsigned(value.coordinator().leaseGeneration()));
        request.put("RelocationCoordinatorNodeRid",
            base64(value.coordinator().nodeRid()));
        request.put("RelocationCoordinatorNodeGeneration",
            unsigned(value.coordinator().nodeGeneration()));
        request.put("RelocationCoordinatorExpectedAuthorityStoreVersion",
            value.coordinator().expectedAuthorityStoreVersion());
        request.put("ActorNodeGeneration", unsigned(value.actorNodeGeneration()));
        request.put("ExpectedOwnerLeaseGeneration",
            unsigned(value.expectedOwnerLeaseGeneration()));

        Map<String, Object> metadata = new LinkedHashMap<>();
        metadata.put("Request", request);
        metadata.put("TargetSpotId", value.targetSpotId());
        metadata.put("TargetNodeRid", base64(value.targetNodeRid()));
        metadata.put("TargetNodeGeneration", unsigned(value.targetNodeGeneration()));
        metadata.put("TargetSpotGeneration", unsigned(value.targetSpotGeneration()));
        metadata.put("TargetAuthorityOwnerGeneration",
            unsigned(value.targetAuthorityOwnerGeneration()));
        metadata.put("OperationIdHigh", unsigned(value.operationId().high()));
        metadata.put("OperationIdLow", unsigned(value.operationId().low()));
        metadata.put("ReplyContentType", value.replyContentType());
        metadata.put("Reply", "");
        try {
            return JSON.writeValueAsBytes(metadata);
        } catch (JsonProcessingException failure) {
            throw new IllegalArgumentException(
                "Actor Join recovery metadata could not be encoded", failure);
        }
    }

    private static void validate(Recovery value) {
        Objects.requireNonNull(value, "value");
        requireText(value.actorId(), "actorId");
        requireText(value.actorType(), "actorType");
        requireText(value.sourceSpotId(), "sourceSpotId");
        requireText(value.targetSpotId(), "targetSpotId");
        requireText(value.relocationContentType(), "relocationContentType");
        requireText(value.requestContentType(), "requestContentType");
        requireText(value.replyContentType(), "replyContentType");
        Objects.requireNonNull(value.sourceNodeRid(), "sourceNodeRid");
        Objects.requireNonNull(value.targetNodeRid(), "targetNodeRid");
        Objects.requireNonNull(value.relocationId(), "relocationId");
        Objects.requireNonNull(value.coordinator(), "coordinator");
        Objects.requireNonNull(value.operationId(), "operationId");
        if (value.relocationId().equals(new UUID(0L, 0L))
            || value.actorGeneration() == 0
            || value.actorAuthorityOwnerGeneration() == 0
            || value.actorNodeGeneration() == 0
            || value.expectedOwnerLeaseGeneration() == 0
            || value.targetNodeGeneration() == 0
            || value.targetSpotGeneration() <= 0
            || value.targetAuthorityOwnerGeneration() <= 0
            || value.targetSpotAuthorityOwnerGeneration() <= 0
            || value.operationId().high() == 0 && value.operationId().low() == 0
            || value.actorAuthorityOwnerGeneration() == Long.MAX_VALUE
            || value.targetAuthorityOwnerGeneration()
                != value.actorAuthorityOwnerGeneration() + 1) {
            throw invalid("Actor Join recovery identity is invalid");
        }
        Coordinator coordinator = value.coordinator();
        requireText(coordinator.ownerId(), "coordinator.ownerId");
        requireText(
            coordinator.expectedAuthorityStoreVersion(),
            "coordinator.expectedAuthorityStoreVersion");
        if (coordinator.leaseGeneration() <= 0
            || coordinator.nodeGeneration() == 0
            || !coordinator.nodeRid().equals(value.sourceNodeRid())
            || coordinator.nodeGeneration() != value.actorNodeGeneration()
            || coordinator.leaseGeneration()
                != value.expectedOwnerLeaseGeneration()) {
            throw invalid("Actor Join recovery coordinator fence is invalid");
        }
        Objects.requireNonNull(value.request(), "request");
        Objects.requireNonNull(value.reply(), "reply");
        reservedPayloadBytes(value.request().length, value.relocationContentType());
    }

    private static BigInteger unsigned(long value) {
        return new BigInteger(Long.toUnsignedString(value));
    }

    private static String base64(RoutingId value) {
        return Base64.getEncoder().encodeToString(value.toBytes());
    }

    private static String compact(UUID value) {
        return value.toString().replace("-", "");
    }

    private static JsonNode object(JsonNode parent, String field) {
        JsonNode value = parent == null ? null : parent.get(field);
        if (value == null || !value.isObject()) {
            throw invalid("Actor Join recovery " + field + " is not an object");
        }
        return value;
    }

    private static List<JsonNode> array(JsonNode parent, String field) {
        JsonNode value = parent.get(field);
        if (value == null || !value.isArray()) {
            throw invalid("Actor Join recovery " + field + " is not an array");
        }
        List<JsonNode> result = new ArrayList<>();
        value.forEach(result::add);
        return result;
    }

    private static String text(JsonNode parent, String field) {
        JsonNode value = parent.get(field);
        if (value == null || !value.isTextual()) {
            throw invalid("Actor Join recovery " + field + " is not text");
        }
        return value.textValue();
    }

    private static String requiredText(JsonNode parent, String field) {
        return requireText(text(parent, field), field);
    }

    private static String requireText(String value, String field) {
        if (value == null || value.isBlank() || value.indexOf('\0') >= 0) {
            throw invalid("Actor Join recovery " + field + " is empty");
        }
        return value;
    }

    private static byte[] base64(JsonNode parent, String field) {
        try {
            String encoded = requiredText(parent, field);
            byte[] decoded = Base64.getDecoder().decode(encoded);
            if (!Base64.getEncoder().encodeToString(decoded).equals(encoded)) {
                throw invalid("Actor Join recovery " + field + " is not canonical base64");
            }
            return decoded;
        } catch (IllegalArgumentException failure) {
            throw invalid("Actor Join recovery " + field + " is invalid base64", failure);
        }
    }

    private static long u64(JsonNode parent, String field) {
        JsonNode node = parent.get(field);
        if (node == null || !(node.isIntegralNumber() || node.isTextual())) {
            throw invalid("Actor Join recovery " + field + " is not u64");
        }
        try {
            BigInteger parsed = new BigInteger(node.asText());
            if (parsed.signum() < 0 || parsed.compareTo(MAXIMUM_U64) > 0) {
                throw invalid("Actor Join recovery " + field + " is outside u64");
            }
            return Long.parseUnsignedLong(parsed.toString());
        } catch (NumberFormatException failure) {
            throw invalid("Actor Join recovery " + field + " is not u64", failure);
        }
    }

    private static long nonzeroU64(JsonNode parent, String field) {
        long value = u64(parent, field);
        if (value == 0) {
            throw invalid("Actor Join recovery " + field + " is zero");
        }
        return value;
    }

    private static long positiveU64(JsonNode parent, String field) {
        long value = u64(parent, field);
        if (value <= 0) {
            throw invalid("Actor Join recovery " + field + " is not positive");
        }
        return value;
    }

    private static boolean allZero(byte[] value, int length) {
        if (value.length != length) return false;
        for (byte item : value) if (item != 0) return false;
        return true;
    }

    private static void text8(DataOutputStream output, String value)
        throws IOException {
        byte[] encoded = requireText(value, "text8")
            .getBytes(StandardCharsets.UTF_8);
        if (encoded.length > 255) {
            throw invalid("Actor Join recovery text8 exceeds 255 bytes");
        }
        output.writeByte(encoded.length);
        output.write(encoded);
    }

    private static void body16(DataOutputStream output, byte[] value)
        throws IOException {
        if (value.length > 0xffff) {
            throw invalid("Actor Join recovery body16 exceeds 65535 bytes");
        }
        output.writeShort(value.length);
        output.write(value);
    }

    private static byte[] section(Writer writer) {
        try {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            DataOutputStream output = new DataOutputStream(bytes);
            writer.write(output);
            output.flush();
            return bytes.toByteArray();
        } catch (IOException failure) {
            throw new IllegalStateException(
                "Actor Join recovery encoding failed", failure);
        }
    }

    private static IllegalArgumentException invalid(String message) {
        return new IllegalArgumentException(message);
    }

    private static IllegalArgumentException invalid(
        String message, Throwable failure) {
        return new IllegalArgumentException(message, failure);
    }

    @FunctionalInterface
    private interface Writer {
        void write(DataOutputStream output) throws IOException;
    }

    public record Coordinator(
        String ownerId,
        long leaseGeneration,
        RoutingId nodeRid,
        long nodeGeneration,
        String expectedAuthorityStoreVersion) {
        public Coordinator {
            Objects.requireNonNull(nodeRid, "nodeRid");
        }
    }

    public record Recovery(
        String actorId,
        String actorType,
        UUID relocationId,
        String sourceSpotId,
        RoutingId sourceNodeRid,
        long actorGeneration,
        long actorAuthorityOwnerGeneration,
        long actorNodeGeneration,
        long expectedOwnerLeaseGeneration,
        String relocationContentType,
        String requestContentType,
        byte[] request,
        String targetSpotId,
        RoutingId targetNodeRid,
        long targetNodeGeneration,
        long targetSpotGeneration,
        long targetAuthorityOwnerGeneration,
        long targetSpotAuthorityOwnerGeneration,
        Coordinator coordinator,
        ZLinkActorJoinOperationId operationId,
        String replyContentType,
        byte[] reply) {
        public Recovery {
            request = Objects.requireNonNull(request, "request").clone();
            reply = Objects.requireNonNull(reply, "reply").clone();
        }

        @Override public byte[] request() { return request.clone(); }
        @Override public byte[] reply() { return reply.clone(); }
    }

    private static final class Reader {
        private final byte[] bytes;
        private int offset;

        private Reader(byte[] bytes) {
            this.bytes = Objects.requireNonNull(bytes, "bytes").clone();
        }

        int remaining() { return bytes.length - offset; }
        int u8() { return Byte.toUnsignedInt(bytes(1)[0]); }
        int u16() { return Short.toUnsignedInt(ByteBuffer.wrap(bytes(2)).getShort()); }
        int u32() { return ByteBuffer.wrap(bytes(4)).getInt(); }
        long u64() { return ByteBuffer.wrap(bytes(8)).getLong(); }

        long nonzeroU64(String field) {
            long value = u64();
            if (value == 0) throw invalid("Actor Join recovery " + field + " is zero");
            return value;
        }

        int boundedLength(int maximum, String field) {
            long value = Integer.toUnsignedLong(u32());
            if (value > maximum) {
                throw invalid("Actor Join recovery " + field + " length exceeds its bound");
            }
            return (int) value;
        }

        int u32Length() {
            long value = Integer.toUnsignedLong(u32());
            if (value > remaining()) {
                throw invalid("Actor Join recovery body32 is truncated");
            }
            return (int) value;
        }

        Reader body16() { return new Reader(bytes(u16())); }
        Reader body32() { return new Reader(bytes(u32Length())); }
        String text8() {
            return new String(bytes(u8()), StandardCharsets.UTF_8);
        }

        byte[] bytes(int length) {
            if (length < 0 || length > remaining()) {
                throw invalid("Actor Join recovery bytes are truncated");
            }
            byte[] result = java.util.Arrays.copyOfRange(
                bytes, offset, offset + length);
            offset += length;
            return result;
        }

        void end(String field) {
            if (remaining() != 0) {
                throw invalid(field + " has trailing bytes");
            }
        }
    }
}
