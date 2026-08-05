package systems.zlink.framework.runtime.messaging;

import java.nio.charset.StandardCharsets;
import java.util.List;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

/** Owns the internal multipart representation used for framework-generated errors. */
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
        String safeMessage = message == null ? "" : message;
        return List.of(
            Message.from(PACKET_NAME.getBytes(StandardCharsets.UTF_8)),
            Message.from(safeMessage.getBytes(StandardCharsets.UTF_8)),
            Message.from((kind == null
                ? ZLinkFrameworkErrorKind.INTERNAL_FAILURE
                : kind).name().getBytes(StandardCharsets.UTF_8)));
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
}
