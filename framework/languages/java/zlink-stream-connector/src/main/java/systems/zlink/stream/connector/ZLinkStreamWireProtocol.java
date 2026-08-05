package systems.zlink.stream.connector;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.Map;

final class ZLinkStreamWireProtocol {
    static final int FORMAT_MARKER = 0xF2;
    static final int CODEC_RAW = 0;
    static final int CODEC_JSON = 1;
    static final int CODEC_MESSAGE_PACK = 2;
    static final int CODEC_PROTOBUF = 3;

    static final int KIND_SEND = 1;
    static final int KIND_REQUEST = 2;
    static final int KIND_RESPONSE = 3;
    static final int KIND_ERROR = 4;
    static final int KIND_CONTROL = 5;

    static final int FLAG_HAS_REQUEST_SEQ = 0x01;
    static final int FLAG_HAS_METADATA = 0x02;
    static final int FLAG_PAYLOAD_COMPRESSED = 0x04;
    static final int FLAG_HAS_CORRELATION_ID = 0x08;
    static final int FLAG_HAS_FLOW_ID = 0x10;

    private static final int KNOWN_FLAGS =
        FLAG_HAS_REQUEST_SEQ | FLAG_HAS_METADATA | FLAG_PAYLOAD_COMPRESSED
            | FLAG_HAS_CORRELATION_ID | FLAG_HAS_FLOW_ID;
    private static final int MAX_PACKET_NAME_BYTES = 255;
    private static final int MAX_METADATA_BYTES = 1024;

    private ZLinkStreamWireProtocol() {
    }

    static byte[] encodeHeader(Header header) {
        String wireName = isReply(header.kind()) ? "" : header.name();
        validateHeader(header, wireName);
        byte[] name = wireName.getBytes(StandardCharsets.UTF_8);
        byte[] metadata = header.metadata().isEmpty()
            ? new byte[0]
            : encodeMetadata(header.metadata());
        boolean hasCorrelationId =
            header.correlationId() != null && !header.correlationId().isEmpty();
        byte[] correlation = hasCorrelationId
            ? header.correlationId().getBytes(StandardCharsets.UTF_8)
            : new byte[0];
        if (correlation.length > 255) {
            throw new IllegalArgumentException("correlation id is too long");
        }
        int flags = header.flags();
        flags = header.requestSeq() == null
            ? flags & ~FLAG_HAS_REQUEST_SEQ
            : flags | FLAG_HAS_REQUEST_SEQ;
        flags = header.metadata().isEmpty()
            ? flags & ~FLAG_HAS_METADATA
            : flags | FLAG_HAS_METADATA;
        flags = hasCorrelationId
            ? flags | FLAG_HAS_CORRELATION_ID
            : flags & ~FLAG_HAS_CORRELATION_ID;
        boolean hasFlow = header.flowId() != null;
        flags = hasFlow ? flags | FLAG_HAS_FLOW_ID : flags & ~FLAG_HAS_FLOW_ID;

        int size = 4
            + (header.requestSeq() == null ? 0 : Long.BYTES)
            + 1
            + name.length
            + (metadata.length == 0 ? 0 : 2 + metadata.length)
            + (hasCorrelationId ? 1 + correlation.length : 0)
            + (hasFlow ? 37 : 0);
        ByteBuffer buffer = ByteBuffer.allocate(size);
        buffer.put((byte) FORMAT_MARKER);
        buffer.put((byte) header.kind());
        buffer.put((byte) header.codec());
        buffer.put((byte) flags);
        if (header.requestSeq() != null) {
            buffer.putLong(header.requestSeq());
        }
        buffer.put((byte) name.length);
        buffer.put(name);
        if (metadata.length > 0) {
            buffer.putShort((short) metadata.length);
            buffer.put(metadata);
        }
        if (hasCorrelationId) {
            buffer.put((byte) correlation.length);
            buffer.put(correlation);
        }
        if (hasFlow) {
            buffer.put(header.flowId().getBytes(StandardCharsets.US_ASCII));
            buffer.put((byte) header.flowOrigin());
        }
        return buffer.array();
    }

