package systems.zlink.framework.runtime.spots;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Optional;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;

final class ZLinkSpotRouteMessages {
    private final ZLinkMessageSerializer serializer;

    ZLinkSpotRouteMessages(ZLinkMessageSerializer serializer) {
        this.serializer = serializer;
    }

    List<Message> encode(Optional<String> packetName, Message payload) {
        return encode(packetName, payload, ZLinkFlowContext.current());
    }

    List<Message> encode(
        Optional<String> packetName,
        Message payload,
        String contentType) {
        return encode(packetName, payload, contentType, ZLinkFlowContext.current());
    }

    /**
     * Encodes with an explicitly passed flow state (R1 value-passing). The
     * state is consumed only here at encode time; callers never install a
     * scope or wrap the stage they return, which keeps the spot dispatch
     * lane's turn stage a bare admission future.
     */
    List<Message> encode(
        Optional<String> packetName,
        Message payload,
        ZLinkFlowContext.State flowState) {
        if (packetName.isEmpty()) return List.of(payload);
        Message flow = ZLinkSpotFlowFrame.encode(flowState);
        return flow == null
            ? List.of(Message.from(packetName.orElseThrow().getBytes(StandardCharsets.UTF_8)), payload)
            : List.of(Message.from(packetName.orElseThrow().getBytes(StandardCharsets.UTF_8)), payload, flow);
    }

    List<Message> encode(
        Optional<String> packetName,
        Message payload,
        String contentType,
        ZLinkFlowContext.State flowState) {
        if (contentType == null || packetName.isEmpty()) {
            return encode(packetName, payload, flowState);
        }
        Message packet = Message.from(
            packetName.orElseThrow().getBytes(StandardCharsets.UTF_8));
        Message contentTypeFrame = ZLinkChannelContentTypeFrame.encode(contentType);
        Message flow = ZLinkSpotFlowFrame.encode(flowState);
        return flow == null
            ? List.of(packet, payload, contentTypeFrame)
            : List.of(packet, payload, contentTypeFrame, flow);
    }

    <TReply> TReply decodeReply(List<Message> replyParts, Class<TReply> replyType) {
        Message emptyReply = null;
        try {
            if (isFrameworkErrorReply(replyParts)) {
                //  The framework-error reply already carries the public kind;
                //  preserve it instead of collapsing to InternalFailure. The
                //  reply metadata (zlink.origin marker for framework-generated
                //  errors) travels with the exception so stale-route control
                //  can require kind + marker.
                throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorReply.kind(replyParts),
                    ZLinkFrameworkErrorReply.message(replyParts),
                    null,
                    ZLinkFrameworkErrorReply.metadata(replyParts));
            }
            Message firstReply = replyParts.isEmpty()
                ? (emptyReply = Message.from(new byte[0]))
                : replyParts.get(replyParts.size() - 1);
            try {
                return ZLinkMessagePayloads.deserialize(serializer, firstReply, replyType);
            } catch (IllegalArgumentException ex) {
                throw new IllegalArgumentException(
                    ex.getMessage()
                        + " (spot route reply parts="
                        + describe(replyParts)
                        + ")",
                    ex);
            }
        } finally {
            if (emptyReply != null) {
                emptyReply.close();
            }
        }
    }

    private static boolean isFrameworkErrorReply(List<Message> parts) {
        return ZLinkFrameworkErrorReply.isReply(parts);
    }

    private static String describe(List<Message> parts) {
        List<String> descriptions = new ArrayList<>(parts.size());
        for (Message part : parts) {
            byte[] bytes = part.toByteArray();
            String text = new String(
                bytes,
                0,
                Math.min(bytes.length, 64),
                StandardCharsets.UTF_8)
                .replace("\n", "\\n")
                .replace("\r", "\\r");
            descriptions.add(bytes.length + ":" + text);
        }
        return descriptions.toString();
    }
}
