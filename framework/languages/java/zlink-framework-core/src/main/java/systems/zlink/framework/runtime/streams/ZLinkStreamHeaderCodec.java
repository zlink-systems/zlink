package systems.zlink.framework.runtime.streams;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.EnumSet;
import java.util.LinkedHashMap;
import java.util.Map;
import java.util.Optional;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;

public final class ZLinkStreamHeaderCodec {
    static final int FORMAT_MARKER = 0xF2;
    static final int KIND_SEND = 1;
    static final int KIND_REQUEST = 2;
    static final int KIND_RESPONSE = 3;
    static final int KIND_ERROR = 4;
    static final int KIND_CONTROL = 5;
    private static final int CODEC_RAW = 0;
    private static final int FLAG_HAS_REQUEST_SEQ = 0x01;

    private ZLinkStreamHeaderCodec() {
    }

    public static ZLinkStreamHeader decodeOrPlain(byte[] bytes) {
        if (bytes.length < 5 || Byte.toUnsignedInt(bytes[0]) != FORMAT_MARKER) {
            throw new IllegalArgumentException("STREAM header format marker is missing or unsupported");
        }
        int kind = Byte.toUnsignedInt(bytes[1]);
        int codec = Byte.toUnsignedInt(bytes[2]);
        int flags = Byte.toUnsignedInt(bytes[3]);
        int offset = 4;
        Optional<Long> requestSeq = Optional.empty();
        if ((flags & FLAG_HAS_REQUEST_SEQ) != 0) {
            if (bytes.length - offset < Long.BYTES) {
                throw new IllegalArgumentException("STREAM header request sequence is incomplete");
            }
            long value = ByteBuffer.wrap(bytes, offset, Long.BYTES).getLong();
            if (value == 0) {
                throw new IllegalArgumentException("STREAM request sequence must not be zero");
            }
            requestSeq = Optional.of(value);
            offset += Long.BYTES;
        }
        if (bytes.length - offset < 1) {
            throw new IllegalArgumentException("STREAM header packet name length is missing");
        }
        int nameLength = Byte.toUnsignedInt(bytes[offset++]);
        boolean reply = isReply(kind);
        if ((reply && nameLength != 0) || (!reply && nameLength == 0)
            || bytes.length - offset < nameLength) {
            throw new IllegalArgumentException("STREAM header packet name is invalid");
        }
        String packetName = new String(bytes, offset, nameLength, StandardCharsets.UTF_8);
        offset += nameLength;
        Map<String, String> metadata = Map.of();
        if ((flags & ZLinkStreamHeaderFlag.HAS_METADATA.value()) != 0) {
            if (bytes.length - offset < 2) {
                throw new IllegalArgumentException("STREAM header metadata is incomplete");
            }
            int metadataLength = Short.toUnsignedInt(ByteBuffer.wrap(bytes, offset, 2).getShort());
            offset += 2;
            if (bytes.length - offset < metadataLength) {
                throw new IllegalArgumentException("STREAM header metadata is incomplete");
            }
            metadata = decodeMetadata(bytes, offset, metadataLength);
            offset += metadataLength;
        }
        Optional<String> correlationId = Optional.empty();
        if ((flags & ZLinkStreamHeaderFlag.HAS_CORRELATION_ID.value()) != 0) {
            if (bytes.length - offset < 1) {
                throw new IllegalArgumentException("STREAM header correlation id length is missing");
            }
            int corrLength = Byte.toUnsignedInt(bytes[offset++]);
            if (corrLength == 0 || bytes.length - offset < corrLength) {
                throw new IllegalArgumentException("STREAM header correlation id is invalid");
            }
            correlationId = Optional.of(new String(bytes, offset, corrLength, StandardCharsets.UTF_8));
            offset += corrLength;
        }
        Optional<String> flowId = Optional.empty();
        Optional<ZLinkFlowOrigin> flowOrigin = Optional.empty();
        if ((flags & ZLinkStreamHeaderFlag.HAS_FLOW_ID.value()) != 0) {
            if (bytes.length - offset < 37) {
                throw new IllegalArgumentException("STREAM header flow fields are incomplete");
            }
            flowId = Optional.of(new String(bytes, offset, 36, StandardCharsets.US_ASCII));
            offset += 36;
            flowOrigin = Optional.of(decodeFlowOrigin(Byte.toUnsignedInt(bytes[offset++])));
        }
        if (offset != bytes.length) {
            throw new IllegalArgumentException("STREAM header contains trailing bytes");
        }
        if (kind == KIND_CONTROL
            && (flags != 0
                || codec != CODEC_RAW
                || requestSeq.isPresent()
                || !metadata.isEmpty()
                || correlationId.isPresent()
                || flowId.isPresent())) {
            throw new IllegalArgumentException(
                "STREAM control packet must use raw codec and must not contain flags");
        }
        return new ZLinkStreamHeader(
            ZLinkStreamMessageKind.fromValue(kind),
            ZLinkStreamCodec.fromValue(codec),
            flagsFromValue(flags),
            requestSeq,
            packetName,
            metadata,
            correlationId,
            flowId,
            flowOrigin);
    }