    static Header decodeHeader(byte[] header) {
        if (header.length < 5) {
            throw new IllegalArgumentException("stream header is too short");
        }
        ByteBuffer buffer = ByteBuffer.wrap(header);
        if (Byte.toUnsignedInt(buffer.get()) != FORMAT_MARKER) {
            throw new IllegalArgumentException("stream header format marker is missing or unsupported");
        }
        int kind = Byte.toUnsignedInt(buffer.get());
        int codec = Byte.toUnsignedInt(buffer.get());
        int flags = Byte.toUnsignedInt(buffer.get());
        Long requestSeq = null;
        if ((flags & FLAG_HAS_REQUEST_SEQ) != 0) {
            requireRemaining(buffer, Long.BYTES, "request sequence");
            requestSeq = buffer.getLong();
            if (requestSeq == 0) {
                throw new IllegalArgumentException("request sequence must not be zero");
            }
        }
        requireRemaining(buffer, 1, "packet name length");
        int nameLength = Byte.toUnsignedInt(buffer.get());
        requireRemaining(buffer, nameLength, "packet name");
        byte[] nameBytes = new byte[nameLength];
        buffer.get(nameBytes);
        Map<String, String> metadata = Map.of();
        if ((flags & FLAG_HAS_METADATA) != 0) {
            requireRemaining(buffer, 2, "metadata length");
            int metadataLength = Short.toUnsignedInt(buffer.getShort());
            requireRemaining(buffer, metadataLength, "metadata");
            byte[] metadataBytes = new byte[metadataLength];
            buffer.get(metadataBytes);
            metadata = decodeMetadata(metadataBytes);
        }
        String correlationId = null;
        if ((flags & FLAG_HAS_CORRELATION_ID) != 0) {
            requireRemaining(buffer, 1, "correlation id length");
            int correlationLength = Byte.toUnsignedInt(buffer.get());
            if (correlationLength == 0) {
                throw new IllegalArgumentException("correlation id is invalid");
            }
            requireRemaining(buffer, correlationLength, "correlation id");
            byte[] correlationBytes = new byte[correlationLength];
            buffer.get(correlationBytes);
            correlationId = new String(correlationBytes, StandardCharsets.UTF_8);
        }
        String flowId = null;
        int flowOrigin = 0;
        if ((flags & FLAG_HAS_FLOW_ID) != 0) {
            requireRemaining(buffer, 37, "flow fields");
            byte[] flowBytes = new byte[36];
            buffer.get(flowBytes);
            flowId = new String(flowBytes, StandardCharsets.US_ASCII);
            flowOrigin = Byte.toUnsignedInt(buffer.get());
        }
        if (buffer.hasRemaining()) {
            throw new IllegalArgumentException("stream header contains trailing bytes");
        }
        Header decoded = new Header(
            kind,
            codec,
            flags,
            requestSeq,
            new String(nameBytes, StandardCharsets.UTF_8),
            metadata,
            correlationId,
            flowId,
            flowOrigin);
        validateHeader(decoded);
        return decoded;
    }

    static byte[] encodeFrame(byte[] header, byte[] payload, int maxPayloadSize) {
        if (header.length > 0xFFFF) {
            throw new IllegalArgumentException("header exceeds u16 header_size");
        }
        if (payload.length > maxPayloadSize) {
            throw new IllegalArgumentException("payload exceeds max payload size");
        }
        ByteBuffer buffer = ByteBuffer.allocate(6 + header.length + payload.length);
        buffer.putShort((short) header.length);
        buffer.putInt(payload.length);
        buffer.put(header);
        buffer.put(payload);
        return buffer.array();
    }

    static Frame decodeFrame(byte[] frame) {
        return decodeFrame(frame, 64 * 1024);
    }

