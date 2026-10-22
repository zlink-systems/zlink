package systems.zlink.framework.runtime.messaging;

import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.core.util.JsonRecyclerPools;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.io.IOException;
import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;

/**
 * Shared cross-language channel/SPOT-route wire envelope: a two-part frame of {@code [JSON header,
 * body]} with {@code formatMarker} 0xF2. The header field names, message kinds and error code names
 * match the canonical C++ implementation ({@code runtime/messaging/envelope_codec.cpp}, {@code
 * runtime/channels/channel_reply_writer.cpp}) and the Node encoder ({@code
 * runtime/channels/channel-envelope.ts}) byte-for-byte in JSON semantics, so Java requests/replies
 * interoperate with the other language frameworks on SPOT route and route mesh paths.
 */
public final class ZLinkChannelEnvelope {
    public static final int FORMAT_MARKER = 0xF2;

    public static final int KIND_REQUEST = 1;
    public static final int KIND_RESPONSE = 2;
    public static final int KIND_COMMAND = 3;
    public static final int KIND_PUBLISH = 4;
    public static final int KIND_ERROR = 5;

    public static final String DEFAULT_CONTENT_TYPE = "application/json";

    private static final JsonFactory JSON =
            JsonFactory.builder().recyclerPool(JsonRecyclerPools.sharedBoundedPool()).build();
    private static final int CORRELATION_ID_MAX_BYTES = 64;
    private static final int CORRELATION_ID_MAX_CHARACTER = 0x7f;

    private ZLinkChannelEnvelope() {}

