package systems.zlink.framework.runtime.messaging;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import com.fasterxml.jackson.databind.node.ObjectNode;
import java.util.Iterator;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

/**
 * Shared cross-language channel/SPOT-route wire envelope: a two-part frame of
 * {@code [JSON header, body]} with {@code formatMarker} 0xF2. The header field
 * names, message kinds and error code names match the canonical C++
 * implementation ({@code runtime/messaging/envelope_codec.cpp},
 * {@code runtime/channels/channel_reply_writer.cpp}) and the Node encoder
 * ({@code runtime/channels/channel-envelope.ts}) byte-for-byte in JSON
 * semantics, so Java requests/replies interoperate with the other language
 * frameworks on SPOT route and route mesh paths.
 */
public final class ZLinkChannelEnvelope {
    public static final int FORMAT_MARKER = 0xF2;

    public static final int KIND_REQUEST = 1;
    public static final int KIND_RESPONSE = 2;
    public static final int KIND_COMMAND = 3;
    public static final int KIND_PUBLISH = 4;
    public static final int KIND_ERROR = 5;

    public static final String DEFAULT_CONTENT_TYPE = "application/json";

    private static final ObjectMapper JSON = new ObjectMapper();

    private ZLinkChannelEnvelope() {
    }

    /**
     * Decoded/encoded envelope header. {@code correlationId}, {@code deadline},
     * {@code topic}, {@code errorCode}, {@code errorMessage}, {@code source},
     * {@code flowId} and {@code flowOrigin} are nullable; {@code metadata} is
     * never null.
     */
    public record Header(
        int kind,
        String channelName,
        String messageName,
        String contentType,
        String correlationId,
        String deadline,
        String topic,
        String errorCode,
        String errorMessage,
        String source,
        Map<String, String> metadata,
        String flowId,
        ZLinkFlowOrigin flowOrigin) {

        public Header {
            channelName = channelName == null ? "" : channelName;
            messageName = messageName == null ? "" : messageName;
            contentType = contentType == null || contentType.isEmpty()
                ? DEFAULT_CONTENT_TYPE
                : contentType;
            metadata = metadata == null || metadata.isEmpty()
                ? Map.of()
                : Map.copyOf(metadata);
        }

        public boolean isError() {
            return kind == KIND_ERROR;
        }
    }

    public static String newCorrelationId() {
        return UUID.randomUUID().toString().replace("-", "");
    }

    /** Outbound request/command/publish header with an explicit flow value. */
    public static Header create(
        int kind,
        String channelName,
        String messageName,
        String contentType,
        String topic,
        Map<String, String> metadata,
        ZLinkFlowContext.State flowState) {
        return new Header(
            kind,
            channelName,
            messageName,
            contentType,
            kind == KIND_REQUEST ? newCorrelationId() : null,
            null,
            topic,
            null,
            null,
            null,
            metadata,
            flowState == null ? null : flowState.flowId(),
            flowState == null ? null : flowState.origin());
    }

    /** Normal reply header (kind 2) echoing the request identifiers. */
    public static Header reply(Header request) {
        return new Header(
            KIND_RESPONSE,
            request.channelName(),
            request.messageName(),
            request.contentType(),
            request.correlationId(),
            null,
            null,
            null,
            null,
            null,
            Map.of(),
            request.flowId(),
            request.flowOrigin());
    }

    /**
     * Error reply header (kind 5). The {@code errorCode} carries the
     * snake_case error kind name from the canonical C++ table; framework-origin
     * and failure-origin markers travel in the header metadata object.
     */
    public static Header error(
        Header request,
        ZLinkFrameworkErrorKind kind,
        String message,
        Map<String, String> metadata) {
        ZLinkFrameworkErrorKind effective =
            kind == null ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE : kind;
        return new Header(
            KIND_ERROR,
            request == null ? "" : request.channelName(),
            request == null ? "" : request.messageName(),
            DEFAULT_CONTENT_TYPE,
            request == null ? null : request.correlationId(),
            null,
            null,
            errorCodeName(effective),
            message == null ? "" : message,
            null,
            metadata,
            request == null ? null : request.flowId(),
            request == null ? null : request.flowOrigin());
    }

