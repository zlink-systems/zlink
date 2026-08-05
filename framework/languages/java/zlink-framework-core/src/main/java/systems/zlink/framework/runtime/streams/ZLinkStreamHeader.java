package systems.zlink.framework.runtime.streams;

import java.nio.charset.StandardCharsets;
import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

public record ZLinkStreamHeader(
    ZLinkStreamMessageKind kind,
    ZLinkStreamCodec codec,
    EnumSet<ZLinkStreamHeaderFlag> flags,
    Optional<Long> requestSequence,
    String name,
    Map<String, String> metadata,
    // First-class correlation id (flag 0x08, wire layout: after metadata, u8 length +
    // UTF-8 bytes). Client-generated, server-echoed. Empty = absent.
    Optional<String> correlationId,
    Optional<String> flowId,
    Optional<ZLinkFlowOrigin> flowOrigin) {
    private static final int MAX_PACKET_NAME_BYTES = 255;

    public ZLinkStreamHeader {
        if (kind == null) {
            throw new IllegalArgumentException("kind is required");
        }
        if (codec == null) {
            throw new IllegalArgumentException("codec is required");
        }
        EnumSet<ZLinkStreamHeaderFlag> normalizedFlags =
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class);
        if (flags != null) {
            normalizedFlags.addAll(flags);
        }
        flags = normalizedFlags;
        requestSequence = requestSequence == null ? Optional.empty() : requestSequence;
        if (requestSequence.isPresent()) {
            flags.add(ZLinkStreamHeaderFlag.HAS_REQUEST_SEQUENCE);
        } else {
            flags.remove(ZLinkStreamHeaderFlag.HAS_REQUEST_SEQUENCE);
        }
        if (name == null || (!isReplyKind(kind) && name.isBlank())) {
            throw new IllegalArgumentException("name is required");
        }
        if (name.getBytes(StandardCharsets.UTF_8).length > MAX_PACKET_NAME_BYTES) {
            throw new IllegalArgumentException("STREAM packet name is too long");
        }
        metadata = metadata == null ? Map.of() : Map.copyOf(metadata);
        if (metadata.isEmpty()) {
            flags.remove(ZLinkStreamHeaderFlag.HAS_METADATA);
        } else {
            flags.add(ZLinkStreamHeaderFlag.HAS_METADATA);
        }
        correlationId = correlationId == null ? Optional.empty() : correlationId;
        if (correlationId.isPresent() && !correlationId.get().isEmpty()) {
            flags.add(ZLinkStreamHeaderFlag.HAS_CORRELATION_ID);
        } else {
            correlationId = Optional.empty();
            flags.remove(ZLinkStreamHeaderFlag.HAS_CORRELATION_ID);
        }
        flowId = flowId == null ? Optional.empty() : flowId;
        flowOrigin = flowOrigin == null ? Optional.empty() : flowOrigin;
        if (flowId.isPresent() != flowOrigin.isPresent()) {
            throw new IllegalArgumentException("STREAM flow id and origin must be present together");
        }
        if (flowId.isPresent()) {
            validateFlowId(flowId.get());
            flags.add(ZLinkStreamHeaderFlag.HAS_FLOW_ID);
        } else {
            flags.remove(ZLinkStreamHeaderFlag.HAS_FLOW_ID);
        }
        validateKindRules(kind, codec, flags, requestSequence, metadata, correlationId, flowId);
    }

    public ZLinkStreamHeader(
        ZLinkStreamMessageKind kind,
        ZLinkStreamCodec codec,
        EnumSet<ZLinkStreamHeaderFlag> flags,
        Optional<Long> requestSequence,
        String name,
        Map<String, String> metadata,
        Optional<String> correlationId) {
        this(kind, codec, flags, requestSequence, name, metadata, correlationId,
            Optional.empty(), Optional.empty());
    }

    // Back-compat 6-arg constructor (no correlation id).
    public ZLinkStreamHeader(
        ZLinkStreamMessageKind kind,
        ZLinkStreamCodec codec,
        EnumSet<ZLinkStreamHeaderFlag> flags,
        Optional<Long> requestSequence,
        String name,
        Map<String, String> metadata) {
        this(kind, codec, flags, requestSequence, name, metadata, Optional.empty(),
            Optional.empty(), Optional.empty());
    }

    public ZLinkStreamHeader(
        String packetName,
        Map<String, String> metadata,
        Optional<Long> requestSequence) {
        this(
            requestSequence != null && requestSequence.isPresent()
                ? ZLinkStreamMessageKind.REQUEST
                : ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.RAW,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            requestSequence,
            packetName,
            metadata,
            Optional.empty(), Optional.empty(), Optional.empty());
    }

    public String packetName() {
        return name;
    }

    public static ZLinkStreamHeader createResponse(
        ZLinkStreamHeader requestHeader,
        ZLinkStreamCodec codec,
        EnumSet<ZLinkStreamHeaderFlag> flags,
        String packetName,
        Map<String, String> metadata) {
        if (requestHeader == null) {
            throw new IllegalArgumentException("requestHeader is required");
        }
        return new ZLinkStreamHeader(
            ZLinkStreamMessageKind.RESPONSE,
            codec,
            flags,
            requestHeader.requestSequence(),
            "",
            metadata,
            requestHeader.correlationId(), requestHeader.flowId(), requestHeader.flowOrigin());
    }

    public static ZLinkStreamHeader createErrorResponse(
        ZLinkStreamHeader requestHeader,
        String packetName) {
        if (requestHeader == null) {
            throw new IllegalArgumentException("requestHeader is required");
        }
        return new ZLinkStreamHeader(
            ZLinkStreamMessageKind.ERROR,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            requestHeader.requestSequence(),
            "",
            Map.of(),
            requestHeader.correlationId(), requestHeader.flowId(), requestHeader.flowOrigin());
    }

    // Returns a copy of this header carrying the given correlation id (for echoing the
    // request corr onto a reply, or stamping a generated corr on an outbound packet).
    public ZLinkStreamHeader withCorrelationId(String correlationId) {
        return new ZLinkStreamHeader(
            kind, codec, flags, requestSequence, name, metadata,
            correlationId == null ? Optional.empty() : Optional.of(correlationId), flowId, flowOrigin);
    }

    public ZLinkStreamHeader withFlow(String id, ZLinkFlowOrigin origin) {
        return new ZLinkStreamHeader(kind, codec, flags, requestSequence, name, metadata,
            correlationId, Optional.ofNullable(id), Optional.ofNullable(origin));
    }

    private static void validateKindRules(
        ZLinkStreamMessageKind kind,
        ZLinkStreamCodec codec,
        EnumSet<ZLinkStreamHeaderFlag> flags,
        Optional<Long> requestSequence,
        Map<String, String> metadata,
        Optional<String> correlationId,
        Optional<String> flowId) {
        if (requestSequence.isPresent() && requestSequence.get() == 0) {
            throw new IllegalArgumentException("STREAM request sequence must not be zero");
        }
        if (kind == ZLinkStreamMessageKind.SEND && requestSequence.isPresent()) {
            throw new IllegalArgumentException("STREAM send packet must not contain a request sequence");
        }
        if ((kind == ZLinkStreamMessageKind.REQUEST || kind == ZLinkStreamMessageKind.RESPONSE)
            && requestSequence.isEmpty()) {
            throw new IllegalArgumentException(
                "STREAM request and response packets must contain a request sequence");
        }
        if (kind == ZLinkStreamMessageKind.ERROR && codec != ZLinkStreamCodec.JSON) {
            throw new IllegalArgumentException("STREAM error packet must use the JSON codec");
        }
        if (kind == ZLinkStreamMessageKind.CONTROL
            && (codec != ZLinkStreamCodec.RAW
                || !flags.isEmpty()
                || requestSequence.isPresent()
                || !metadata.isEmpty()
                || correlationId.isPresent()
                || flowId.isPresent())) {
            throw new IllegalArgumentException(
                "STREAM control packet must use raw codec and must not contain flags");
        }
    }

    private static boolean isReplyKind(ZLinkStreamMessageKind kind) {
        return kind == ZLinkStreamMessageKind.RESPONSE
            || kind == ZLinkStreamMessageKind.ERROR;
    }

    private static void validateFlowId(String value) {
        if (!value.matches("[0-9a-f]{8}-[0-9a-f]{4}-7[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}")) {
            throw new IllegalArgumentException("STREAM flow id must be a canonical UUIDv7");
        }
    }
}