    /**
     * Decoded/encoded envelope header. {@code correlationId}, {@code deadline}, {@code topic},
     * {@code errorCode}, {@code errorMessage}, {@code source}, {@code flowId} and {@code
     * flowOrigin} are nullable; {@code metadata} is never null.
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
            contentType =
                    contentType == null || contentType.isEmpty()
                            ? DEFAULT_CONTENT_TYPE
                            : contentType;
            metadata = metadata == null || metadata.isEmpty() ? Map.of() : Map.copyOf(metadata);
        }

        public boolean canCorrelateReply() {
            if (correlationId == null
                    || correlationId.isEmpty()
                    || correlationId.length() > CORRELATION_ID_MAX_BYTES) return false;
            for (int index = 0; index < correlationId.length(); index++) {
                if (correlationId.charAt(index) > CORRELATION_ID_MAX_CHARACTER) return false;
            }
            return true;
        }

        public boolean isError() {
            return kind == KIND_ERROR;
        }
    }

    public static String newCorrelationId() {
        return ZLinkServiceOperationIds.correlationId(ZLinkServiceOperationIds.next());
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
        return create(
                kind,
                channelName,
                messageName,
                contentType,
                topic,
                metadata,
                flowState,
                kind == KIND_REQUEST ? ZLinkServiceOperationIds.next() : null);
    }

    /** Internal request identity overload for the operation owner. */
    public static Header create(
            int kind,
            String channelName,
            String messageName,
            String contentType,
            String topic,
            Map<String, String> metadata,
            ZLinkFlowContext.State flowState,
            UUID operationId) {
        return new Header(
                kind,
                channelName,
                messageName,
                contentType,
                kind == KIND_REQUEST ? ZLinkServiceOperationIds.correlationId(operationId) : null,
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
     * Error reply header (kind 5). The {@code errorCode} carries the snake_case error kind name
     * from the canonical C++ table; framework-origin and failure-origin markers travel in the
     * header metadata object.
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

    private static long writeMetadata(Map<String, String> metadata, ByteBuffer json) {
        if (metadata.isEmpty()) return ascii(json, "{}");
        long size = 2;
        ascii(json, "{");
        try {
            for (Map.Entry<String, String> entry : metadata.entrySet()) {
                String key = entry.getKey();
                String value = entry.getValue();
                if (key == null || key.isEmpty() || value == null) {
                    throw protocolError(
                            "ZLink envelope metadata requires nonempty keys and string values",
                            null);
                }
                if (key.indexOf('\0') >= 0 || value.indexOf('\0') >= 0) {
                    throw protocolError("ZLink envelope metadata must not contain NUL", null);
                }
                if (size > 2) {
                    size++;
                    ascii(json, ",");
                }
                size += jsonString(json, key, true) + 1;
                ascii(json, ":");
                size += jsonString(json, value, true);
            }
        } catch (IOException ex) {
            throw protocolError("ZLink envelope metadata must be valid UTF-8", ex);
        }
        ascii(json, "}");
        if (size > ServiceWireConstants.METADATA_BYTES) {
            throw protocolError(
                    "ZLink envelope metadata exceeds "
                            + ServiceWireConstants.METADATA_BYTES
                            + " UTF-8 bytes",
                    null);
        }
        return size;
    }

    private static int ascii(ByteBuffer json, String text) {
        if (json != null) {
            for (int index = 0; index < text.length(); index++) {
                json.put((byte) text.charAt(index));
            }
        }
        return text.length();
    }

    private static long jsonString(ByteBuffer json, String value, boolean metadata)
            throws IOException {
        if (value == null) return ascii(json, "null");
        long size = 2;
        ascii(json, "\"");
        for (int index = 0; index < value.length(); index++) {
            char c = value.charAt(index);
            String escape =
                    switch (c) {
                        case '"' -> "\\\"";
                        case '\\' -> "\\\\";
                        case '\b' -> "\\b";
                        case '\f' -> "\\f";
                        case '\n' -> "\\n";
                        case '\r' -> "\\r";
                        case '\t' -> "\\t";
                        default -> null;
                    };
            if (escape != null) {
                size += ascii(json, escape);
            } else if (c < 0x20) {
                size += unicodeEscape(json, c, metadata);
            } else {
                int codePoint = c;
                if (Character.isSurrogate(c)) {
                    // UTF8JsonGenerator escapes each UTF-16 surrogate in header fields.
                    if (!metadata) {
                        size += unicodeEscape(json, c, false);
                        continue;
                    }
                    if (!Character.isHighSurrogate(c)
                            || index + 1 == value.length()
                            || !Character.isLowSurrogate(value.charAt(index + 1))) {
                        throw new IOException("ZLink envelope string must be valid UTF-8");
                    }
                    char low = value.charAt(++index);
                    codePoint = Character.toCodePoint(c, low);
                }
                int length =
                        codePoint < 0x80 ? 1 : codePoint < 0x800 ? 2 : codePoint < 0x10000 ? 3 : 4;
                size += length;
                if (json != null) {
                    if (length == 1) {
                        json.put((byte) codePoint);
                    } else {
                        json.put(
                                (byte)
                                        ((0xff << (8 - length))
                                                | (codePoint >>> (6 * (length - 1)))));
                        for (int shift = 6 * (length - 2); shift >= 0; shift -= 6) {
                            json.put((byte) (0x80 | ((codePoint >>> shift) & 0x3f)));
                        }
                    }
                }
            }
        }
        ascii(json, "\"");
        return size;
    }

    private static int unicodeEscape(ByteBuffer json, char value, boolean metadata) {
        ascii(json, "\\u");
        if (json != null) {
            for (int shift = Character.SIZE - 4; shift >= 0; shift -= 4) {
                char digit = Character.forDigit((value >>> shift) & 0xf, 16);
                json.put((byte) (metadata ? digit : Character.toUpperCase(digit)));
            }
        }
        return 6;
    }

    private static int jsonInt(ByteBuffer json, int value) {
        long magnitude = Math.abs((long) value);
        int digits = 1;
        for (long rest = magnitude; rest >= 10; rest /= 10) digits++;
        int size = digits + (value < 0 ? 1 : 0);
        if (json != null) {
            int end = json.position() + size;
            for (int index = end - 1; index >= end - digits; index--) {
                json.put(index, (byte) ('0' + magnitude % 10));
                magnitude /= 10;
            }
            if (value < 0) json.put((byte) '-');
            json.position(end);
        }
        return size;
    }

    private static long field(ByteBuffer json, String prefix, String value) throws IOException {
        return ascii(json, prefix) + jsonString(json, value, false);
    }

    private static long writeHeader(Header header, ByteBuffer json) throws IOException {
        long size = ascii(json, "{\"formatMarker\":") + jsonInt(json, FORMAT_MARKER);
        size += field(json, ",\"flowId\":", header.flowId());
        size += ascii(json, ",\"flowOrigin\":");
        size +=
                header.flowId() == null
                        ? ascii(json, "null")
                        : jsonInt(json, flowOriginWireValue(header.flowOrigin()));
        size += ascii(json, ",\"kind\":") + jsonInt(json, header.kind());
        size += field(json, ",\"channelName\":", header.channelName());
        size += field(json, ",\"messageName\":", header.messageName());
        size += field(json, ",\"contentType\":", header.contentType());
        size += field(json, ",\"correlationId\":", header.correlationId());
        size += field(json, ",\"deadline\":", header.deadline());
        size += field(json, ",\"topic\":", header.topic());
        size += field(json, ",\"errorCode\":", header.errorCode());
        size += field(json, ",\"errorMessage\":", header.errorMessage());
        size += field(json, ",\"source\":", header.source());
        size += ascii(json, ",\"metadata\":") + writeMetadata(header.metadata(), json);
        return size + ascii(json, "}");
    }

    public static Message encodeHeader(Header header) {
        return Message.from(encodeHeaderBytes(header));
    }

    /** Encodes the JSON header into heap storage, sized by a first counting pass. */
    public static byte[] encodeHeaderBytes(Header header) {
        validateFlowPair(header.flowId(), header.flowOrigin());
        try {
            long size = writeHeader(header, null);
            if (size > Integer.MAX_VALUE)
                throw new IOException("ZLink envelope header is too large");
            byte[] encoded = new byte[(int) size];
            writeHeader(header, ByteBuffer.wrap(encoded));
            return encoded;
        } catch (IOException ex) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                    "ZLink envelope header could not be encoded",
                    ex);
        }
    }