    public static Message encodeHeader(Header header) {
        validateFlowPair(header.flowId(), header.flowOrigin());
        ObjectNode json = JSON.createObjectNode();
        json.put("formatMarker", FORMAT_MARKER);
        if (header.flowId() == null) {
            json.putNull("flowId");
            json.putNull("flowOrigin");
        } else {
            json.put("flowId", header.flowId());
            json.put("flowOrigin", flowOriginWireValue(header.flowOrigin()));
        }
        json.put("kind", header.kind());
        json.put("channelName", header.channelName());
        json.put("messageName", header.messageName());
        json.put("contentType", header.contentType());
        putNullable(json, "correlationId", header.correlationId());
        putNullable(json, "deadline", header.deadline());
        putNullable(json, "topic", header.topic());
        putNullable(json, "errorCode", header.errorCode());
        putNullable(json, "errorMessage", header.errorMessage());
        putNullable(json, "source", header.source());
        ObjectNode metadata = json.putObject("metadata");
        header.metadata().forEach(metadata::put);
        try {
            return Message.from(JSON.writeValueAsBytes(json));
        } catch (com.fasterxml.jackson.core.JsonProcessingException ex) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                "ZLink envelope header could not be encoded",
                ex);
        }
    }

    /** Encodes {@code [header, body]}; the body message is not copied. */
    public static List<Message> encode(Header header, Message body) {
        return List.of(encodeHeader(header), body);
    }

    /**
     * Strict header decode with C++-equivalent semantics: JSON parse failure,
     * a missing/mismatched {@code formatMarker} or malformed flow fields are
     * {@link ZLinkFrameworkErrorKind#PROTOCOL_ERROR}. Flow fields are read and
     * validated only when {@code captureFlow} is set (spec 27 §4).
     */
    public static Header decodeHeader(Message headerPart, boolean captureFlow) {
        JsonNode json;
        try {
            json = JSON.readTree(headerPart.toByteArray());
        } catch (Exception ex) {
            throw protocolError("invalid ZLink envelope header: " + ex.getMessage(), ex);
        }
        if (json == null || !json.isObject()) {
            throw protocolError("ZLink envelope header must be a JSON object", null);
        }
        int marker = json.hasNonNull("formatMarker") && json.get("formatMarker").isInt()
            ? json.get("formatMarker").asInt()
            : 0;
        if (marker != FORMAT_MARKER) {
            throw protocolError("ZLink envelope format marker is invalid", null);
        }
        if (!json.hasNonNull("kind") || !json.get("kind").isInt()) {
            throw protocolError("ZLink envelope kind is missing", null);
        }
        int kind = json.get("kind").asInt();
        String channelName = requiredString(json, "channelName");
        String messageName = requiredString(json, "messageName");
        String contentType = optionalString(json, "contentType");
        Map<String, String> metadata = decodeMetadata(json.get("metadata"));
        String flowId = null;
        ZLinkFlowOrigin flowOrigin = null;
        if (captureFlow) {
            flowId = optionalString(json, "flowId");
            JsonNode originNode = json.get("flowOrigin");
            if (originNode != null && !originNode.isNull()) {
                if (!originNode.isInt()) {
                    throw protocolError("ZLink envelope flow origin is invalid", null);
                }
                flowOrigin = flowOriginFromWire(originNode.asInt());
            }
            if ((flowId == null) != (flowOrigin == null)) {
                throw protocolError(
                    "ZLink envelope flow id and origin must be present together", null);
            }
            if (flowId != null && !ZLinkFlowContext.isValidFlowId(flowId)) {
                throw protocolError("ZLink envelope flow id must be UUIDv7", null);
            }
        }
        return new Header(
            kind,
            channelName,
            messageName,
            contentType,
            optionalString(json, "correlationId"),
            optionalString(json, "deadline"),
            optionalString(json, "topic"),
            optionalString(json, "errorCode"),
            optionalString(json, "errorMessage"),
            optionalString(json, "source"),
            metadata,
            flowId,
            flowOrigin);
    }

    /**
     * Lenient envelope probe for reply/branch points: returns {@code null}
     * unless the parts are a well-formed two-part envelope. Never throws, so a
     * raw single-part JSON payload is not mistaken for a corrupt envelope.
     */
    public static Header tryDecodeHeader(List<Message> parts, boolean captureFlow) {
        if (!looksLikeEnvelope(parts)) {
            return null;
        }
        try {
            return decodeHeader(parts.get(0), captureFlow);
        } catch (RuntimeException ignored) {
            return null;
        }
    }

    /**
     * Dispatch-side decode: {@code null} for legacy/internal raw parts (first
     * part is not a JSON object); a JSON-object first part that fails strict
     * envelope validation is a {@code PROTOCOL_ERROR} (task/spec parity with
     * the C++ decoder).
     */
    public static Header decodeDispatchHeader(List<Message> parts, boolean captureFlow) {
        if (!looksLikeEnvelope(parts)) {
            return null;
        }
        return decodeHeader(parts.get(0), captureFlow);
    }

    /** Two or more parts whose first frame starts with a JSON object byte. */
    public static boolean looksLikeEnvelope(List<Message> parts) {
        if (parts == null || parts.size() < 2 || parts.get(0).size() == 0) {
            return false;
        }
        byte[] first = parts.get(0).toByteArray();
        for (byte value : first) {
            if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
                continue;
            }
            return value == '{';
        }
        return false;
    }

    /** Envelope body part; {@code PROTOCOL_ERROR} when it is missing. */
    public static Message decodeBody(List<Message> parts) {
        if (parts.size() < 2) {
            throw protocolError("ZLink envelope body part is missing", null);
        }
        return parts.get(1);
    }

    /**
     * Canonical snake_case error code table, 1:1 with C++
     * {@code channel_reply_writer.cpp} {@code error_code_name}.
     */
    public static String errorCodeName(ZLinkFrameworkErrorKind kind) {
        return switch (kind == null ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE : kind) {
            case NOT_FOUND -> "not_found";
            case ALREADY_EXISTS -> "already_exists";
            case TYPE_MISMATCH -> "type_mismatch";
            case NOT_CONFIGURED -> "not_configured";
            case REJECTED -> "rejected";
            case UNAVAILABLE -> "unavailable";
            case CAPACITY_EXCEEDED -> "capacity_exceeded";
            case DEADLINE_EXCEEDED -> "deadline_exceeded";
            case SHUTTING_DOWN -> "shutting_down";
            case PROTOCOL_ERROR -> "protocol_error";
            case INVALID_OPERATION -> "invalid_operation";
            case DATA_LOST -> "data_lost";
            case INTERNAL_FAILURE -> "internal_failure";
        };
    }

    /**
     * Maps a wire {@code errorCode} back to the public kind. Accepts the 13
     * canonical snake_case names and, for interoperability with peers that
     * emit numeric kinds, an integer in {@code 0..12}; anything else is
     * {@code INTERNAL_FAILURE}.
     */
    public static ZLinkFrameworkErrorKind errorKindFromCode(String errorCode) {
        if (errorCode == null || errorCode.isBlank()) {
            return ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
        }
        switch (errorCode) {
            case "not_found": return ZLinkFrameworkErrorKind.NOT_FOUND;
            case "already_exists": return ZLinkFrameworkErrorKind.ALREADY_EXISTS;
            case "type_mismatch": return ZLinkFrameworkErrorKind.TYPE_MISMATCH;
            case "not_configured": return ZLinkFrameworkErrorKind.NOT_CONFIGURED;
            case "rejected": return ZLinkFrameworkErrorKind.REJECTED;
            case "unavailable": return ZLinkFrameworkErrorKind.UNAVAILABLE;
            case "capacity_exceeded": return ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED;
            case "deadline_exceeded": return ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED;
            case "shutting_down": return ZLinkFrameworkErrorKind.SHUTTING_DOWN;
            case "protocol_error": return ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
            case "invalid_operation": return ZLinkFrameworkErrorKind.INVALID_OPERATION;
            case "data_lost": return ZLinkFrameworkErrorKind.DATA_LOST;
            case "internal_failure": return ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
            default:
                break;
        }
        try {
            int value = Integer.parseInt(errorCode.trim());
            if (value >= 0 && value <= 12) {
                return ZLinkFrameworkErrorKind.fromValue(value);
            }
        } catch (NumberFormatException ignored) {
        }
        return ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
    }

    public static int flowOriginWireValue(ZLinkFlowOrigin origin) {
        return switch (origin) {
            case INBOUND -> 1;
            case TIMER -> 2;
            case APPLICATION -> 3;
            case LIFECYCLE -> 4;
        };
    }

    public static ZLinkFlowOrigin flowOriginFromWire(int value) {
        return switch (value) {
            case 1 -> ZLinkFlowOrigin.INBOUND;
            case 2 -> ZLinkFlowOrigin.TIMER;
            case 3 -> ZLinkFlowOrigin.APPLICATION;
            case 4 -> ZLinkFlowOrigin.LIFECYCLE;
            default -> throw protocolError("ZLink envelope flow origin is invalid", null);
        };
    }

    private static void validateFlowPair(String flowId, ZLinkFlowOrigin flowOrigin) {
        if ((flowId == null) != (flowOrigin == null)) {
            throw protocolError(
                "ZLink envelope flow id and origin must be present together", null);
        }
        if (flowId != null && !ZLinkFlowContext.isValidFlowId(flowId)) {
            throw protocolError("ZLink envelope flow id must be UUIDv7", null);
        }
    }

    private static Map<String, String> decodeMetadata(JsonNode node) {
        if (node == null || node.isNull() || !node.isObject() || node.isEmpty()) {
            return Map.of();
        }
        Map<String, String> metadata = new LinkedHashMap<>();
        Iterator<Map.Entry<String, JsonNode>> fields = node.fields();
        while (fields.hasNext()) {
            Map.Entry<String, JsonNode> field = fields.next();
            if (!field.getValue().isTextual()) {
                throw protocolError(
                    "ZLink envelope metadata values must be strings", null);
            }
            metadata.put(field.getKey(), field.getValue().asText());
        }
        return metadata;
    }

    private static String requiredString(JsonNode json, String field) {
        JsonNode node = json.get(field);
        if (node == null || node.isNull() || !node.isTextual()) {
            throw protocolError("ZLink envelope " + field + " must be a string", null);
        }
        return node.asText();
    }

    private static String optionalString(JsonNode json, String field) {
        JsonNode node = json.get(field);
        if (node == null || node.isNull()) {
            return null;
        }
        if (!node.isTextual()) {
            throw protocolError(
                "ZLink envelope " + field + " must be a string or null", null);
        }
        return node.asText();
    }

    private static void putNullable(ObjectNode json, String field, String value) {
        if (value == null) {
            json.putNull(field);
        } else {
            json.put(field, value);
        }
    }

    private static ZLinkFrameworkException protocolError(String message, Throwable cause) {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message, cause);
    }
}
