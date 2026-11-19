package systems.zlink.framework.runtime.messaging;

import com.google.protobuf.ByteString;
import com.google.protobuf.CodedInputStream;
import com.google.protobuf.InvalidProtocolBufferException;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;

import zlink.framework.channel.v1.ChannelHeaderV1.ChannelHeader;
import zlink.framework.channel.v1.ChannelHeaderV1.MetadataEntry;

import java.nio.ByteBuffer;
import java.util.ArrayList;
import java.util.Comparator;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;

/** Generated-protobuf codec and Framework validation for channel envelope headers. */
public final class ZLinkChannelEnvelope {
    public static final int KIND_REQUEST = 1;
    public static final int KIND_RESPONSE = 2;
    public static final int KIND_COMMAND = 3;
    public static final int KIND_PUBLISH = 4;
    public static final int KIND_ERROR = 5;
    public static final String DEFAULT_CONTENT_TYPE = "application/json";
    private static final int MAX_METADATA_BYTES = 1024;
    // Native header parts are direct buffers. protobuf-java parses those by duplicating the
    // buffer into a fresh byte[] per call when its Unsafe decoder is unavailable, so the decoder
    // copies the header once into this per-thread array instead. Headers within the fixed fields
    // plus MAX_METADATA_BYTES fit; a larger one uses its own array and is not retained.
    private static final int RETAINED_HEADER_SCRATCH_BYTES = 4096;
    private static final ThreadLocal<byte[]> HEADER_SCRATCH =
            ThreadLocal.withInitial(() -> new byte[512]);
    private static final Comparator<String> UTF8_ORDER =
            (left, right) -> {
                int leftIndex = 0;
                int rightIndex = 0;
                while (leftIndex < left.length() && rightIndex < right.length()) {
                    int leftCodePoint = left.codePointAt(leftIndex);
                    int rightCodePoint = right.codePointAt(rightIndex);
                    if (leftCodePoint != rightCodePoint)
                        return Integer.compare(leftCodePoint, rightCodePoint);
                    leftIndex += Character.charCount(leftCodePoint);
                    rightIndex += Character.charCount(rightCodePoint);
                }
                return Integer.compare(left.length() - leftIndex, right.length() - rightIndex);
            };