    /**
     * Encodes {@code [header, body]}; the body message is not copied and the header stays in heap
     * storage until a transport writes or materializes it. Closing the list releases the body.
     */
    public static List<Message> encode(Header header, Message body) {
        return ZLinkFrameworkMultipartView.encoded(encodeHeaderBytes(header), body);
    }

    /**
     * Strict header decode with C++-equivalent semantics: JSON parse failure, a missing/mismatched
     * {@code formatMarker} or malformed flow fields are {@link
     * ZLinkFrameworkErrorKind#PROTOCOL_ERROR}. Flow fields are read and validated only when {@code
     * captureFlow} is set (spec 27 §4).
     */
    public static Header decodeHeader(Message headerPart, boolean captureFlow) {
        return decodeHeader(headerPart.dataBuffer(), captureFlow);
    }

    /**
     * Reads the header bytes in place and consumes the buffer's remaining bytes; the buffer owner
     * must outlive this call.
     */
    public static Header decodeHeader(ByteBuffer headerPart, boolean captureFlow) {
        try (JsonParser json = JSON.createParser(new BufferInputStream(headerPart))) {
            return decodeHeader(json, captureFlow);
        } catch (Exception ex) {
            if (ex instanceof ZLinkFrameworkException frameworkError) {
                throw frameworkError;
            }
            throw protocolError("invalid ZLink envelope header: " + ex.getMessage(), ex);
        }
    }

    /**
     * Lenient envelope probe for reply/branch points: returns {@code null} unless the parts are a
     * well-formed two-part envelope. Never throws, so a raw single-part JSON payload is not
     * mistaken for a corrupt envelope.
     */
    public static Header tryDecodeHeader(List<Message> parts, boolean captureFlow) {
        if (!looksLikeEnvelope(parts)) {
            return null;
        }
        try {
            return decodeHeader(ZLinkFrameworkMultipartView.buffer(parts, 0), captureFlow);
        } catch (RuntimeException ignored) {
            return null;
        }
    }