    static Frame decodeFrame(byte[] frame, int maxPayloadSize) {
        if (frame.length < 6) {
            throw new IllegalArgumentException("frame prefix is incomplete");
        }
        ByteBuffer buffer = ByteBuffer.wrap(frame);
        int headerLength = Short.toUnsignedInt(buffer.getShort());
        int payloadLength = buffer.getInt();
        int bodyLength = checkedBodyLength(headerLength, payloadLength, maxPayloadSize);
        if (frame.length != 6L + bodyLength) {
            throw new IllegalArgumentException("frame length does not match prefix");
        }
        byte[] header = new byte[headerLength];
        byte[] payload = new byte[payloadLength];
        buffer.get(header);
        buffer.get(payload);
        return new Frame(header, payload);
    }

    static int checkedBodyLength(int headerLength, int payloadLength, int maxPayloadSize) {
        if (payloadLength < 0) {
            throw new IllegalArgumentException("negative payload length");
        }
        if (payloadLength > maxPayloadSize) {
            throw new IllegalArgumentException("payload exceeds max receive payload size");
        }
        long bodyLength = (long) headerLength + payloadLength;
        if (bodyLength > Integer.MAX_VALUE) {
            throw new IllegalArgumentException("frame body length exceeds supported size");
        }
        return (int) bodyLength;
    }

    static long maxFrameLength(int maxPayloadSize) {
        return 6L + 0xFFFF + maxPayloadSize;
    }

    private static byte[] encodeMetadata(Map<String, String> metadata) {
        if (metadata.size() > 255) {
            throw new IllegalArgumentException("metadata entry count must not exceed 255");
        }
        int size = 1;
        for (Map.Entry<String, String> entry : metadata.entrySet()) {
            byte[] key = entry.getKey().getBytes(StandardCharsets.UTF_8);
            byte[] value = entry.getValue().getBytes(StandardCharsets.UTF_8);
            if (key.length == 0 || key.length > 255) {
                throw new IllegalArgumentException("metadata key length is invalid");
            }
            if (value.length > 0xFFFF) {
                throw new IllegalArgumentException("metadata value is too large");
            }
            size += 1 + key.length + 2 + value.length;
            if (size > MAX_METADATA_BYTES) {
                throw new IllegalArgumentException("metadata must not exceed 1024 bytes");
            }
        }
        ByteBuffer buffer = ByteBuffer.allocate(size);
        buffer.put((byte) metadata.size());
        for (Map.Entry<String, String> entry : metadata.entrySet()) {
            byte[] key = entry.getKey().getBytes(StandardCharsets.UTF_8);
            byte[] value = entry.getValue().getBytes(StandardCharsets.UTF_8);
            buffer.put((byte) key.length);
            buffer.put(key);
            buffer.putShort((short) value.length);
            buffer.put(value);
        }
        return buffer.array();
    }

    private static Map<String, String> decodeMetadata(byte[] metadata) {
        if (metadata.length == 0) {
            throw new IllegalArgumentException("metadata payload is empty");
        }
        ByteBuffer buffer = ByteBuffer.wrap(metadata);
        int count = Byte.toUnsignedInt(buffer.get());
        Map<String, String> values = new LinkedHashMap<>();
        for (int i = 0; i < count; i++) {
            requireRemaining(buffer, 1, "metadata key length");
            int keyLength = Byte.toUnsignedInt(buffer.get());
            if (keyLength == 0) {
                throw new IllegalArgumentException("metadata key is invalid");
            }
            requireRemaining(buffer, keyLength, "metadata key");
            byte[] keyBytes = new byte[keyLength];
            buffer.get(keyBytes);
            requireRemaining(buffer, 2, "metadata value length");
            int valueLength = Short.toUnsignedInt(buffer.getShort());
            requireRemaining(buffer, valueLength, "metadata value");
            byte[] valueBytes = new byte[valueLength];
            buffer.get(valueBytes);
            String key = new String(keyBytes, StandardCharsets.UTF_8);
            if (values.containsKey(key)) {
                throw new IllegalArgumentException("duplicate metadata key");
            }
            values.put(key, new String(valueBytes, StandardCharsets.UTF_8));
        }
        if (buffer.hasRemaining()) {
            throw new IllegalArgumentException("metadata contains trailing bytes");
        }
        return Collections.unmodifiableMap(values);
    }

