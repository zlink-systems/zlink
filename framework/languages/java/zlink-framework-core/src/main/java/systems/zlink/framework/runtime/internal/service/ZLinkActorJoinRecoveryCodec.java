package systems.zlink.framework.runtime.internal.service;

import com.fasterxml.jackson.core.JsonProcessingException;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.actors.ZLinkActorJoinOperationId;
import systems.zlink.framework.runtime.internal.locations.ZLinkServiceRelocationEnvelopeCodec;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

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

/** Byte-stable ZLJR saved-work codec shared by canonical Actor Join recovery. */
public final class ZLinkActorJoinRecoveryCodec {
    private static final String FIELD_ACTOR_AUTHORITY_OWNER_GENERATION =
            "ActorAuthorityOwnerGeneration";
    private static final String FIELD_ACTOR_GENERATION = "ActorGeneration";
    private static final String FIELD_ACTOR_ID = "ActorId";
    private static final String FIELD_ACTOR_NODE_GENERATION = "ActorNodeGeneration";
    private static final String FIELD_ACTOR_TYPE = "ActorType";
    private static final String FIELD_EXPECTED_OWNER_LEASE_GENERATION =
            "ExpectedOwnerLeaseGeneration";
    private static final String FIELD_HANDOFF_FRAMES = "HandoffFrames";
    private static final String FIELD_HANDOFF_ID = "HandoffId";
    private static final String FIELD_OPERATION_ID_HIGH = "OperationIdHigh";
    private static final String FIELD_OPERATION_ID_LOW = "OperationIdLow";
    private static final String FIELD_RELOCATION_AGGREGATE_GENERATION =
            "RelocationAggregateGeneration";
    private static final String FIELD_RELOCATION_AGGREGATE_ID = "RelocationAggregateId";
    private static final String FIELD_RELOCATION_CHECKSUM_CRC32C = "RelocationChecksumCrc32c";
    private static final String FIELD_RELOCATION_CONTENT_TYPE = "RelocationContentType";
    private static final String FIELD_RELOCATION_COORDINATOR_EXPECTED_AUTHORITY_STORE_VERSION =
            "RelocationCoordinatorExpectedAuthorityStoreVersion";
    private static final String FIELD_RELOCATION_COORDINATOR_LEASE_GENERATION =
            "RelocationCoordinatorLeaseGeneration";
    private static final String FIELD_RELOCATION_COORDINATOR_NODE_GENERATION =
            "RelocationCoordinatorNodeGeneration";
    private static final String FIELD_RELOCATION_COORDINATOR_NODE_RID =
            "RelocationCoordinatorNodeRid";
    private static final String FIELD_RELOCATION_COORDINATOR_OWNER_ID =
            "RelocationCoordinatorOwnerId";
    private static final String FIELD_RELOCATION_INVENTORY_DIGEST = "RelocationInventoryDigest";
    private static final String FIELD_RELOCATION_REFERENCE = "RelocationReference";
    private static final String FIELD_REPLY_CONTENT_TYPE = "ReplyContentType";
    private static final String FIELD_REQUEST = "Request";
    private static final String FIELD_REQUEST_CONTENT_TYPE = "RequestContentType";
    private static final String FIELD_RESERVATION_TOKEN = "ReservationToken";
    private static final String FIELD_RESERVED_PAYLOAD_BYTES = "ReservedPayloadBytes";
    private static final String FIELD_SOURCE_NODE_RID = "SourceNodeRid";
    private static final String FIELD_SOURCE_SPOT_ID = "SourceSpotId";
    private static final String FIELD_TARGET_AUTHORITY_OWNER_GENERATION =
            "TargetAuthorityOwnerGeneration";
    private static final String FIELD_TARGET_NODE_GENERATION = "TargetNodeGeneration";
    private static final String FIELD_TARGET_NODE_RID = "TargetNodeRid";
    private static final String FIELD_TARGET_SPOT_AUTHORITY_OWNER_GENERATION =
            "TargetSpotAuthorityOwnerGeneration";
    private static final String FIELD_TARGET_SPOT_GENERATION = "TargetSpotGeneration";
    private static final String FIELD_TARGET_SPOT_ID = "TargetSpotId";
    private static final String RELOCATION_REFERENCE_PENDING = "pending";

    public static final String PACKET_NAME = "__zlink.actor.routed_join.recovery";
    public static final String CONTENT_TYPE = "application/x-zlink-actor-routed-join-recovery-v1";
    public static final String RECREATE_CONTENT_TYPE =
            "application/vnd.zlink.actor-relocation.recreate";
    public static final String SNAPSHOT_CONTENT_TYPE =
            "application/vnd.zlink.actor-relocation.snapshot";