    /**
     * Dispatch-side decode: {@code null} for legacy/internal raw parts (first part is not a JSON
     * object); a JSON-object first part that fails strict envelope validation is a {@code
     * PROTOCOL_ERROR} (task/spec parity with the C++ decoder).
     */
    public static Header decodeDispatchHeader(List<Message> parts, boolean captureFlow) {
        if (!looksLikeEnvelope(parts)) {
            return null;
        }
        return decodeHeader(ZLinkFrameworkMultipartView.buffer(parts, 0), captureFlow);
    }

    /**
     * Packet name and payload selected from the decoded envelope or an internal raw frame. The
     * payload part is read from the received parts only when a consumer asks for it.
     */
    public record DispatchPacket(
            String packetName, List<Message> parts, int payloadIndex, Header header) {
        public Message payload() {
            return parts.get(payloadIndex);
        }

        /** Borrows the payload bytes while the received parts stay open. */
        public ByteBuffer payloadBuffer() {
            return ZLinkFrameworkMultipartView.buffer(parts, payloadIndex);
        }
    }

    public static DispatchPacket decodeDispatchPacket(List<Message> parts, Header decodedHeader) {
        Header header = decodedHeader == null ? decodeDispatchHeader(parts, false) : decodedHeader;
        if (header != null) {
            return new DispatchPacket(header.messageName(), parts, bodyIndex(parts), header);
        }
        return parts.size() >= 2
                ? new DispatchPacket(parts.get(0).toUtf8String(), parts, 1, null)
                : new DispatchPacket("", parts, 0, null);
    }

    /** Two or more parts whose first frame starts with a JSON object byte. */
    public static boolean looksLikeEnvelope(List<Message> parts) {
        if (parts == null || parts.size() < 2) {
            return false;
        }
        ByteBuffer first = ZLinkFrameworkMultipartView.buffer(parts, 0);
        for (int index = first.position(); index < first.limit(); index++) {
            byte value = first.get(index);
            if (value == ' ' || value == '\t' || value == '\r' || value == '\n') {
                continue;
            }
            return value == '{';
        }
        return false;
    }

    /** Envelope body part index; {@code PROTOCOL_ERROR} when it is missing. */
    private static int bodyIndex(List<Message> parts) {
        if (parts.size() < 2) {
            throw protocolError("ZLink envelope body part is missing", null);
        }
        return 1;
    }

    /**
     * Canonical snake_case error code table, 1:1 with C++ {@code channel_reply_writer.cpp} {@code
     * error_code_name}.
     */
    public static String errorCodeName(ZLinkFrameworkErrorKind kind) {
        return switch (kind == null ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE : kind) {
            case NOT_FOUND -> "not_found";
            case ALREADY_EXISTS -> "already_exists";
            case TYPE_MISMATCH -> "type_mismatch";
            case NOT_CONFIGURED -> "not_configured";
            case REJECTED -> "rejected";
            case UNAVAILABLE -> "unavailable";
            case DEADLINE_EXCEEDED -> "deadline_exceeded";
            case SHUTTING_DOWN -> "shutting_down";
            case PROTOCOL_ERROR -> "protocol_error";
            case INVALID_OPERATION -> "invalid_operation";
            case DATA_LOST -> "data_lost";
            case INTERNAL_FAILURE -> "internal_failure";
        };
    }