    public static byte[] encode(ZLinkStreamHeader header) {
        if (header == null) {
            throw new IllegalArgumentException("header is required");
        }
        ZLinkStreamHeader effective = header;
        ZLinkFlowContext.State current = ZLinkFlowContext.current();
        if (effective.flowId().isEmpty()
            && current != null
            && effective.kind() != ZLinkStreamMessageKind.CONTROL) {
            effective = effective.withFlow(current.flowId(), current.origin());
        }
        return encode(
            effective.kind().value(),
            effective.codec().value(),
            flagsValue(effective.flags()),
            effective.packetName(),
            effective.requestSequence(),
            effective.metadata(),
            effective.correlationId(),
            effective.flowId(),
            effective.flowOrigin());
    }

    private static byte[] encode(
        int kind,
        int codec,
        int initialFlags,
        String packetName,
        Optional<Long> requestSeq,
        Map<String, String> metadata,
        Optional<String> correlationId,
        Optional<String> flowId,
        Optional<ZLinkFlowOrigin> flowOrigin) {
        boolean reply = isReply(kind);
        if (!reply && (packetName == null || packetName.isBlank())) {
            throw new IllegalArgumentException("packetName is required");
        }
        byte[] name = reply
            ? new byte[0]
            : packetName.getBytes(StandardCharsets.UTF_8);
        byte[] metadataBytes = encodeMetadata(metadata);
        boolean hasMetadata = metadataBytes.length > 0;
        boolean hasCorrelationId = correlationId != null
            && correlationId.isPresent()
            && !correlationId.get().isEmpty();
        if (kind == KIND_CONTROL && hasCorrelationId) {
            throw new IllegalArgumentException(
                "STREAM control packet must not contain a correlation id");
        }
        boolean hasFlow = flowId != null && flowId.isPresent();
        if (hasFlow != (flowOrigin != null && flowOrigin.isPresent())) {
            throw new IllegalArgumentException("STREAM flow id and origin must be present together");
        }
        if (kind == KIND_CONTROL && hasFlow) {
            throw new IllegalArgumentException("STREAM control packet must not contain flow fields");
        }
        byte[] correlationBytes = hasCorrelationId
            ? correlationId.get().getBytes(StandardCharsets.UTF_8)
            : new byte[0];
        if (correlationBytes.length > 255) {
            throw new IllegalArgumentException("STREAM correlation id is too long");
        }
        int flags = requestSeq.isPresent()
            ? initialFlags | FLAG_HAS_REQUEST_SEQ
            : initialFlags & ~FLAG_HAS_REQUEST_SEQ;
        flags = hasMetadata
            ? flags | ZLinkStreamHeaderFlag.HAS_METADATA.value()
            : flags & ~ZLinkStreamHeaderFlag.HAS_METADATA.value();
        flags = hasCorrelationId
            ? flags | ZLinkStreamHeaderFlag.HAS_CORRELATION_ID.value()
            : flags & ~ZLinkStreamHeaderFlag.HAS_CORRELATION_ID.value();
        flags = hasFlow
            ? flags | ZLinkStreamHeaderFlag.HAS_FLOW_ID.value()
            : flags & ~ZLinkStreamHeaderFlag.HAS_FLOW_ID.value();
        ByteBuffer buffer = ByteBuffer.allocate(
            4
                + (requestSeq.isPresent() ? Long.BYTES : 0)
                + 1
                + name.length
                + (hasMetadata ? 2 + metadataBytes.length : 0)
                + (hasCorrelationId ? 1 + correlationBytes.length : 0)
                + (hasFlow ? 37 : 0));
        buffer.put((byte) FORMAT_MARKER);
        buffer.put((byte) kind);
        buffer.put((byte) codec);
        buffer.put((byte) flags);
        requestSeq.ifPresent(value -> {
            if (value == 0) {
                throw new IllegalArgumentException("STREAM request sequence must not be zero");
            }
            buffer.putLong(value);
        });
        buffer.put((byte) name.length);
        buffer.put(name);
        if (hasMetadata) {
            buffer.putShort((short) metadataBytes.length);
            buffer.put(metadataBytes);
        }
        if (hasCorrelationId) {
            buffer.put((byte) correlationBytes.length);
            buffer.put(correlationBytes);
        }
        if (hasFlow) {
            buffer.put(flowId.get().getBytes(StandardCharsets.US_ASCII));
            buffer.put((byte) encodeFlowOrigin(flowOrigin.get()));
        }
        return buffer.array();
    }