    private static void validateHeader(Header header) {
        validateHeader(header, header.name());
    }

    private static void validateHeader(Header header, String packetName) {
        validateEnum(header.kind(), header.codec(), header.flags());
        byte[] name = packetName.getBytes(StandardCharsets.UTF_8);
        if ((isReply(header.kind()) && name.length != 0)
            || (!isReply(header.kind()) && name.length == 0)
            || name.length > MAX_PACKET_NAME_BYTES) {
            throw new IllegalArgumentException("packet name is invalid");
        }
        boolean hasRequestSeq = header.requestSeq() != null
            || (header.flags() & FLAG_HAS_REQUEST_SEQ) != 0;
        boolean hasMetadata = !header.metadata().isEmpty()
            || (header.flags() & FLAG_HAS_METADATA) != 0;
        if (header.kind() == KIND_SEND && hasRequestSeq) {
            throw new IllegalArgumentException("send packet must not contain a request sequence");
        }
        if ((header.kind() == KIND_REQUEST || header.kind() == KIND_RESPONSE) && !hasRequestSeq) {
            throw new IllegalArgumentException("request and response packets must contain a request sequence");
        }
        if (header.kind() == KIND_ERROR && header.codec() != CODEC_JSON) {
            throw new IllegalArgumentException("error packet must use the JSON codec");
        }
        boolean hasCorrelationId =
            header.correlationId() != null && !header.correlationId().isEmpty();
        boolean hasFlow = header.flowId() != null;
        if (hasFlow) {
            if (!header.flowId().matches("[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")) {
                throw new IllegalArgumentException("flow id must be a canonical UUIDv7");
            }
            if (header.flowOrigin() < 1 || header.flowOrigin() > 4) {
                throw new IllegalArgumentException("flow origin is invalid");
            }
        } else if (header.flowOrigin() != 0) {
            throw new IllegalArgumentException("flow id and origin must be present together");
        }
        if (header.kind() == KIND_CONTROL
            && (header.flags() != 0 || hasRequestSeq || hasMetadata || hasCorrelationId || hasFlow
                || header.codec() != CODEC_RAW)) {
            throw new IllegalArgumentException("control packet must use raw codec and must not contain flags");
        }
    }

    private static boolean isReply(int kind) {
        return kind == KIND_RESPONSE || kind == KIND_ERROR;
    }

    private static void validateEnum(int kind, int codec, int flags) {
        if (kind < KIND_SEND || kind > KIND_CONTROL) {
            throw new IllegalArgumentException("unknown stream message kind");
        }
        if (codec < CODEC_RAW || codec > CODEC_PROTOBUF) {
            throw new IllegalArgumentException("unknown stream codec");
        }
        if ((flags & ~KNOWN_FLAGS) != 0) {
            throw new IllegalArgumentException("unknown stream header flag");
        }
    }

    private static void requireRemaining(ByteBuffer buffer, int needed, String name) {
        if (buffer.remaining() < needed) {
            throw new IllegalArgumentException(name + " is incomplete");
        }
    }

    record Header(
        int kind,
        int codec,
        int flags,
        Long requestSeq,
        String name,
        Map<String, String> metadata,
        String correlationId,
        String flowId,
        int flowOrigin) {
        Header {
            metadata = Collections.unmodifiableMap(new LinkedHashMap<>(metadata));
        }

        Header(
            int kind,
            int codec,
            int flags,
            Long requestSeq,
            String name,
            Map<String, String> metadata,
            String correlationId) {
            this(kind, codec, flags, requestSeq, name, metadata, correlationId, null, 0);
        }
    }

    record Frame(byte[] header, byte[] payload) {
        Frame {
            header = Arrays.copyOf(header, header.length);
            payload = Arrays.copyOf(payload, payload.length);
        }

        @Override
        public byte[] header() {
            return Arrays.copyOf(header, header.length);
        }

        @Override
        public byte[] payload() {
            return Arrays.copyOf(payload, payload.length);
        }
    }
}