    /**
     * Maps a wire {@code errorCode} back to the public kind. Only the 12 canonical snake_case names
     * are valid; an uninterpretable error reply is a protocol error.
     */
    public static ZLinkFrameworkErrorKind errorKindFromCode(String errorCode) {
        if (errorCode == null || errorCode.isBlank()) {
            return ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
        }
        switch (errorCode) {
            case "not_found":
                return ZLinkFrameworkErrorKind.NOT_FOUND;
            case "already_exists":
                return ZLinkFrameworkErrorKind.ALREADY_EXISTS;
            case "type_mismatch":
                return ZLinkFrameworkErrorKind.TYPE_MISMATCH;
            case "not_configured":
                return ZLinkFrameworkErrorKind.NOT_CONFIGURED;
            case "rejected":
                return ZLinkFrameworkErrorKind.REJECTED;
            case "unavailable":
                return ZLinkFrameworkErrorKind.UNAVAILABLE;
            case "deadline_exceeded":
                return ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED;
            case "shutting_down":
                return ZLinkFrameworkErrorKind.SHUTTING_DOWN;
            case "protocol_error":
                return ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
            case "invalid_operation":
                return ZLinkFrameworkErrorKind.INVALID_OPERATION;
            case "data_lost":
                return ZLinkFrameworkErrorKind.DATA_LOST;
            case "internal_failure":
                return ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
            default:
                break;
        }
        return ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
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

    private static Header decodeHeader(JsonParser json, boolean captureFlow) throws IOException {
        if (json.nextToken() != JsonToken.START_OBJECT) {
            throw protocolError("ZLink envelope header must be a JSON object", null);
        }

        int marker = 0;
        int kind = 0;
        boolean kindIsInt = false;
        String channelName = null;
        boolean channelNameIsString = false;
        String messageName = null;
        boolean messageNameIsString = false;
        String contentType = null;
        boolean contentTypeIsStringOrNull = true;
        String correlationId = null;
        boolean correlationIdIsStringOrNull = true;
        String deadline = null;
        boolean deadlineIsStringOrNull = true;
        String topic = null;
        boolean topicIsStringOrNull = true;
        String errorCode = null;
        boolean errorCodeIsStringOrNull = true;
        String errorMessage = null;
        boolean errorMessageIsStringOrNull = true;
        String source = null;
        boolean sourceIsStringOrNull = true;
        Map<String, String> metadata = Map.of();
        boolean metadataIsValid = true;
        String flowId = null;
        boolean flowIdIsStringOrNull = true;
        int flowOriginValue = 0;
        boolean flowOriginIsIntOrNull = true;
        boolean hasFlowOrigin = false;

        try {
            while (json.nextToken() != JsonToken.END_OBJECT) {
                if (json.currentToken() != JsonToken.FIELD_NAME) {
                    throw new IOException("expected an envelope header field name");
                }
                String field = json.currentName();
                JsonToken value = json.nextToken();
                if (value == null) {
                    throw new IOException("unexpected end of envelope header");
                }
                switch (field) {
                    case "formatMarker" -> {
                        marker =
                                value == JsonToken.VALUE_NUMBER_INT
                                                && json.getNumberType() == JsonParser.NumberType.INT
                                        ? json.getIntValue()
                                        : 0;
                        json.skipChildren();
                    }
                    case "kind" -> {
                        kindIsInt =
                                value == JsonToken.VALUE_NUMBER_INT
                                        && json.getNumberType() == JsonParser.NumberType.INT;
                        if (kindIsInt) {
                            kind = json.getIntValue();
                        }
                        json.skipChildren();
                    }
                    case "channelName" -> {
                        channelNameIsString = value == JsonToken.VALUE_STRING;
                        channelName = channelNameIsString ? json.getText() : null;
                        json.skipChildren();
                    }
                    case "messageName" -> {
                        messageNameIsString = value == JsonToken.VALUE_STRING;
                        messageName = messageNameIsString ? json.getText() : null;
                        json.skipChildren();
                    }
                    case "contentType" -> {
                        contentTypeIsStringOrNull = isStringOrNull(value);
                        contentType = value == JsonToken.VALUE_STRING ? json.getText() : null;
                        json.skipChildren();
                    }
                    case "correlationId" -> {
                        correlationIdIsStringOrNull = isStringOrNull(value);
                        correlationId = value == JsonToken.VALUE_STRING ? json.getText() : null;
                        json.skipChildren();
                    }
                    case "deadline" -> {
                        deadlineIsStringOrNull = isStringOrNull(value);
                        deadline = value == JsonToken.VALUE_STRING ? json.getText() : null;
                        json.skipChildren();
                    }
                    case "topic" -> {
                        topicIsStringOrNull = isStringOrNull(value);
                        topic = value == JsonToken.VALUE_STRING ? json.getText() : null;
                        json.skipChildren();
                    }
                    case "errorCode" -> {
                        errorCodeIsStringOrNull = isStringOrNull(value);
                        errorCode = value == JsonToken.VALUE_STRING ? json.getText() : null;
                        json.skipChildren();
                    }
                    case "errorMessage" -> {
                        errorMessageIsStringOrNull = isStringOrNull(value);
                        errorMessage = value == JsonToken.VALUE_STRING ? json.getText() : null;
                        json.skipChildren();
                    }
                    case "source" -> {
                        sourceIsStringOrNull = isStringOrNull(value);
                        source = value == JsonToken.VALUE_STRING ? json.getText() : null;
                        json.skipChildren();
                    }
                    case "metadata" -> {
                        metadata = Map.of();
                        metadataIsValid = true;
                        if (value == JsonToken.START_OBJECT) {
                            Map<String, String> values = null;
                            while (json.nextToken() != JsonToken.END_OBJECT) {
                                if (json.currentToken() != JsonToken.FIELD_NAME) {
                                    throw new IOException(
                                            "expected an envelope metadata field name");
                                }
                                String key = json.currentName();
                                JsonToken metadataValue = json.nextToken();
                                if (metadataValue == null) {
                                    throw new IOException("unexpected end of envelope metadata");
                                }
                                if (values == null) {
                                    values = new LinkedHashMap<>();
                                }
                                values.put(
                                        key,
                                        metadataValue == JsonToken.VALUE_STRING
                                                ? json.getText()
                                                : null);
                                json.skipChildren();
                            }
                            if (values != null) {
                                metadata = values;
                                for (String metadataValue : values.values()) {
                                    if (metadataValue == null) {
                                        metadataIsValid = false;
                                        break;
                                    }
                                }
                            }
                        } else {
                            metadataIsValid = false;
                            json.skipChildren();
                        }
                    }
                    case "flowId" -> {
                        if (captureFlow) {
                            flowIdIsStringOrNull = isStringOrNull(value);
                            flowId = value == JsonToken.VALUE_STRING ? json.getText() : null;
                        }
                        json.skipChildren();
                    }
                    case "flowOrigin" -> {
                        if (captureFlow) {
                            hasFlowOrigin = value != JsonToken.VALUE_NULL;
                            flowOriginIsIntOrNull =
                                    value == JsonToken.VALUE_NULL
                                            || (value == JsonToken.VALUE_NUMBER_INT
                                                    && json.getNumberType()
                                                            == JsonParser.NumberType.INT);
                            if (flowOriginIsIntOrNull && hasFlowOrigin) {
                                flowOriginValue = json.getIntValue();
                            }
                        }
                        json.skipChildren();
                    }
                    default -> json.skipChildren();
                }
            }

            if (json.nextToken() != null) {
                throw new IOException("ZLink envelope header must contain one JSON object");
            }
            if (marker != FORMAT_MARKER) {
                throw protocolError("ZLink envelope format marker is invalid", null);
            }
            if (!kindIsInt || kind < KIND_REQUEST || kind > KIND_ERROR) {
                throw protocolError("ZLink envelope kind is invalid", null);
            }
            if (!channelNameIsString) {
                throw protocolError("ZLink envelope channelName must be a string", null);
            }
            if (!messageNameIsString) {
                throw protocolError("ZLink envelope messageName must be a string", null);
            }
            validateOptionalString("contentType", contentTypeIsStringOrNull);
            validateOptionalString("correlationId", correlationIdIsStringOrNull);
            validateOptionalString("deadline", deadlineIsStringOrNull);
            validateOptionalString("topic", topicIsStringOrNull);
            validateOptionalString("errorCode", errorCodeIsStringOrNull);
            validateOptionalString("errorMessage", errorMessageIsStringOrNull);
            validateOptionalString("source", sourceIsStringOrNull);
            if (!metadataIsValid) {
                throw protocolError("ZLink envelope metadata values must be strings", null);
            }
            writeMetadata(metadata, null);

            ZLinkFlowOrigin flowOrigin = null;
            if (captureFlow) {
                validateOptionalString("flowId", flowIdIsStringOrNull);
                if (!flowOriginIsIntOrNull) {
                    throw protocolError("ZLink envelope flow origin is invalid", null);
                }
                if (hasFlowOrigin) {
                    flowOrigin = flowOriginFromWire(flowOriginValue);
                }
                if ((flowId == null) != (flowOrigin == null)) {
                    throw protocolError(
                            "ZLink envelope flow id and origin must be present together", null);
                }
                if (flowId != null && !ZLinkFlowContext.isValidFlowId(flowId)) {
                    throw protocolError("ZLink envelope flow id must be UUIDv7", null);
                }
            }
            Header header =
                    new Header(
                            kind,
                            channelName,
                            messageName,
                            contentType,
                            correlationId,
                            deadline,
                            topic,
                            errorCode,
                            errorMessage,
                            source,
                            metadata,
                            flowId,
                            flowOrigin);
            if (correlationId != null && !header.canCorrelateReply()) {
                throw protocolError("ZLink envelope correlationId must be 1..64 byte ASCII", null);
            }
            return header;
        } catch (IOException | ZLinkFrameworkException invalidHeader) {
            // Only a complete JSON object can retain reply identifiers. Invalid
            // metadata and flow fields cannot be copied into the error reply.
            Header rejectedHeader =
                    invalidHeader instanceof IOException
                            ? null
                            : new Header(
                                    kind,
                                    channelName,
                                    messageName,
                                    contentType,
                                    correlationId,
                                    null,
                                    null,
                                    null,
                                    null,
                                    null,
                                    Map.of(),
                                    null,
                                    null);
            throw new HeaderDecodeException(rejectedHeader, invalidHeader);
        }
    }

    private static boolean isStringOrNull(JsonToken token) {
        return token == JsonToken.VALUE_STRING || token == JsonToken.VALUE_NULL;
    }

    private static void validateOptionalString(String field, boolean valid) {
        if (!valid) {
            throw protocolError("ZLink envelope " + field + " must be a string or null", null);
        }
    }

    private static void validateFlowPair(String flowId, ZLinkFlowOrigin flowOrigin) {
        if ((flowId == null) != (flowOrigin == null)) {
            throw protocolError("ZLink envelope flow id and origin must be present together", null);
        }
        if (flowId != null && !ZLinkFlowContext.isValidFlowId(flowId)) {
            throw protocolError("ZLink envelope flow id must be UUIDv7", null);
        }
    }

    private static final class BufferInputStream extends InputStream {
        private final ByteBuffer input;

        BufferInputStream(ByteBuffer input) {
            this.input = input;
        }

        @Override
        public int read() {
            return input.hasRemaining() ? input.get() & 0xff : -1;
        }

        @Override
        public int read(byte[] target, int offset, int length) {
            Objects.checkFromIndexSize(offset, length, target.length);
            if (length == 0) {
                return 0;
            }
            if (!input.hasRemaining()) {
                return -1;
            }
            int count = Math.min(length, input.remaining());
            input.get(target, offset, count);
            return count;
        }
    }

    /** Internal decode failure carrying the identifiers read before header rejection. */
    public static final class HeaderDecodeException extends ZLinkFrameworkException {
        private final Header header;

        private HeaderDecodeException(Header header, Exception cause) {
            super(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    cause instanceof IOException
                            ? "invalid ZLink envelope header: " + cause.getMessage()
                            : cause.getMessage(),
                    cause);
            this.header = header;
        }

        public Header header() {
            return header;
        }
    }

    private static ZLinkFrameworkException protocolError(String message, Throwable cause) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message, cause);
    }
}
