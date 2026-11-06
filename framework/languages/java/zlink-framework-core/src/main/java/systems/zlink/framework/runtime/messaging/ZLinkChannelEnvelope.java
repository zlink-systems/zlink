package systems.zlink.framework.runtime.messaging;

import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.JsonParser;
import com.fasterxml.jackson.core.JsonToken;
import com.fasterxml.jackson.core.async.ByteBufferFeeder;
import com.fasterxml.jackson.core.exc.StreamConstraintsException;
import com.fasterxml.jackson.core.util.JsonRecyclerPools;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.internal.execution.ZLinkStateLane;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.internal.service.ZLinkServiceOperationIds;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

import java.io.IOException;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CoderResult;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Objects;
import java.util.UUID;
import java.util.concurrent.ConcurrentHashMap;

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
    // Registered header fields; a field seen twice in one object is a protocol error
    // (spec 03-client-server-channel §envelope-header-structure).
    private static final Map<String, Integer> HEADER_FIELD_BITS =
            Map.ofEntries(
                    Map.entry("formatMarker", 1 << 0),
                    Map.entry("kind", 1 << 1),
                    Map.entry("channelName", 1 << 2),
                    Map.entry("messageName", 1 << 3),
                    Map.entry("contentType", 1 << 4),
                    Map.entry("correlationId", 1 << 5),
                    Map.entry("deadline", 1 << 6),
                    Map.entry("topic", 1 << 7),
                    Map.entry("errorCode", 1 << 8),
                    Map.entry("errorMessage", 1 << 9),
                    Map.entry("source", 1 << 10),
                    Map.entry("metadata", 1 << 11),
                    Map.entry("flowId", 1 << 12),
                    Map.entry("flowOrigin", 1 << 13));
    private static final int MAX_DECODED_HEADER_CACHE_ENTRIES = 64;
    private static final int MAX_SIMPLE_HEADER_CACHE_ENTRIES = 4096;
    private static final int MAX_CACHEABLE_HEADER_BYTES = 1024;
    private static final ZLinkStateLane CACHE_LANE = new ZLinkStateLane();
    private static volatile HeaderCacheEntry[] decodedHeaderCache = new HeaderCacheEntry[0];
    private static final ConcurrentHashMap<Header, byte[]> SIMPLE_HEADER_CACHE =
            new ConcurrentHashMap<>();
    private static final ArrayDeque<Header> simpleHeaderCacheOrder = new ArrayDeque<>();

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

    private static long writeMetadata(Map<String, String> metadata, byte[] json, long offset) {
        if (metadata.isEmpty()) return ascii(json, offset, "{}");
        long size = ascii(json, offset, "{");
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
                if (size > 1) size += ascii(json, offset + size, ",");
                size += jsonString(json, offset + size, key, true);
                size += ascii(json, offset + size, ":");
                size += jsonString(json, offset + size, value, true);
            }
        } catch (IOException ex) {
            throw protocolError("ZLink envelope metadata must be valid UTF-8", ex);
        }
        size += ascii(json, offset + size, "}");
        if (size > ServiceWireConstants.METADATA_BYTES) {
            throw protocolError(
                    "ZLink envelope metadata exceeds "
                            + ServiceWireConstants.METADATA_BYTES
                            + " UTF-8 bytes",
                    null);
        }
        return size;
    }

    private static int ascii(byte[] json, long offset, String text) {
        if (json != null) {
            int position = (int) offset;
            for (int index = 0; index < text.length(); index++) {
                json[position + index] = (byte) text.charAt(index);
            }
        }
        return text.length();
    }

    private static long jsonString(byte[] json, long offset, String value, boolean metadata)
            throws IOException {
        if (value == null) return ascii(json, offset, "null");
        long size = ascii(json, offset, "\"");
        for (int index = 0; index < value.length(); index++) {
            char c = value.charAt(index);
            if (c >= 0x20 && c < 0x80 && c != '"' && c != '\\') {
                if (json != null) json[(int) (offset + size)] = (byte) c;
                size++;
                continue;
            }
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
                size += ascii(json, offset + size, escape);
            } else if (c < 0x20) {
                size += unicodeEscape(json, offset + size, c, metadata);
            } else {
                int codePoint = c;
                if (Character.isSurrogate(c)) {
                    // UTF8JsonGenerator escapes each UTF-16 surrogate in header fields.
                    if (!metadata) {
                        size += unicodeEscape(json, offset + size, c, false);
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
                if (json != null) {
                    int position = (int) (offset + size);
                    if (length == 1) {
                        json[position] = (byte) codePoint;
                    } else {
                        json[position++] =
                                (byte)
                                        ((0xff << (8 - length))
                                                | (codePoint >>> (6 * (length - 1))));
                        for (int shift = 6 * (length - 2); shift >= 0; shift -= 6) {
                            json[position++] = (byte) (0x80 | ((codePoint >>> shift) & 0x3f));
                        }
                    }
                }
                size += length;
            }
        }
        return size + ascii(json, offset + size, "\"");
    }

    private static int unicodeEscape(byte[] json, long offset, char value, boolean metadata) {
        ascii(json, offset, "\\u");
        if (json != null) {
            int position = (int) offset + 2;
            for (int shift = Character.SIZE - 4; shift >= 0; shift -= 4) {
                char digit = Character.forDigit((value >>> shift) & 0xf, 16);
                json[position++] = (byte) (metadata ? digit : Character.toUpperCase(digit));
            }
        }
        return 6;
    }

    private static int jsonInt(byte[] json, long offset, int value) {
        long magnitude = Math.abs((long) value);
        int digits = 1;
        for (long rest = magnitude; rest >= 10; rest /= 10) digits++;
        int size = digits + (value < 0 ? 1 : 0);
        if (json != null) {
            int end = (int) offset + size;
            for (int index = end - 1; index >= end - digits; index--) {
                json[index] = (byte) ('0' + magnitude % 10);
                magnitude /= 10;
            }
            if (value < 0) json[(int) offset] = (byte) '-';
        }
        return size;
    }

    private static long field(byte[] json, long offset, String prefix, String value)
            throws IOException {
        int size = ascii(json, offset, prefix);
        return size + jsonString(json, offset + size, value, false);
    }

    /** Immutable field encodings retained by the registration/codec owner. */
    public static final class PreparedNames {
        private final String channelName;
        private final String messageName;
        private final String contentType;
        private final byte[] channelBytes;
        private final byte[] messageBytes;
        private final byte[] contentBytes;

        public PreparedNames(String channelName, String messageName, String contentType) {
            this.channelName = channelName;
            this.messageName = messageName;
            this.contentType = contentType;
            channelBytes = channelName == null ? null : prepareString(channelName);
            messageBytes = messageName == null ? null : prepareString(messageName);
            contentBytes = contentType == null ? null : prepareString(contentType);
        }

        /** Shares the registered content type while preparing a declared message name once. */
        public PreparedNames(String messageName, PreparedNames contentNames) {
            channelName = null;
            channelBytes = null;
            this.messageName = messageName;
            messageBytes = prepareString(messageName);
            contentType = contentNames.contentType;
            contentBytes = contentNames.contentBytes;
        }

        @Override
        public boolean equals(Object other) {
            if (this == other) return true;
            if (!(other instanceof PreparedNames names)) return false;
            return Objects.equals(channelName, names.channelName)
                    && Objects.equals(messageName, names.messageName)
                    && Objects.equals(contentType, names.contentType);
        }

        @Override
        public int hashCode() {
            int hash = Objects.hashCode(channelName);
            hash = 31 * hash + Objects.hashCode(messageName);
            return 31 * hash + Objects.hashCode(contentType);
        }

        private static byte[] prepareString(String value) {
            try {
                long size = jsonString(null, 0, value, false);
                if (size > Integer.MAX_VALUE)
                    throw new IOException("ZLink envelope string is too large");
                byte[] bytes = new byte[(int) size];
                jsonString(bytes, 0, value, false);
                return bytes;
            } catch (IOException failure) {
                throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                        "ZLink envelope string could not be encoded",
                        failure);
            }
        }
    }

    private static long preparedField(
            byte[] json, long offset, String prefix, String value, byte[] prepared)
            throws IOException {
        if (prepared == null) return field(json, offset, prefix, value);
        int size = ascii(json, offset, prefix);
        if (json != null) System.arraycopy(prepared, 0, json, (int) offset + size, prepared.length);
        return (long) size + prepared.length;
    }

    private static long writeHeader(
            Header header,
            byte[] json,
            PreparedNames channelNames,
            PreparedNames names,
            PreparedNames contentNames)
            throws IOException {
        long size = ascii(json, 0, "{\"formatMarker\":");
        size += jsonInt(json, size, FORMAT_MARKER);
        size += field(json, size, ",\"flowId\":", header.flowId());
        size += ascii(json, size, ",\"flowOrigin\":");
        size +=
                header.flowId() == null
                        ? ascii(json, size, "null")
                        : jsonInt(json, size, flowOriginWireValue(header.flowOrigin()));
        size += ascii(json, size, ",\"kind\":");
        size += jsonInt(json, size, header.kind());
        size +=
                preparedField(
                        json,
                        size,
                        ",\"channelName\":",
                        header.channelName(),
                        channelNames != null
                                        && Objects.equals(
                                                channelNames.channelName, header.channelName())
                                ? channelNames.channelBytes
                                : null);
        size +=
                preparedField(
                        json,
                        size,
                        ",\"messageName\":",
                        header.messageName(),
                        names != null && Objects.equals(names.messageName, header.messageName())
                                ? names.messageBytes
                                : null);
        size +=
                preparedField(
                        json,
                        size,
                        ",\"contentType\":",
                        header.contentType(),
                        contentNames != null
                                        && Objects.equals(
                                                contentNames.contentType, header.contentType())
                                ? contentNames.contentBytes
                                : null);
        size += field(json, size, ",\"correlationId\":", header.correlationId());
        size += field(json, size, ",\"deadline\":", header.deadline());
        size += field(json, size, ",\"topic\":", header.topic());
        size += field(json, size, ",\"errorCode\":", header.errorCode());
        size += field(json, size, ",\"errorMessage\":", header.errorMessage());
        size += field(json, size, ",\"source\":", header.source());
        size += ascii(json, size, ",\"metadata\":");
        size += writeMetadata(header.metadata(), json, size);
        return size + ascii(json, size, "}");
    }

    public static Message encodeHeader(Header header) {
        return Message.from(encodeHeaderBytesInternal(header));
    }

    /** Returns an owned JSON header array; simple headers reuse an internal cached encoding. */
    public static byte[] encodeHeaderBytes(Header header) {
        return encodeHeaderBytes(header, null);
    }

    private static byte[] encodeHeaderBytesInternal(Header header) {
        return encodeHeaderBytesInternal(header, null, null, null);
    }

    private static boolean isSimpleHeader(Header header) {
        return (header.kind() == KIND_COMMAND || header.kind() == KIND_PUBLISH)
                && header.correlationId() == null
                && header.deadline() == null
                && header.topic() == null
                && header.errorCode() == null
                && header.errorMessage() == null
                && header.source() == null
                && header.metadata().isEmpty()
                && header.flowId() == null
                && header.flowOrigin() == null;
    }

    private static byte[] getSimpleHeaderBytes(Header header) {
        byte[] hit = SIMPLE_HEADER_CACHE.get(header);
        if (hit != null) return hit;

        var mutation =
                CACHE_LANE.tryRunNow(
                        () -> {
                            byte[] cached = SIMPLE_HEADER_CACHE.get(header);
                            if (cached != null) return cached;

                            if (SIMPLE_HEADER_CACHE.size() >= MAX_SIMPLE_HEADER_CACHE_ENTRIES) {
                                Header evicted = simpleHeaderCacheOrder.removeFirst();
                                SIMPLE_HEADER_CACHE.remove(evicted);
                            }
                            byte[] encoded = encodeHeaderBytesUncached(header);
                            SIMPLE_HEADER_CACHE.put(header, encoded);
                            simpleHeaderCacheOrder.addLast(header);
                            return encoded;
                        });
        return mutation == null
                ? encodeHeaderBytesUncached(header)
                : Objects.requireNonNull(mutation.toCompletableFuture().getNow(null));
    }

    private static byte[] encodeHeaderBytesUncached(Header header) {
        return encodeHeaderBytesUncached(header, null, null, null);
    }

    /** Encodes with registration-owned names, using the ordinary writer for dynamic fields. */
    public static byte[] encodeHeaderBytes(Header header, PreparedNames names) {
        validateFlowPair(header.flowId(), header.flowOrigin());
        if (isSimpleHeader(header)) return getSimpleHeaderBytes(header).clone();
        return encodeHeaderBytesUncached(header, names, names, names);
    }

    private static byte[] encodeHeaderBytesInternal(
            Header header,
            PreparedNames channelNames,
            PreparedNames names,
            PreparedNames contentNames) {
        validateFlowPair(header.flowId(), header.flowOrigin());
        if (isSimpleHeader(header)) return getSimpleHeaderBytes(header);
        return encodeHeaderBytesUncached(header, channelNames, names, contentNames);
    }

    private static byte[] encodeHeaderBytesUncached(
            Header header,
            PreparedNames channelNames,
            PreparedNames names,
            PreparedNames contentNames) {
        try {
            long size = writeHeader(header, null, channelNames, names, contentNames);
            if (size > Integer.MAX_VALUE)
                throw new IOException("ZLink envelope header is too large");
            byte[] encoded = new byte[(int) size];
            writeHeader(header, encoded, channelNames, names, contentNames);
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
        return ZLinkFrameworkMultipartView.encoded(encodeHeaderBytesInternal(header), body);
    }

    public static List<Message> encode(Header header, ZLinkFrameworkMultipartView body) {
        return ZLinkFrameworkMultipartView.encoded(encodeHeaderBytesInternal(header), body);
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
        return decodeHeader(headerPart, captureFlow, false);
    }

    private static Header decodeHeader(
            ByteBuffer headerPart, boolean captureFlow, boolean allowNonEnvelope) {
        int start = headerPart == null ? 0 : headerPart.position();
        int limit = headerPart == null ? 0 : headerPart.limit();
        int length = limit - start;
        boolean cacheableSize = headerPart != null && length <= MAX_CACHEABLE_HEADER_BYTES;
        Header cached = findDecodedHeaderCacheEntry(headerPart);
        if (cached != null) {
            headerPart.position(limit);
            return cached;
        }

        return decodeHeader(headerPart, captureFlow, start, limit, cacheableSize, allowNonEnvelope);
    }

    private static Header findDecodedHeaderCacheEntry(ByteBuffer bytes) {
        for (HeaderCacheEntry entry : decodedHeaderCache) {
            if (entry.bytes().equals(bytes)) return entry.header();
        }
        return null;
    }

    private static void addDecodedHeaderCacheEntry(
            ByteBuffer bytes, int start, int limit, Header header) {
        // Cache registration is cold; the hot lookup reads the retained buffers without a view.
        ByteBuffer candidate = bytes.asReadOnlyBuffer().position(start).limit(limit);
        var mutation =
                CACHE_LANE.tryRunNow(
                        () -> {
                            if (findDecodedHeaderCacheEntry(candidate) != null) return null;
                            HeaderCacheEntry[] cache = decodedHeaderCache;
                            int nextSize =
                                    Math.min(cache.length + 1, MAX_DECODED_HEADER_CACHE_ENTRIES);
                            HeaderCacheEntry[] next = new HeaderCacheEntry[nextSize];
                            if (cache.length == next.length) {
                                System.arraycopy(cache, 1, next, 0, next.length - 1);
                            } else {
                                System.arraycopy(cache, 0, next, 0, cache.length);
                            }
                            byte[] owned = new byte[candidate.remaining()];
                            candidate.get(owned);
                            next[next.length - 1] =
                                    new HeaderCacheEntry(
                                            ByteBuffer.wrap(owned).asReadOnlyBuffer(), header);
                            decodedHeaderCache = next;
                            return null;
                        });
        if (mutation != null) mutation.toCompletableFuture().getNow(null);
    }

    /**
     * Lenient envelope probe for reply/branch points: returns {@code null} unless the parts are a
     * well-formed two-part envelope. Never throws, so a raw single-part JSON payload is not
     * mistaken for a corrupt envelope.
     */
    public static Header tryDecodeHeader(List<Message> parts, boolean captureFlow) {
        ByteBuffer first = envelopeHeader(parts);
        if (first == null || !looksLikeEnvelope(first)) {
            return null;
        }
        try {
            return decodeHeader(first, captureFlow);
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
        ByteBuffer first = envelopeHeader(parts);
        return first == null ? null : decodeHeader(first, captureFlow, true);
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
        ByteBuffer first = envelopeHeader(parts);
        return first != null && looksLikeEnvelope(first);
    }

    private static ByteBuffer envelopeHeader(List<Message> parts) {
        return parts == null || parts.size() < 2
                ? null
                : ZLinkFrameworkMultipartView.buffer(parts, 0);
    }

    private static boolean looksLikeEnvelope(ByteBuffer first) {
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

    private static Header decodeHeader(
            ByteBuffer headerPart,
            boolean captureFlow,
            int start,
            int limit,
            boolean cacheableSize,
            boolean allowNonEnvelope) {
        HeaderReader reader = new HeaderReader();
        boolean canonical;
        try {
            canonical = reader.readCanonical(headerPart, captureFlow);
        } catch (StreamConstraintsException unsupported) {
            // The general reader owns Jackson's constraint rejection and error classification.
            canonical = false;
        }
        if (!canonical) reader = new HeaderReader();
        try {
            return reader.decode(
                    headerPart,
                    captureFlow,
                    start,
                    limit,
                    cacheableSize,
                    allowNonEnvelope,
                    canonical);
        } catch (RuntimeException failure) {
            if (headerPart != null) headerPart.position(start);
            throw failure;
        }
    }

    private static final class HeaderReader {
        String correlationId = null;
        String deadline = null;
        Map<String, String> metadata = Map.of();
        boolean hasFlowValue = false;

        int marker = 0;
        int kind = 0;
        boolean kindIsInt = false;
        String channelName = null;
        boolean channelNameIsString = false;
        String messageName = null;
        boolean messageNameIsString = false;
        String contentType = null;
        boolean contentTypeIsStringOrNull = true;
        boolean correlationIdIsStringOrNull = true;
        boolean deadlineIsStringOrNull = true;
        String topic = null;
        boolean topicIsStringOrNull = true;
        String errorCode = null;
        boolean errorCodeIsStringOrNull = true;
        String errorMessage = null;
        boolean errorMessageIsStringOrNull = true;
        String source = null;
        boolean sourceIsStringOrNull = true;
        boolean metadataIsValid = true;
        String flowId = null;
        boolean flowIdIsStringOrNull = true;
        int flowOriginValue = 0;
        boolean flowOriginIsIntOrNull = true;
        boolean hasFlowOrigin = false;

        private Header decode(
                ByteBuffer headerPart,
                boolean captureFlow,
                int start,
                int limit,
                boolean cacheableSize,
                boolean allowNonEnvelope,
                boolean canonical) {
            Header header;
            try (JsonParser json = canonical ? null : JSON.createNonBlockingByteBufferParser()) {
                if (json != null) {
                    validateUtf8(headerPart);
                    ByteBufferFeeder input = (ByteBufferFeeder) json.getNonBlockingInputFeeder();
                    input.feedInput(headerPart);
                    input.endOfInput();
                    if (nextToken(json) != JsonToken.START_OBJECT) {
                        if (allowNonEnvelope) {
                            headerPart.position(start);
                            return null;
                        }
                        throw protocolError("ZLink envelope header must be a JSON object", null);
                    }
                }
                try {
                    if (json != null) {
                        int seenFields = 0;
                        while (nextToken(json) != JsonToken.END_OBJECT) {
                            if (json.currentToken() != JsonToken.FIELD_NAME) {
                                throw new IOException("expected an envelope header field name");
                            }
                            String field = json.currentName();
                            JsonToken value = nextToken(json);
                            if (value == null) {
                                throw new IOException("unexpected end of envelope header");
                            }
                            switch (field) {
                                case "formatMarker" -> {
                                    marker =
                                            value == JsonToken.VALUE_NUMBER_INT
                                                            && json.getNumberType()
                                                                    == JsonParser.NumberType.INT
                                                    ? json.getIntValue()
                                                    : 0;
                                    skipChildren(json);
                                }
                                case "kind" -> {
                                    kindIsInt =
                                            value == JsonToken.VALUE_NUMBER_INT
                                                    && json.getNumberType()
                                                            == JsonParser.NumberType.INT;
                                    if (kindIsInt) {
                                        kind = json.getIntValue();
                                    }
                                    skipChildren(json);
                                }
                                case "channelName" -> {
                                    channelNameIsString = value == JsonToken.VALUE_STRING;
                                    channelName = channelNameIsString ? json.getText() : null;
                                    skipChildren(json);
                                }
                                case "messageName" -> {
                                    messageNameIsString = value == JsonToken.VALUE_STRING;
                                    messageName = messageNameIsString ? json.getText() : null;
                                    skipChildren(json);
                                }
                                case "contentType" -> {
                                    contentTypeIsStringOrNull = isStringOrNull(value);
                                    contentType =
                                            value == JsonToken.VALUE_STRING ? json.getText() : null;
                                    skipChildren(json);
                                }
                                case "correlationId" -> {
                                    correlationIdIsStringOrNull = isStringOrNull(value);
                                    correlationId =
                                            value == JsonToken.VALUE_STRING ? json.getText() : null;
                                    skipChildren(json);
                                }
                                case "deadline" -> {
                                    deadlineIsStringOrNull = isStringOrNull(value);
                                    deadline =
                                            value == JsonToken.VALUE_STRING ? json.getText() : null;
                                    skipChildren(json);
                                }
                                case "topic" -> {
                                    topicIsStringOrNull = isStringOrNull(value);
                                    topic = value == JsonToken.VALUE_STRING ? json.getText() : null;
                                    skipChildren(json);
                                }
                                case "errorCode" -> {
                                    errorCodeIsStringOrNull = isStringOrNull(value);
                                    errorCode =
                                            value == JsonToken.VALUE_STRING ? json.getText() : null;
                                    skipChildren(json);
                                }
                                case "errorMessage" -> {
                                    errorMessageIsStringOrNull = isStringOrNull(value);
                                    errorMessage =
                                            value == JsonToken.VALUE_STRING ? json.getText() : null;
                                    skipChildren(json);
                                }
                                case "source" -> {
                                    sourceIsStringOrNull = isStringOrNull(value);
                                    source =
                                            value == JsonToken.VALUE_STRING ? json.getText() : null;
                                    skipChildren(json);
                                }
                                case "metadata" -> {
                                    metadata = Map.of();
                                    metadataIsValid = true;
                                    if (value == JsonToken.START_OBJECT) {
                                        Map<String, String> values = null;
                                        while (nextToken(json) != JsonToken.END_OBJECT) {
                                            if (json.currentToken() != JsonToken.FIELD_NAME) {
                                                throw new IOException(
                                                        "expected an envelope metadata field name");
                                            }
                                            String key = json.currentName();
                                            JsonToken metadataValue = nextToken(json);
                                            if (metadataValue == null) {
                                                throw new IOException(
                                                        "unexpected end of envelope metadata");
                                            }
                                            if (values == null) {
                                                values = new LinkedHashMap<>();
                                            }
                                            if (values.containsKey(key)) {
                                                throw protocolError(
                                                        "duplicate envelope metadata key", null);
                                            }
                                            values.put(
                                                    key,
                                                    metadataValue == JsonToken.VALUE_STRING
                                                            ? json.getText()
                                                            : null);
                                            skipChildren(json);
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
                                        skipChildren(json);
                                    }
                                }
                                case "flowId" -> {
                                    hasFlowValue |= value != JsonToken.VALUE_NULL;
                                    if (captureFlow) {
                                        flowIdIsStringOrNull = isStringOrNull(value);
                                        flowId =
                                                value == JsonToken.VALUE_STRING
                                                        ? json.getText()
                                                        : null;
                                    }
                                    skipChildren(json);
                                }
                                case "flowOrigin" -> {
                                    hasFlowValue |= value != JsonToken.VALUE_NULL;
                                    if (captureFlow) {
                                        hasFlowOrigin = value != JsonToken.VALUE_NULL;
                                        flowOriginIsIntOrNull =
                                                value == JsonToken.VALUE_NULL
                                                        || (value == JsonToken.VALUE_NUMBER_INT
                                                                && json.getNumberType()
                                                                        == JsonParser.NumberType
                                                                                .INT);
                                        if (flowOriginIsIntOrNull && hasFlowOrigin) {
                                            flowOriginValue = json.getIntValue();
                                        }
                                    }
                                    skipChildren(json);
                                }
                                default -> {
                                    skipChildren(json);
                                    continue;
                                }
                            }
                            int fieldBit = HEADER_FIELD_BITS.get(field);
                            if ((seenFields & fieldBit) != 0) {
                                throw protocolError("duplicate envelope header field", null);
                            }
                            seenFields |= fieldBit;
                        }

                        if (nextToken(json) != null) {
                            throw new IOException(
                                    "ZLink envelope header must contain one JSON object");
                        }
                    }
                    header = finish(captureFlow);
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

                headerPart.position(limit);
            } catch (Exception ex) {
                if (allowNonEnvelope
                        && !looksLikeEnvelope(headerPart.duplicate().position(start))) {
                    headerPart.position(start);
                    return null;
                }
                if (ex instanceof ZLinkFrameworkException frameworkError) {
                    throw frameworkError;
                }
                throw protocolError("invalid ZLink envelope header: " + ex.getMessage(), ex);
            }

            if (cacheableSize
                    && correlationId == null
                    && deadline == null
                    && !hasFlowValue
                    && metadata.isEmpty()) {
                addDecodedHeaderCacheEntry(headerPart, start, limit, header);
            }
            return header;
        }

        private Header finish(boolean captureFlow) throws IOException {
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
            writeMetadata(metadata, null, 0);

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
        }

        private boolean readCanonical(ByteBuffer bytes, boolean captureFlow)
                throws StreamConstraintsException {
            if (bytes == null) return false;
            JSON.streamReadConstraints().validateDocumentLength(bytes.remaining());
            JSON.streamReadConstraints().validateNestingDepth(2);
            CanonicalReader reader = new CanonicalReader(bytes);
            if (!reader.field("{\"formatMarker\":")) return false;
            marker = reader.number(false);
            if (!reader.field(",\"flowId\":")) return false;
            String readFlowId = reader.string(false);
            hasFlowValue = readFlowId != null;
            if (!reader.field(",\"flowOrigin\":")) return false;
            int origin = reader.number(true);
            boolean originPresent = !reader.wasNull;
            hasFlowValue |= originPresent;
            if (captureFlow) {
                flowId = readFlowId;
                flowOriginValue = origin;
                hasFlowOrigin = originPresent;
            }
            if (!reader.field(",\"kind\":")) return false;
            kind = reader.number(false);
            kindIsInt = true;
            if (!reader.field(",\"channelName\":")) return false;
            channelName = reader.string(false);
            channelNameIsString = channelName != null;
            if (!channelNameIsString) return false;
            if (!reader.field(",\"messageName\":")) return false;
            messageName = reader.string(false);
            messageNameIsString = messageName != null;
            if (!messageNameIsString) return false;
            if (!reader.field(",\"contentType\":")) return false;
            contentType = reader.string(false);
            if (contentType == null) return false;
            if (!reader.field(",\"correlationId\":")) return false;
            correlationId = reader.string(false);
            if (!reader.field(",\"deadline\":")) return false;
            deadline = reader.string(false);
            if (!reader.field(",\"topic\":")) return false;
            topic = reader.string(false);
            if (!reader.field(",\"errorCode\":")) return false;
            errorCode = reader.string(false);
            if (!reader.field(",\"errorMessage\":")) return false;
            errorMessage = reader.string(false);
            if (!reader.field(",\"source\":")) return false;
            source = reader.string(false);
            if (!reader.field(",\"metadata\":") || !reader.consume("{")) return false;
            if (!reader.consume("}")) {
                Map<String, String> values = new LinkedHashMap<>();
                do {
                    String key = reader.string(true);
                    if (key == null || values.containsKey(key) || !reader.consume(":"))
                        return false;
                    String value = reader.string(false);
                    if (value == null) return false;
                    values.put(key, value);
                } while (reader.consume(","));
                if (!reader.consume("}")) return false;
                metadata = values;
            }
            return reader.consume("}") && reader.valid && reader.position == bytes.limit();
        }
    }

    /** Recognizes only the writer's ASCII, unescaped representation; all misses use JSON. */
    private static final class CanonicalReader {
        private final ByteBuffer bytes;
        private int position;
        private boolean valid = true;
        private boolean wasNull;

        private CanonicalReader(ByteBuffer bytes) {
            this.bytes = bytes;
            position = bytes.position();
        }

        private boolean consume(String literal) {
            if (!valid || bytes.limit() - position < literal.length()) return false;
            for (int index = 0; index < literal.length(); index++) {
                if (bytes.get(position + index) != literal.charAt(index)) return false;
            }
            position += literal.length();
            return true;
        }

        private boolean field(String literal) throws StreamConstraintsException {
            if (!consume(literal)) return false;
            JSON.streamReadConstraints().validateNameLength(literal.length() - 4);
            return true;
        }

        private String string(boolean name) throws StreamConstraintsException {
            if (consume("null")) return null;
            if (!consume("\"")) {
                valid = false;
                return null;
            }
            int start = position;
            while (position < bytes.limit()) {
                int value = bytes.get(position++) & 0xff;
                if (value == '"') {
                    int length = position - start - 1;
                    if (name) JSON.streamReadConstraints().validateNameLength(length);
                    else JSON.streamReadConstraints().validateStringLength(length);
                    if (bytes.hasArray()) {
                        return new String(
                                bytes.array(),
                                bytes.arrayOffset() + start,
                                length,
                                StandardCharsets.ISO_8859_1);
                    }
                    char[] chars = new char[length];
                    for (int index = 0; index < chars.length; index++)
                        chars[index] = (char) bytes.get(start + index);
                    return new String(chars);
                }
                if (value < 0x20 || value >= 0x80 || value == '\\') break;
            }
            valid = false;
            return null;
        }

        private int number(boolean nullable) throws StreamConstraintsException {
            wasNull = nullable && consume("null");
            if (wasNull) return 0;
            int start = position;
            boolean negative = consume("-");
            long value = 0;
            int digits = position;
            while (position < bytes.limit()) {
                int digit = bytes.get(position) - '0';
                if (digit < 0 || digit > 9) break;
                value = value * 10 + digit;
                position++;
                if (value > (negative ? 2147483648L : Integer.MAX_VALUE)) {
                    valid = false;
                    return 0;
                }
            }
            JSON.streamReadConstraints().validateIntegerLength(position - digits);
            if (position == digits
                    || (position - digits > 1 && bytes.get(digits) == '0')
                    || (negative && position == start + 1)) valid = false;
            return (int) (negative ? -value : value);
        }
    }

    private static void validateUtf8(ByteBuffer bytes) throws IOException {
        var decoder = StandardCharsets.UTF_8.newDecoder();
        ByteBuffer input = bytes.asReadOnlyBuffer();
        // A supplementary code point requires two output characters.
        CharBuffer output = CharBuffer.allocate(Math.min(1024, Math.max(2, input.remaining())));
        CoderResult result;
        do {
            result = decoder.decode(input, output, true);
            if (result.isError()) result.throwException();
            output.clear();
        } while (result.isOverflow());
        do {
            result = decoder.flush(output);
            if (result.isError()) result.throwException();
            output.clear();
        } while (result.isOverflow());
    }

    private static JsonToken nextToken(JsonParser json) throws IOException {
        JsonToken token = json.nextToken();
        if (token == JsonToken.VALUE_STRING || token == JsonToken.FIELD_NAME) {
            String value = json.getText();
            for (int index = 0; index < value.length(); index++) {
                char character = value.charAt(index);
                if (!Character.isSurrogate(character)) continue;
                if (!Character.isHighSurrogate(character)
                        || index + 1 == value.length()
                        || !Character.isLowSurrogate(value.charAt(++index))) {
                    throw new IOException("ZLink envelope string must be valid Unicode");
                }
            }
        }
        return token;
    }

    private static void skipChildren(JsonParser json) throws IOException {
        if (!json.currentToken().isStructStart()) return;
        int depth = 1;
        while (depth != 0) {
            JsonToken token = nextToken(json);
            if (token == null) throw new IOException("unexpected end of envelope header");
            if (token.isStructStart()) depth++;
            if (token.isStructEnd()) depth--;
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

    /** Package view of the header caches for the cache bound tests. */
    static int decodedHeaderCacheSize() {
        return decodedHeaderCache.length;
    }

    static int simpleHeaderCacheSize() {
        return SIMPLE_HEADER_CACHE.size();
    }

    static ZLinkStateLane headerCacheLane() {
        return CACHE_LANE;
    }

    private record HeaderCacheEntry(ByteBuffer bytes, Header header) {}

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