    private ZLinkChannelEnvelope() {}

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
            contentType = contentType == null ? DEFAULT_CONTENT_TYPE : contentType;
            metadata = metadata == null || metadata.isEmpty() ? Map.of() : Map.copyOf(metadata);
        }

        public boolean canCorrelateReply() {
            if (correlationId == null || correlationId.isEmpty() || correlationId.length() > 64)
                return false;
            for (int i = 0; i < correlationId.length(); i++)
                if (correlationId.charAt(i) > 0x7f) return false;
            return true;
        }

        public boolean isError() {
            return kind == KIND_ERROR;
        }
    }

    /** Keeps prepared UTF-8 protobuf values for the channel header writer. */
    public static final class PreparedNames {
        private final String channelName;
        private final String messageName;
        private final String contentType;
        private final ByteString channelNameBytes;
        private final ByteString messageNameBytes;
        private final ByteString contentTypeBytes;

        public PreparedNames(String channelName, String messageName, String contentType) {
            this.channelName = channelName;
            this.messageName = messageName;
            this.contentType = contentType;
            channelNameBytes = encodeString(channelName);
            messageNameBytes = encodeString(messageName);
            contentTypeBytes = encodeString(contentType);
        }

        public PreparedNames(String messageName, PreparedNames contentNames) {
            this(null, messageName, contentNames.contentType);
        }

        @Override
        public boolean equals(Object other) {
            return other instanceof PreparedNames p
                    && Objects.equals(channelName, p.channelName)
                    && Objects.equals(messageName, p.messageName)
                    && Objects.equals(contentType, p.contentType);
        }

        @Override
        public int hashCode() {
            return Objects.hash(channelName, messageName, contentType);
        }
    }

    public static String newCorrelationId() {
        return ZLinkServiceOperationIds.correlationId(ZLinkServiceOperationIds.next());
    }

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

    public static Header error(
            Header request,
            ZLinkFrameworkErrorKind kind,
            String message,
            Map<String, String> metadata) {
        var effective = kind == null ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE : kind;
        return new Header(
                KIND_ERROR,
                request == null || request.channelName().isEmpty()
                        ? "unknown"
                        : request.channelName(),
                request == null || request.messageName().isEmpty()
                        ? "unknown"
                        : request.messageName(),
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
        return Message.from(encodeHeaderBytes(header));
    }

    public static byte[] encodeHeaderBytes(Header header) {
        return encodeHeaderBytesInternal(header, null, null, null);
    }

    public static byte[] encodeHeaderBytes(Header header, PreparedNames names) {
        return encodeHeaderBytesInternal(header, names, names, names);
    }

    private static byte[] encodeHeaderBytesInternal(
            Header header,
            PreparedNames channelNames,
            PreparedNames names,
            PreparedNames contentNames) {
        try {
            return toProto(header, channelNames, names, contentNames).toByteArray();
        } catch (RuntimeException failure) {
            throw protocolError("invalid ZLink envelope header: " + failure.getMessage(), failure);
        }
    }

    private static ChannelHeader toProto(
            Header header,
            PreparedNames channelNames,
            PreparedNames names,
            PreparedNames contentNames) {
        validateFlowPair(header.flowId(), header.flowOrigin());
        validateMetadata(header.metadata());
        validateEncodingStrings(header);
        var builder = ChannelHeader.newBuilder().setKindValue(header.kind());
        if (channelNames != null && Objects.equals(channelNames.channelName, header.channelName()))
            builder.setChannelNameBytes(channelNames.channelNameBytes);
        else builder.setChannelName(header.channelName());
        if (names != null && Objects.equals(names.messageName, header.messageName()))
            builder.setMessageNameBytes(names.messageNameBytes);
        else builder.setMessageName(header.messageName());
        if (contentNames != null && Objects.equals(contentNames.contentType, header.contentType()))
            builder.setContentTypeBytes(contentNames.contentTypeBytes);
        else builder.setContentType(header.contentType());
        if (header.correlationId() != null) builder.setCorrelationId(header.correlationId());
        if (header.deadline() != null) builder.setDeadline(header.deadline());
        if (header.errorCode() != null) builder.setErrorCode(header.errorCode());
        if (header.errorMessage() != null) builder.setErrorMessage(header.errorMessage());
        if (header.flowId() != null) builder.setFlowId(header.flowId());
        if (header.flowOrigin() != null)
            builder.setFlowOriginValue(flowOriginWireValue(header.flowOrigin()));
        if (header.source() != null) builder.setSource(header.source());
        if (header.topic() != null) builder.setTopic(header.topic());
        int metadataBytes = 0;
        if (!header.metadata().isEmpty()) {
            var entries = new ArrayList<>(header.metadata().entrySet());
            entries.sort(Map.Entry.comparingByKey(UTF8_ORDER));
            for (var entry : entries) {
                MetadataEntry metadata =
                        MetadataEntry.newBuilder()
                                .setKey(entry.getKey())
                                .setValue(entry.getValue())
                                .build();
                metadataBytes +=
                        com.google.protobuf.CodedOutputStream.computeMessageSize(13, metadata);
                builder.addMetadata(metadata);
            }
        }
        ChannelHeader encoded = builder.build();
        if (metadataBytes > MAX_METADATA_BYTES)
            throw protocolError("ZLink envelope metadata exceeds 1024 encoded bytes", null);
        return encoded;
    }

    public static List<Message> encode(Header header, Message body) {
        return ZLinkFrameworkMultipartView.encoded(encodeHeaderBytes(header), body);
    }

    public static List<Message> encode(Header header, ZLinkFrameworkMultipartView body) {
        return ZLinkFrameworkMultipartView.encoded(encodeHeaderBytes(header), body);
    }

    public static List<Message> encode(
            Header header,
            ZLinkFrameworkMultipartView body,
            PreparedNames channelNames,
            PreparedNames names,
            PreparedNames contentNames) {
        return ZLinkFrameworkMultipartView.encoded(
                encodeHeaderBytesInternal(header, channelNames, names, contentNames), body);
    }

    public static List<Message> encode(
            Header header,
            Message body,
            PreparedNames channelNames,
            PreparedNames names,
            PreparedNames contentNames) {
        return ZLinkFrameworkMultipartView.encoded(
                encodeHeaderBytesInternal(header, channelNames, names, contentNames), body);
    }

    private static ByteString encodeString(String value) {
        return value == null ? null : ByteString.copyFromUtf8(value);
    }

    public static Header decodeHeader(Message part, boolean captureFlow) {
        return decodeHeader(part.dataBuffer(), captureFlow);
    }

    public static Header decodeHeader(ByteBuffer bytes, boolean captureFlow) {
        int start = bytes == null ? 0 : bytes.position();
        ChannelHeader decoded;
        try {
            CodedInputStream input = headerInput(bytes, start);
            decoded = ChannelHeader.parser().parseFrom(input);
            input.checkLastTagWas(0);
        } catch (InvalidProtocolBufferException | RuntimeException failure) {
            if (bytes != null) bytes.position(start);
            if (failure instanceof ZLinkFrameworkException frameworkError) throw frameworkError;
            throw protocolError("invalid ZLink envelope header: " + failure.getMessage(), failure);
        }
        try {
            Header header = fromProto(decoded, captureFlow);
            if (bytes != null) bytes.position(bytes.limit());
            return header;
        } catch (ZLinkFrameworkException failure) {
            if (bytes != null) bytes.position(start);
            throw new HeaderDecodeException(correlationCandidate(decoded), failure);
        } catch (RuntimeException failure) {
            if (bytes != null) bytes.position(start);
            throw new HeaderDecodeException(
                    correlationCandidate(decoded),
                    protocolError(
                            "invalid ZLink envelope header: " + failure.getMessage(), failure));
        }
    }

    private static CodedInputStream headerInput(ByteBuffer bytes, int start) {
        int length = bytes.remaining();
        if (bytes.hasArray())
            return CodedInputStream.newInstance(bytes.array(), bytes.arrayOffset() + start, length);
        byte[] scratch = HEADER_SCRATCH.get();
        if (scratch.length < length) {
            scratch = new byte[length];
            if (length <= RETAINED_HEADER_SCRATCH_BYTES) HEADER_SCRATCH.set(scratch);
        }
        bytes.get(start, scratch, 0, length);
        return CodedInputStream.newInstance(scratch, 0, length);
    }

    private static Header fromProto(ChannelHeader proto, boolean captureFlow) {
        Map<String, String> metadata = new HashMap<>();
        for (MetadataEntry entry : proto.getMetadataList()) {
            if (metadata.putIfAbsent(entry.getKey(), entry.getValue()) != null)
                throw protocolError("duplicate envelope metadata key", null);
        }
        boolean hasFlowId = captureFlow && proto.hasFlowId();
        boolean hasFlowOrigin = captureFlow && proto.hasFlowOrigin();
        String flowId = hasFlowId ? proto.getFlowId() : null;
        ZLinkFlowOrigin origin =
                hasFlowOrigin ? flowOriginFromWire(proto.getFlowOriginValue()) : null;
        Header header =
                new Header(
                        proto.getKindValue(),
                        proto.getChannelName(),
                        proto.getMessageName(),
                        proto.getContentType(),
                        proto.hasCorrelationId() ? proto.getCorrelationId() : null,
                        proto.hasDeadline() ? proto.getDeadline() : null,
                        proto.hasTopic() ? proto.getTopic() : null,
                        proto.hasErrorCode() ? proto.getErrorCode() : null,
                        proto.hasErrorMessage() ? proto.getErrorMessage() : null,
                        proto.hasSource() ? proto.getSource() : null,
                        metadata,
                        flowId,
                        origin);
        validateHeader(header, captureFlow, hasFlowId, hasFlowOrigin, metadataSize(proto));
        return header;
    }

    private static void validateHeader(
            Header header,
            boolean captureFlow,
            boolean hasFlowId,
            boolean hasFlowOrigin,
            int encodedMetadataBytes) {
        if (header.kind() < KIND_REQUEST || header.kind() > KIND_ERROR)
            throw protocolError("ZLink envelope kind is invalid", null);
        requireString("channelName", header.channelName());
        requireString("messageName", header.messageName());
        requireString("contentType", header.contentType());
        requireString("correlationId", header.correlationId());
        requireString("deadline", header.deadline());
        requireString("errorCode", header.errorCode());
        requireString("errorMessage", header.errorMessage());
        requireString("source", header.source());
        requireString("topic", header.topic());
        if (header.correlationId() != null && !header.canCorrelateReply())
            throw protocolError("ZLink envelope correlationId must be 1..64 byte ASCII", null);
        validateMetadata(header.metadata());
        if (encodedMetadataBytes > MAX_METADATA_BYTES)
            throw protocolError("ZLink envelope metadata exceeds 1024 encoded bytes", null);
        if (captureFlow) validateFlow(header, hasFlowId, hasFlowOrigin);
    }

    private static void validateMetadata(Map<String, String> metadata) {
        for (var entry : metadata.entrySet()) {
            if (entry.getKey().isEmpty())
                throw protocolError("ZLink envelope metadata key is required", null);
            requireString("metadata key", entry.getKey());
            requireString("metadata value", entry.getValue());
        }
    }

    private static void validateEncodingStrings(Header header) {
        requireEncodableString(header.channelName());
        requireEncodableString(header.messageName());
        requireEncodableString(header.contentType());
        requireEncodableString(header.correlationId());
        requireEncodableString(header.deadline());
        requireEncodableString(header.topic());
        requireEncodableString(header.errorCode());
        requireEncodableString(header.errorMessage());
        requireEncodableString(header.source());
        requireEncodableString(header.flowId());
        for (var entry : header.metadata().entrySet()) {
            requireWellFormedUtf16(entry.getKey());
            requireWellFormedUtf16(entry.getValue());
        }
    }

    private static void requireEncodableString(String value) {
        requireString("header", value);
        requireWellFormedUtf16(value);
    }

    private static void requireWellFormedUtf16(String value) {
        if (value == null) return;
        for (int i = 0; i < value.length(); i++) {
            char c = value.charAt(i);
            if (Character.isHighSurrogate(c)) {
                if (++i >= value.length() || !Character.isLowSurrogate(value.charAt(i)))
                    throw protocolError("ZLink envelope string must be valid UTF-8", null);
            } else if (Character.isLowSurrogate(c)) {
                throw protocolError("ZLink envelope string must be valid UTF-8", null);
            }
        }
    }

    private static void validateFlowPair(String flowId, ZLinkFlowOrigin flowOrigin) {
        if ((flowId == null) != (flowOrigin == null))
            throw protocolError("ZLink envelope flow id and origin must be present together", null);
        if (flowId != null) {
            requireString("flowId", flowId);
            if (!ZLinkFlowContext.isValidFlowId(flowId))
                throw protocolError("ZLink envelope flow id must be UUIDv7", null);
        }
    }

    private static void validateFlow(Header header, boolean hasFlowId, boolean hasFlowOrigin) {
        if (hasFlowId != hasFlowOrigin)
            throw protocolError("ZLink envelope flow id and origin must be present together", null);
        if (hasFlowOrigin && header.flowOrigin() == null)
            throw protocolError("ZLink envelope flow origin is invalid", null);
        if (header.flowId() != null) {
            requireString("flowId", header.flowId());
            if (!ZLinkFlowContext.isValidFlowId(header.flowId()))
                throw protocolError("ZLink envelope flow id must be UUIDv7", null);
        }
    }

    private static void requireString(String field, String value) {
        if (value == null) return;
        if (value.indexOf('\0') >= 0)
            throw protocolError("ZLink envelope " + field + " must not contain NUL", null);
    }

    private static int metadataSize(ChannelHeader header) {
        int size = 0;
        for (MetadataEntry entry : header.getMetadataList())
            size += com.google.protobuf.CodedOutputStream.computeMessageSize(13, entry);
        return size;
    }

    /** Best-effort parse for reply paths that may also receive an unwrapped error body. */
    public static Header tryDecodeHeader(List<Message> parts, boolean captureFlow) {
        if (parts == null || parts.isEmpty()) return null;
        try {
            return decodeHeader(ZLinkFrameworkMultipartView.buffer(parts, 0), captureFlow);
        } catch (ZLinkFrameworkException ignored) {
            return null;
        }
    }

    public static Header decodeDispatchHeader(List<Message> parts, boolean captureFlow) {
        if (parts == null || parts.isEmpty()) {
            throw protocolError("ZLink envelope header is missing", null);
        }
        try {
            return decodeHeader(ZLinkFrameworkMultipartView.buffer(parts, 0), captureFlow);
        } catch (ZLinkFrameworkException failure) {
            if (failure instanceof HeaderDecodeException decodedFailure) throw decodedFailure;
            throw new HeaderDecodeException(null, failure);
        }
    }

    private static Header correlationCandidate(ChannelHeader proto) {
        String correlationId = proto.hasCorrelationId() ? proto.getCorrelationId() : null;
        var candidate =
                new Header(
                        proto.getKindValue(),
                        proto.getChannelName(),
                        proto.getMessageName(),
                        proto.getContentType(),
                        correlationId,
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of(),
                        null,
                        null);
        return candidate.canCorrelateReply() ? candidate : null;
    }

    public record DispatchPacket(
            String packetName, List<Message> parts, int payloadIndex, Header header) {
        public Message payload() {
            return parts.get(payloadIndex);
        }
    }

    public static DispatchPacket decodeDispatchPacket(List<Message> parts, Header decodedHeader) {
        Header header = decodedHeader == null ? decodeDispatchHeader(parts, false) : decodedHeader;
        return new DispatchPacket(header.messageName(), parts, bodyIndex(parts), header);
    }

    private static int bodyIndex(List<Message> parts) {
        if (parts.size() < 2) throw protocolError("ZLink envelope body part is missing", null);
        return 1;
    }

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

    public static ZLinkFrameworkErrorKind errorKindFromCode(String code) {
        if (code == null) return ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
        return switch (code) {
            case "not_found" -> ZLinkFrameworkErrorKind.NOT_FOUND;
            case "already_exists" -> ZLinkFrameworkErrorKind.ALREADY_EXISTS;
            case "type_mismatch" -> ZLinkFrameworkErrorKind.TYPE_MISMATCH;
            case "not_configured" -> ZLinkFrameworkErrorKind.NOT_CONFIGURED;
            case "rejected" -> ZLinkFrameworkErrorKind.REJECTED;
            case "unavailable" -> ZLinkFrameworkErrorKind.UNAVAILABLE;
            case "deadline_exceeded" -> ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED;
            case "shutting_down" -> ZLinkFrameworkErrorKind.SHUTTING_DOWN;
            case "protocol_error" -> ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
            case "invalid_operation" -> ZLinkFrameworkErrorKind.INVALID_OPERATION;
            case "data_lost" -> ZLinkFrameworkErrorKind.DATA_LOST;
            case "internal_failure" -> ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
            default -> ZLinkFrameworkErrorKind.PROTOCOL_ERROR;
        };
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

    private static ZLinkFrameworkException protocolError(String message, Throwable cause) {
        return new ZLinkFrameworkException(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, message, cause);
    }

    public static final class HeaderDecodeException extends ZLinkFrameworkException {
        private final Header header;

        private HeaderDecodeException(Header header, Exception cause) {
            super(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, cause.getMessage(), cause);
            this.header = header;
        }

        public Header header() {
            return header;
        }
    }
}
