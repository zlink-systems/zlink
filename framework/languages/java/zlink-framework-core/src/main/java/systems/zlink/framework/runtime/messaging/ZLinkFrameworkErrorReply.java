package systems.zlink.framework.runtime.messaging;

import java.util.List;
import java.util.Map;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

/**
 * Framework-generated error replies on the shared cross-language envelope
 * wire: a kind-5 envelope whose header carries the snake_case
 * {@code errorCode}, the {@code errorMessage} and the reply metadata object
 * (including the {@code zlink.origin=framework} marker for framework-origin
 * errors). The legacy raw-parts representation
 * {@code ["ZLinkFrameworkError", message, KIND, metadata?]} is gone; encode
 * and decode operate on the envelope only.
 */
public final class ZLinkFrameworkErrorReply {
    private static final String PACKET_NAME = "ZLinkFrameworkError";

    private ZLinkFrameworkErrorReply() {
    }

    public static List<Message> create(String message) {
        return create(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, message);
    }

    public static List<Message> create(
        ZLinkFrameworkErrorKind kind,
        String message) {
        return create(kind, message, Map.of());
    }

    /** Error envelope without a request context (no correlation echo). */
    public static List<Message> create(
        ZLinkFrameworkErrorKind kind,
        String message,
        Map<String, String> metadata) {
        return create(null, kind, message, metadata);
    }

    /**
     * Error envelope echoing the request header's channel/message names and
     * correlation id, per the C++ reply writer
     * ({@code channel_reply_writer.cpp} {@code create_error_header}).
     */
    public static List<Message> create(
        ZLinkChannelEnvelope.Header request,
        ZLinkFrameworkErrorKind kind,
        String message,
        Map<String, String> metadata) {
        return ZLinkChannelEnvelope.encode(
            ZLinkChannelEnvelope.error(request, kind, message, metadata),
            Message.from(new byte[0]));
    }

    public static boolean isReply(List<Message> parts) {
        ZLinkChannelEnvelope.Header header =
            ZLinkChannelEnvelope.tryDecodeHeader(parts, false);
        return header != null && header.isError();
    }

    /**
     * Legacy stray-error marker name. Envelope error replies are detected via
     * the header kind; this remains only for callers that inspect a raw
     * first-frame packet name.
     */
    public static boolean isPacketName(String packetName) {
        return PACKET_NAME.equals(packetName);
    }

    public static String message(List<Message> parts) {
        ZLinkChannelEnvelope.Header header = requireErrorHeader(parts);
        return header.errorMessage() == null ? "" : header.errorMessage();
    }

    public static ZLinkFrameworkErrorKind kind(List<Message> parts) {
        ZLinkChannelEnvelope.Header header = requireErrorHeader(parts);
        return ZLinkChannelEnvelope.errorKindFromCode(header.errorCode());
    }

    /**
     * Reply metadata from the envelope header. A reply without metadata — or
     * parts that are not an error envelope at all — yields an empty map, so a
     * missing or malformed marker never turns into a framework-origin claim.
     */
    public static Map<String, String> metadata(List<Message> parts) {
        ZLinkChannelEnvelope.Header header =
            ZLinkChannelEnvelope.tryDecodeHeader(parts, false);
        return header == null || !header.isError() ? Map.of() : header.metadata();
    }

    private static ZLinkChannelEnvelope.Header requireErrorHeader(List<Message> parts) {
        ZLinkChannelEnvelope.Header header =
            ZLinkChannelEnvelope.tryDecodeHeader(parts, false);
        if (header == null || !header.isError()) {
            throw new IllegalArgumentException(
                "parts do not contain a framework error reply");
        }
        return header;
    }
}