    private static final int MAXIMUM_METADATA_BYTES = 256 * 1024;
    private static final long FRAMEWORK_METADATA_RESERVATION_BYTES = 64L * 1024;
    private static final long ACCEPTED_JOURNAL_RESERVATION_BYTES = 16L * 1024 * 1024;
    private static final long SNAPSHOT_STATE_RESERVATION_BYTES = 64L * 1024 * 1024;
    private static final BigInteger MAXIMUM_U64 =
            BigInteger.ONE.shiftLeft(Long.SIZE).subtract(BigInteger.ONE);
    private static final ObjectMapper JSON = new ObjectMapper();

    private ZLinkActorJoinRecoveryCodec() {}

    public static UUID canonicalHandoffId(
            byte[] sourceActorNodeRid,
            String actorId,
            long actorGeneration,
            long sourceActorNodeGeneration,
            long correlation) {
        byte[] source = Objects.requireNonNull(sourceActorNodeRid, "sourceActorNodeRid").clone();
        byte[] actor = requireText(actorId, "actorId").getBytes(StandardCharsets.UTF_8);
        if (actor.length > 0xffff) {
            throw new IllegalArgumentException("Actor id exceeds u16 bytes");
        }
        ByteBuffer material =
                ByteBuffer.allocate(
                        Math.addExact(
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
        byte[] guid =
                new byte[] {
                    hash[3], hash[2], hash[1], hash[0],
                    hash[5], hash[4], hash[7], hash[6],
                    hash[8], hash[9], hash[10], hash[11],
                    hash[12], hash[13], hash[14], hash[15]
                };
        ByteBuffer bytes = ByteBuffer.wrap(guid);
        return new UUID(bytes.getLong(), bytes.getLong());
    }

    public static long reservedPayloadBytes(int requestBytes, String relocationContentType) {
        if (requestBytes < 0) {
            throw new IllegalArgumentException("requestBytes must not be negative");
        }
        boolean snapshot = SNAPSHOT_CONTENT_TYPE.equals(relocationContentType);
        if (!snapshot && !RECREATE_CONTENT_TYPE.equals(relocationContentType)) {
            throw new IllegalArgumentException("Actor Join relocation content type is invalid");
        }
        long result =
                Math.addExact(
                        FRAMEWORK_METADATA_RESERVATION_BYTES, ACCEPTED_JOURNAL_RESERVATION_BYTES);
        result = Math.addExact(result, requestBytes);
        return snapshot ? Math.addExact(result, SNAPSHOT_STATE_RESERVATION_BYTES) : result;
    }

    public static byte[] encodeSavedWork(Recovery value) {
        validate(value);
        byte[] request = value.request();
        byte[] reply = value.reply();
        byte[] metadata = encodeMetadata(value);
        if (metadata.length > MAXIMUM_METADATA_BYTES) {
            throw new IllegalArgumentException("Actor Join recovery metadata exceeds 256 KiB");
        }
        try {
            return ServiceWirePilotCodec.encodeZljrRecordV1(
                    new ServiceWirePilotCodec.ZljrRecordV1(
                            new ServiceWirePilotCodec.ZljrNodeSourceV1(
                                    value.sourceNodeRid().toHex().getBytes(StandardCharsets.UTF_8),
                                    value.actorNodeGeneration(),
                                    value.coordinator().ownerId(),
                                    value.coordinator().leaseGeneration()),
                            new ServiceWirePilotCodec.OperationId(0, 0),
                            metadata,
                            request,
                            reply));
        } catch (IOException failure) {
            throw invalid("Actor Join recovery encoding failed", failure);
        }
    }

    /** Returns empty when the generated codec does not recognize a ZLJR record. */
    public static Optional<Recovery> decodeSavedWork(byte[] encoded) {
        Objects.requireNonNull(encoded, "encoded");
        final ServiceWirePilotCodec.ZljrRecordV1 generated;
        try {
            generated = ServiceWirePilotCodec.decodeZljrRecordV1(encoded);
        } catch (IOException failure) {
            return Optional.empty();
        }
        if (generated.operation().high() != 0 || generated.operation().low() != 0) {
            throw invalid("Actor Join recovery operation field changed");
        }
        Recovery recovery =
                decodeMetadata(generated.metadata(), generated.request(), generated.reply());
        String sourceRidHex = new String(generated.source().nodeRid(), StandardCharsets.UTF_8);
        RoutingId sourceNodeRid = RoutingId.fromHex(sourceRidHex);
        if (!sourceNodeRid.equals(recovery.sourceNodeRid())
                || generated.source().nodeGeneration() != recovery.actorNodeGeneration()
                || !generated.source().ownerId().equals(recovery.coordinator().ownerId())
                || generated.source().ownerLeaseGeneration()
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

    private static Recovery decodeMetadata(
            byte[] metadata, byte[] requestBytes, byte[] replyBytes) {
        try {
            JsonNode root = JSON.readTree(metadata);
            JsonNode request = object(root, FIELD_REQUEST);
            if (!text(request, FIELD_REQUEST).isEmpty()
                    || !array(request, FIELD_HANDOFF_FRAMES).isEmpty()) {
                throw invalid("Actor Join recovery embedded bodies are not empty");
            }
            UUID relocationId =
                    UUID.fromString(requiredText(request, FIELD_RELOCATION_AGGREGATE_ID));
            String handoffId = requiredText(request, FIELD_HANDOFF_ID);
            if (!compact(relocationId).equals(handoffId)
                    || !handoffId.equals(requiredText(request, FIELD_RESERVATION_TOKEN))) {
                throw invalid("Actor Join recovery reservation identity changed");
            }
            byte[] sourceRid = base64(request, FIELD_SOURCE_NODE_RID);
            byte[] targetRid = base64(root, FIELD_TARGET_NODE_RID);
            if (!java.util.Arrays.equals(targetRid, base64(request, FIELD_TARGET_NODE_RID))) {
                throw invalid("Actor Join recovery target routing id changed");
            }
            long actorGeneration = nonzeroU64(request, FIELD_ACTOR_GENERATION);
            long actorAuthority = nonzeroU64(request, FIELD_ACTOR_AUTHORITY_OWNER_GENERATION);
            long targetAuthority = nonzeroU64(root, FIELD_TARGET_AUTHORITY_OWNER_GENERATION);
            long operationHigh = u64(root, FIELD_OPERATION_ID_HIGH);
            long operationLow = u64(root, FIELD_OPERATION_ID_LOW);
            if (operationHigh == 0 && operationLow == 0) {
                throw invalid("Actor Join recovery operation identity is missing");
            }
            Recovery value =
                    new Recovery(
                            requiredText(request, FIELD_ACTOR_ID),
                            requiredText(request, FIELD_ACTOR_TYPE),
                            relocationId,
                            requiredText(request, FIELD_SOURCE_SPOT_ID),
                            RoutingId.from(sourceRid),
                            actorGeneration,
                            actorAuthority,
                            nonzeroU64(request, FIELD_ACTOR_NODE_GENERATION),
                            nonzeroU64(request, FIELD_EXPECTED_OWNER_LEASE_GENERATION),
                            requiredText(request, FIELD_RELOCATION_CONTENT_TYPE),
                            requiredText(request, FIELD_REQUEST_CONTENT_TYPE),
                            requestBytes,
                            requiredText(root, FIELD_TARGET_SPOT_ID),
                            RoutingId.from(targetRid),
                            nonzeroU64(root, FIELD_TARGET_NODE_GENERATION),
                            positiveU64(root, FIELD_TARGET_SPOT_GENERATION),
                            targetAuthority,
                            positiveU64(request, FIELD_TARGET_SPOT_AUTHORITY_OWNER_GENERATION),
                            new Coordinator(
                                    requiredText(request, FIELD_RELOCATION_COORDINATOR_OWNER_ID),
                                    nonzeroU64(
                                            request, FIELD_RELOCATION_COORDINATOR_LEASE_GENERATION),
                                    RoutingId.from(
                                            base64(request, FIELD_RELOCATION_COORDINATOR_NODE_RID)),
                                    nonzeroU64(
                                            request, FIELD_RELOCATION_COORDINATOR_NODE_GENERATION),
                                    requiredText(
                                            request,
                                            FIELD_RELOCATION_COORDINATOR_EXPECTED_AUTHORITY_STORE_VERSION)),
                            new ZLinkActorJoinOperationId(operationHigh, operationLow),
                            requiredText(root, FIELD_REPLY_CONTENT_TYPE),
                            replyBytes);
            if (u64(request, FIELD_RELOCATION_AGGREGATE_GENERATION) != 1L
                    || u64(request, FIELD_RELOCATION_CHECKSUM_CRC32C) != 0L
                    || !RELOCATION_REFERENCE_PENDING.equals(
                            requiredText(request, FIELD_RELOCATION_REFERENCE))
                    || !allZero(base64(request, FIELD_RELOCATION_INVENTORY_DIGEST), 32)
                    || positiveU64(request, FIELD_RESERVED_PAYLOAD_BYTES)
                            != reservedPayloadBytes(
                                    requestBytes.length, value.relocationContentType())
                    || nonzeroU64(request, FIELD_TARGET_NODE_GENERATION)
                            != value.targetNodeGeneration()
                    || positiveU64(request, FIELD_TARGET_SPOT_GENERATION)
                            != value.targetSpotGeneration()
                    || nonzeroU64(request, FIELD_TARGET_AUTHORITY_OWNER_GENERATION)
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
        request.put(FIELD_ACTOR_ID, value.actorId());
        request.put(FIELD_ACTOR_TYPE, value.actorType());
        request.put(FIELD_HANDOFF_ID, compact(value.relocationId()));
        request.put("BoundSessionNodeRid", null);
        request.put("BoundSessionRid", null);
        request.put(FIELD_RELOCATION_CONTENT_TYPE, value.relocationContentType());
        request.put(FIELD_RELOCATION_REFERENCE, RELOCATION_REFERENCE_PENDING);
        request.put(FIELD_RELOCATION_CHECKSUM_CRC32C, 0);
        request.put(FIELD_RELOCATION_AGGREGATE_ID, value.relocationId().toString());
        request.put(FIELD_RELOCATION_AGGREGATE_GENERATION, 1);
        request.put(
                FIELD_RELOCATION_INVENTORY_DIGEST,
                Base64.getEncoder().encodeToString(new byte[32]));
        request.put(FIELD_REQUEST_CONTENT_TYPE, value.requestContentType());
        request.put(FIELD_REQUEST, "");
        request.put(FIELD_HANDOFF_FRAMES, List.of());
        request.put(FIELD_SOURCE_SPOT_ID, value.sourceSpotId());
        request.put(FIELD_SOURCE_NODE_RID, base64(value.sourceNodeRid()));
        request.put(FIELD_ACTOR_GENERATION, unsigned(value.actorGeneration()));
        request.put(
                FIELD_ACTOR_AUTHORITY_OWNER_GENERATION,
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
        request.put(FIELD_RESERVATION_TOKEN, compact(value.relocationId()));
        request.put(
                FIELD_RESERVED_PAYLOAD_BYTES,
                unsigned(
                        reservedPayloadBytes(
                                value.request().length, value.relocationContentType())));
        request.put(FIELD_TARGET_NODE_RID, base64(value.targetNodeRid()));
        request.put(FIELD_TARGET_NODE_GENERATION, unsigned(value.targetNodeGeneration()));
        request.put(FIELD_TARGET_SPOT_GENERATION, unsigned(value.targetSpotGeneration()));
        request.put(
                FIELD_TARGET_AUTHORITY_OWNER_GENERATION,
                unsigned(value.targetAuthorityOwnerGeneration()));
        request.put(
                FIELD_TARGET_SPOT_AUTHORITY_OWNER_GENERATION,
                unsigned(value.targetSpotAuthorityOwnerGeneration()));
        request.put(FIELD_RELOCATION_COORDINATOR_OWNER_ID, value.coordinator().ownerId());
        request.put(
                FIELD_RELOCATION_COORDINATOR_LEASE_GENERATION,
                unsigned(value.coordinator().leaseGeneration()));
        request.put(FIELD_RELOCATION_COORDINATOR_NODE_RID, base64(value.coordinator().nodeRid()));
        request.put(
                FIELD_RELOCATION_COORDINATOR_NODE_GENERATION,
                unsigned(value.coordinator().nodeGeneration()));
        request.put(
                FIELD_RELOCATION_COORDINATOR_EXPECTED_AUTHORITY_STORE_VERSION,
                value.coordinator().expectedAuthorityStoreVersion());
        request.put(FIELD_ACTOR_NODE_GENERATION, unsigned(value.actorNodeGeneration()));
        request.put(
                FIELD_EXPECTED_OWNER_LEASE_GENERATION,
                unsigned(value.expectedOwnerLeaseGeneration()));

        Map<String, Object> metadata = new LinkedHashMap<>();
        metadata.put(FIELD_REQUEST, request);
        metadata.put(FIELD_TARGET_SPOT_ID, value.targetSpotId());
        metadata.put(FIELD_TARGET_NODE_RID, base64(value.targetNodeRid()));
        metadata.put(FIELD_TARGET_NODE_GENERATION, unsigned(value.targetNodeGeneration()));
        metadata.put(FIELD_TARGET_SPOT_GENERATION, unsigned(value.targetSpotGeneration()));
        metadata.put(
                FIELD_TARGET_AUTHORITY_OWNER_GENERATION,
                unsigned(value.targetAuthorityOwnerGeneration()));
        metadata.put(FIELD_OPERATION_ID_HIGH, unsigned(value.operationId().high()));
        metadata.put(FIELD_OPERATION_ID_LOW, unsigned(value.operationId().low()));
        metadata.put(FIELD_REPLY_CONTENT_TYPE, value.replyContentType());
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
                || coordinator.leaseGeneration() != value.expectedOwnerLeaseGeneration()) {
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

    private static IllegalArgumentException invalid(String message) {
        return new IllegalArgumentException(message);
    }

    private static IllegalArgumentException invalid(String message, Throwable failure) {
        return new IllegalArgumentException(message, failure);
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

        @Override
        public byte[] request() {
            return request.clone();
        }

        @Override
        public byte[] reply() {
            return reply.clone();
        }
    }
}