    private static boolean isKnownKind(byte value) {
        int kind = Byte.toUnsignedInt(value);
        return kind >= KIND_SEND && kind <= 5;
    }

    private static boolean isReply(int kind) {
        return kind == KIND_RESPONSE || kind == KIND_ERROR;
    }

    private static int encodeFlowOrigin(ZLinkFlowOrigin origin) {
        return switch (origin) {
            case INBOUND -> 1;
            case TIMER -> 2;
            case APPLICATION -> 3;
            case LIFECYCLE -> 4;
        };
    }

    private static ZLinkFlowOrigin decodeFlowOrigin(int value) {
        return switch (value) {
            case 1 -> ZLinkFlowOrigin.INBOUND;
            case 2 -> ZLinkFlowOrigin.TIMER;
            case 3 -> ZLinkFlowOrigin.APPLICATION;
            case 4 -> ZLinkFlowOrigin.LIFECYCLE;
            default -> throw new IllegalArgumentException("unknown flow origin: " + value);
        };
    }

    private static EnumSet<ZLinkStreamHeaderFlag> flagsFromValue(int value) {
        EnumSet<ZLinkStreamHeaderFlag> flags =
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class);
        for (ZLinkStreamHeaderFlag flag : ZLinkStreamHeaderFlag.values()) {
            if ((value & flag.value()) != 0) {
                flags.add(flag);
                value &= ~flag.value();
            }
        }
        if (value != 0) {
            throw new IllegalArgumentException("STREAM header contains unknown flags");
        }
        return flags;
    }

    private static int flagsValue(EnumSet<ZLinkStreamHeaderFlag> flags) {
        int value = 0;
        for (ZLinkStreamHeaderFlag flag : flags) {
            value |= flag.value();
        }
        return value;
    }

    private static byte[] encodeMetadata(Map<String, String> metadata) {
        if (metadata == null || metadata.isEmpty()) {
            return new byte[0];
        }
        if (metadata.size() > 255) {
            throw new IllegalArgumentException("STREAM metadata entry count must not exceed 255");
        }
        int size = 1;
        for (Map.Entry<String, String> entry : metadata.entrySet()) {
            byte[] key = entry.getKey().getBytes(StandardCharsets.UTF_8);
            byte[] value = entry.getValue().getBytes(StandardCharsets.UTF_8);
            if (key.length == 0 || key.length > 255) {
                throw new IllegalArgumentException("STREAM metadata key length is invalid");
            }
            if (value.length > 65535) {
                throw new IllegalArgumentException("STREAM metadata value is too large");
            }
            size += 1 + key.length + 2 + value.length;
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

    private static Map<String, String> decodeMetadata(byte[] bytes, int offset, int length) {
        if (length == 0) {
            throw new IllegalArgumentException("STREAM metadata payload is empty");
        }
        ByteBuffer buffer = ByteBuffer.wrap(bytes, offset, length);
        int count = Byte.toUnsignedInt(buffer.get());
        Map<String, String> metadata = new LinkedHashMap<>();
        for (int i = 0; i < count; i++) {
            if (buffer.remaining() < 1) {
                throw new IllegalArgumentException("STREAM metadata key length is missing");
            }
            int keyLength = Byte.toUnsignedInt(buffer.get());
            if (keyLength == 0 || buffer.remaining() < keyLength) {
                throw new IllegalArgumentException("STREAM metadata key is invalid");
            }
            byte[] keyBytes = new byte[keyLength];
            buffer.get(keyBytes);
            if (buffer.remaining() < 2) {
                throw new IllegalArgumentException("STREAM metadata value length is missing");
            }
            int valueLength = Short.toUnsignedInt(buffer.getShort());
            if (buffer.remaining() < valueLength) {
                throw new IllegalArgumentException("STREAM metadata value is invalid");
            }
            byte[] valueBytes = new byte[valueLength];
            buffer.get(valueBytes);
            String key = new String(keyBytes, StandardCharsets.UTF_8);
            String previous = metadata.putIfAbsent(
                key,
                new String(valueBytes, StandardCharsets.UTF_8));
            if (previous != null) {
                throw new IllegalArgumentException("STREAM metadata contains duplicate key");
            }
        }
        if (buffer.hasRemaining()) {
            throw new IllegalArgumentException("STREAM metadata contains trailing bytes");
        }
        return Map.copyOf(metadata);
    }
}
