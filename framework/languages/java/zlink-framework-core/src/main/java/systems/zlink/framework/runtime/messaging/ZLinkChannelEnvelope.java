package systems.zlink.framework.runtime.messaging;

import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.core.io.SerializedString;
import com.fasterxml.jackson.core.util.JsonRecyclerPools;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.nio.charset.StandardCharsets;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;

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

    // Jackson's default recycler is thread-local. Framework handlers use
    // virtual threads, so use Jackson's own bounded shared pool instead.
    private static final ObjectMapper JSON = new ObjectMapper(JsonFactory.builder()
        .recyclerPool(JsonRecyclerPools.sharedBoundedPool())
        .build());
    private static final int HEADER_INITIAL_CAPACITY = 256;
    private static final int HEADER_WRITER_POOL_CAPACITY =
        Math.max(1, Runtime.getRuntime().availableProcessors());
    private static final ArrayBlockingQueue<HeaderWriter> HEADER_WRITERS =
        new ArrayBlockingQueue<>(HEADER_WRITER_POOL_CAPACITY);
    // Reuse canonical fixed-header tokens; metadata keys remain dynamic and
    // are never cached.
    private static final SerializedString FORMAT_MARKER_FIELD =
        new SerializedString("formatMarker");
    private static final SerializedString FLOW_ID_FIELD = new SerializedString("flowId");
    private static final SerializedString FLOW_ORIGIN_FIELD =
        new SerializedString("flowOrigin");
    private static final SerializedString KIND_FIELD = new SerializedString("kind");
    private static final SerializedString CHANNEL_NAME_FIELD =
        new SerializedString("channelName");
    private static final SerializedString MESSAGE_NAME_FIELD =
        new SerializedString("messageName");
    private static final SerializedString CONTENT_TYPE_FIELD =
        new SerializedString("contentType");
    private static final SerializedString CORRELATION_ID_FIELD =
        new SerializedString("correlationId");
    private static final SerializedString DEADLINE_FIELD = new SerializedString("deadline");
    private static final SerializedString TOPIC_FIELD = new SerializedString("topic");
    private static final SerializedString ERROR_CODE_FIELD =
        new SerializedString("errorCode");
    private static final SerializedString ERROR_MESSAGE_FIELD =
        new SerializedString("errorMessage");
    private static final SerializedString SOURCE_FIELD = new SerializedString("source");
    private static final SerializedString METADATA_FIELD = new SerializedString("metadata");

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
            kind == KIND_REQUEST
                ? ZLinkServiceOperationIds.correlationId(operationId)
                : null,
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
        HeaderWriter writer = borrowWriter();
        boolean complete = false;
        try {
            Message encoded = writer.encode(header);
            complete = true;
            return encoded;
        } catch (IOException ex) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                "ZLink envelope header could not be encoded",
                ex);
        } finally {
            if (complete) {
                HEADER_WRITERS.offer(writer);
            }
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
        try (JsonParser json = JSON.getFactory().createParser(new MessageInputStream(headerPart))) {
            return decodeHeader(json, captureFlow);
        } catch (Exception ex) {
            if (ex instanceof ZLinkFrameworkException frameworkError) {
                throw frameworkError;
            }
            throw protocolError("invalid ZLink envelope header: " + ex.getMessage(), ex);
        }
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
        Message first = parts.get(0);
        for (int index = 0; index < first.size(); index++) {
            byte value = first.readByte(index);
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
            case DEADLINE_EXCEEDED -> "deadline_exceeded";
            case SHUTTING_DOWN -> "shutting_down";
            case PROTOCOL_ERROR -> "protocol_error";
            case INVALID_OPERATION -> "invalid_operation";
            case DATA_LOST -> "data_lost";
            case INTERNAL_FAILURE -> "internal_failure";
        };
    }

    /**
     * Maps a wire {@code errorCode} back to the public kind. Only the 12
     * canonical snake_case names are valid; an uninterpretable error reply is
     * a protocol error.
     */
    public static ZLinkFrameworkErrorKind errorKindFromCode(String errorCode) {
        if (errorCode == null || errorCode.isBlank()) {
            return ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
        }
        switch (errorCode) {
            case "not_found": return ZLinkFrameworkErrorKind.NOT_FOUND;
            case "already_exists": return ZLinkFrameworkErrorKind.ALREADY_EXISTS;
            case "type_mismatch": return ZLinkFrameworkErrorKind.TYPE_MISMATCH;
            case "not_configured": return ZLinkFrameworkErrorKind.NOT_CONFIGURED;
            case "rejected": return ZLinkFrameworkErrorKind.REJECTED;
            case "unavailable": return ZLinkFrameworkErrorKind.UNAVAILABLE;
            case "deadline_exceeded": return ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED;
            case "shutting_down": return ZLinkFrameworkErrorKind.SHUTTING_DOWN;
            case "protocol_error": return ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
            case "invalid_operation": return ZLinkFrameworkErrorKind.INVALID_OPERATION;
            case "data_lost": return ZLinkFrameworkErrorKind.DATA_LOST;
            case "internal_failure": return ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
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
                    marker = value == JsonToken.VALUE_NUMBER_INT
                        && json.getNumberType() == JsonParser.NumberType.INT
                        ? json.getIntValue()
                        : 0;
                    json.skipChildren();
                }
                case "kind" -> {
                    kindIsInt = value == JsonToken.VALUE_NUMBER_INT
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
                                throw new IOException("expected an envelope metadata field name");
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
                                metadataValue == JsonToken.VALUE_STRING ? json.getText() : null);
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
                        flowOriginIsIntOrNull = value == JsonToken.VALUE_NULL
                            || (value == JsonToken.VALUE_NUMBER_INT
                                && json.getNumberType() == JsonParser.NumberType.INT);
                        if (flowOriginIsIntOrNull && hasFlowOrigin) {
                            flowOriginValue = json.getIntValue();
                        }
                    }
                    json.skipChildren();
                }
                default -> json.skipChildren();
            }
        }

        if (marker != FORMAT_MARKER) {
            throw protocolError("ZLink envelope format marker is invalid", null);
        }
        if (!kindIsInt) {
            throw protocolError("ZLink envelope kind is missing", null);
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
        return new Header(
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
    }

    private static boolean isStringOrNull(JsonToken token) {
        return token == JsonToken.VALUE_STRING || token == JsonToken.VALUE_NULL;
    }

    private static void validateOptionalString(String field, boolean valid) {
        if (!valid) {
            throw protocolError(
                "ZLink envelope " + field + " must be a string or null", null);
        }
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

    private static void writeNullableString(
        JsonGenerator json,
        SerializedString field,
        String value) throws IOException {
        json.writeFieldName(field);
        if (value == null) {
            json.writeNull();
        } else {
            json.writeString(value);
        }
    }

    private static HeaderWriter borrowWriter() {
        HeaderWriter writer = HEADER_WRITERS.poll();
        if (writer != null) {
            return writer;
        }
        try {
            return new HeaderWriter();
        } catch (IOException ex) {
            throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                "ZLink envelope header writer could not be initialized",
                ex);
        }
    }

    /** A single borrower owns the generator and its output buffer at a time. */
    private static final class HeaderWriter {
        private final ReusableByteArrayOutputStream bytes =
            new ReusableByteArrayOutputStream(HEADER_INITIAL_CAPACITY);
        private final JsonGenerator json;
        private String channelName;
        private SerializedString channelNameToken;
        private String messageName;
        private SerializedString messageNameToken;
        private String contentType;
        private SerializedString contentTypeToken;
        private int kind;
        private SerializedString kindToken;

        HeaderWriter() throws IOException {
            json = JSON.getFactory().createGenerator(bytes);
            json.setRootValueSeparator(null);
        }

        Message encode(Header header) throws IOException {
            cacheStableValues(header);
            bytes.reset();
            json.writeStartObject();
            json.writeFieldName(FORMAT_MARKER_FIELD);
            json.writeNumber(FORMAT_MARKER);
            writeNullableString(json, FLOW_ID_FIELD, header.flowId());
            json.writeFieldName(FLOW_ORIGIN_FIELD);
            if (header.flowId() == null) {
                json.writeNull();
            } else {
                json.writeNumber(flowOriginWireValue(header.flowOrigin()));
            }
            json.writeFieldName(KIND_FIELD);
            json.writeRawValue(kindToken);
            json.writeFieldName(CHANNEL_NAME_FIELD);
            json.writeRawValue(channelNameToken);
            json.writeFieldName(MESSAGE_NAME_FIELD);
            json.writeRawValue(messageNameToken);
            json.writeFieldName(CONTENT_TYPE_FIELD);
            json.writeRawValue(contentTypeToken);
            writeNullableString(json, CORRELATION_ID_FIELD, header.correlationId());
            writeNullableString(json, DEADLINE_FIELD, header.deadline());
            writeNullableString(json, TOPIC_FIELD, header.topic());
            writeNullableString(json, ERROR_CODE_FIELD, header.errorCode());
            writeNullableString(json, ERROR_MESSAGE_FIELD, header.errorMessage());
            writeNullableString(json, SOURCE_FIELD, header.source());
            json.writeFieldName(METADATA_FIELD);
            json.writeStartObject();
            for (Map.Entry<String, String> entry : header.metadata().entrySet()) {
                json.writeStringField(entry.getKey(), entry.getValue());
            }
            json.writeEndObject();
            json.writeEndObject();
            json.flush();
            return Message.from(bytes.buffer(), 0, bytes.size());
        }

        private void cacheStableValues(Header header) throws IOException {
            if (kindToken == null || kind != header.kind()) {
                kind = header.kind();
                kindToken = new SerializedString(Integer.toString(kind));
            }
            if (!Objects.equals(channelName, header.channelName())) {
                channelName = header.channelName();
                channelNameToken = quotedScalar(channelName);
            }
            if (!Objects.equals(messageName, header.messageName())) {
                messageName = header.messageName();
                messageNameToken = quotedScalar(messageName);
            }
            if (!Objects.equals(contentType, header.contentType())) {
                contentType = header.contentType();
                contentTypeToken = quotedScalar(contentType);
            }
        }

        private SerializedString quotedScalar(String value) throws IOException {
            bytes.reset();
            json.writeString(value);
            json.flush();
            return new SerializedString(bytes.toString(StandardCharsets.UTF_8));
        }
    }

    private static final class ReusableByteArrayOutputStream extends ByteArrayOutputStream {
        ReusableByteArrayOutputStream(int size) {
            super(size);
        }

        byte[] buffer() {
            return buf;
        }
    }

    private static final class MessageInputStream extends InputStream {
        private final Message message;
        private final int size;
        private int position;

        MessageInputStream(Message message) {
            this.message = message;
            size = message.size();
        }

        @Override
        public int read() {
            return position == size ? -1 : message.readByte(position++) & 0xff;
        }

        @Override
        public int read(byte[] target, int offset, int length) {
            Objects.checkFromIndexSize(offset, length, target.length);
            if (length == 0) {
                return 0;
            }
            int remaining = size - position;
            if (remaining == 0) {
                return -1;
            }
            int count = Math.min(length, remaining);
            message.copyTo(target, position, offset, count);
            position += count;
            return count;
        }
    }

    private static ZLinkFrameworkException protocolError(String message, Throwable cause) {
        return new ZLinkFrameworkException(
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message, cause);
    }
}
