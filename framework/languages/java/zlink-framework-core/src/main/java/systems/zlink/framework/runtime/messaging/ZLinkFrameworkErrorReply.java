package systems.zlink.framework.runtime.messaging;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

/** Owns the internal multipart representation used for framework-generated errors. */
public final class ZLinkFrameworkErrorReply {
    private static final String PACKET_NAME = "ZLinkFrameworkError";
    private static final int METADATA_PART_INDEX = 3;

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

    /**
     * Encodes a framework error reply with reply metadata as an optional
     * fourth part in the canonical Core application metadata frame format.
     * Decoders that predate the metadata part ignore the extra frame.
     */
    public static List<Message> create(
        ZLinkFrameworkErrorKind kind,
        String message,
        Map<String, String> metadata) {
        String safeMessage = message == null ? "" : message;
        Message name = Message.from(PACKET_NAME.getBytes(StandardCharsets.UTF_8));
        Message text = Message.from(safeMessage.getBytes(StandardCharsets.UTF_8));
        Message kindName = Message.from((kind == null
            ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE
            : kind).name().getBytes(StandardCharsets.UTF_8));
        if (metadata == null || metadata.isEmpty()) {
            return List.of(name, text, kindName);
        }
        return List.of(
            name,
            text,
            kindName,
            Message.from(ZLinkApplicationMetadata.copyOf(metadata).encode()));
    }

    public static boolean isReply(List<Message> parts) {
        return parts != null
            && parts.size() >= 2
            && isPacketName(parts.get(0).toUtf8String());
    }

    public static boolean isPacketName(String packetName) {
        return PACKET_NAME.equals(packetName);
    }

    public static String message(List<Message> parts) {
        if (!isReply(parts)) {
            throw new IllegalArgumentException("parts do not contain a framework error reply");
        }
        return parts.get(1).toUtf8String();
    }

    public static ZLinkFrameworkErrorKind kind(List<Message> parts) {
        if (!isReply(parts)) {
            throw new IllegalArgumentException(
                "parts do not contain a framework error reply");
        }
        if (parts.size() < 3) {
            return ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
        }
        try {
            return ZLinkFrameworkErrorKind.valueOf(parts.get(2).toUtf8String());
        } catch (IllegalArgumentException ignored) {
            return ZLinkFrameworkErrorKind.INTERNAL_FAILURE;
        }
    }

    /**
     * Reads the optional reply metadata part. A reply without the part — or
     * with a part this decoder cannot read — yields an empty map, so a missing
     * or malformed marker never turns into a framework-origin claim.
     */
    public static Map<String, String> metadata(List<Message> parts) {
        if (!isReply(parts) || parts.size() <= METADATA_PART_INDEX) {
            return Map.of();
        }
        try {
            return ZLinkApplicationMetadata.decode(
                parts.get(METADATA_PART_INDEX).toByteArray());
        } catch (IllegalArgumentException ignored) {
            return Map.of();
        }
    }
}
