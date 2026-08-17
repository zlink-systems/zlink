package systems.zlink.framework.runtime.spots;

import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;

/**
 * SPOT route request/send/publish wire codec on the shared cross-language
 * envelope: {@code [JSON header, body]} with content type, application
 * metadata and the flow pair carried as header fields (no separate
 * content-type/flow frames). A call without a packet name stays a bare
 * single-part payload (non-framework publisher semantics, matching the C++
 * fan-out fallback).
 */
final class ZLinkSpotRouteMessages {
    private final ZLinkMessageSerializer serializer;

    ZLinkSpotRouteMessages(ZLinkMessageSerializer serializer) {
        this.serializer = serializer;
    }

    /**
     * One-way send envelope (kind 3). The flow state is an explicitly passed
     * value (R1 value-passing): it is consumed only here at encode time, so
     * callers never install a scope or wrap the stage they return, which
     * keeps the spot dispatch lane's turn stage a bare admission future.
     */
    List<Message> encodeSend(
        String channelName,
        Optional<String> packetName,
        Message payload,
        String contentType,
        Map<String, String> metadata,
        ZLinkFlowContext.State flowState) {
        return encode(
            ZLinkChannelEnvelope.KIND_COMMAND,
            channelName,
            packetName,
            payload,
            contentType,
            null,
            metadata,
            flowState);
    }

    /** Request envelope (kind 1) with a generated correlation id. */
    List<Message> encodeRequest(
        String channelName,
        Optional<String> packetName,
        Message payload,
        String contentType,
        Map<String, String> metadata,
        ZLinkFlowContext.State flowState) {
        return encode(
            ZLinkChannelEnvelope.KIND_REQUEST,
            channelName,
            packetName,
            payload,
            contentType,
            null,
            metadata,
            flowState);
    }

    /** Publish envelope (kind 4) with the topic carried in the header. */
    List<Message> encodePublish(
        String channelName,
        String topic,
        Optional<String> packetName,
        Message payload,
        String contentType,
        Map<String, String> metadata,
        ZLinkFlowContext.State flowState) {
        return encode(
            ZLinkChannelEnvelope.KIND_PUBLISH,
            channelName,
            packetName,
            payload,
            contentType,
            topic,
            metadata,
            flowState);
    }

    private List<Message> encode(
        int kind,
        String channelName,
        Optional<String> packetName,
        Message payload,
        String contentType,
        String topic,
        Map<String, String> metadata,
        ZLinkFlowContext.State flowState) {
        if (packetName.isEmpty()) {
            return List.of(payload);
        }
        return ZLinkChannelEnvelope.encode(
            ZLinkChannelEnvelope.create(
                kind,
                channelName,
                packetName.orElseThrow(),
                contentType,
                topic,
                metadata,
                flowState),
            payload);
    }

    <TReply> TReply decodeReply(List<Message> replyParts, Class<TReply> replyType) {
        ZLinkChannelEnvelope.Header header =
            ZLinkChannelEnvelope.tryDecodeHeader(replyParts, false);
        if (header != null && header.isError()) {
            //  The envelope error reply already carries the public kind;
            //  preserve it instead of collapsing to InternalFailure. The
            //  header metadata (zlink.origin marker for framework-generated
            //  errors) travels with the exception so stale-route control
            //  can require kind + marker.
            throw new ZLinkFrameworkException(
                ZLinkChannelEnvelope.errorKindFromCode(header.errorCode()),
                header.errorMessage(),
                null,
                header.metadata());
        }
        Message emptyReply = null;
        try {
            Message body = header != null
                ? replyParts.get(1)
                : replyParts.isEmpty()
                    ? (emptyReply = Message.from(new byte[0]))
                    : replyParts.get(replyParts.size() - 1);
            try {
                return ZLinkMessagePayloads.deserialize(serializer, body, replyType);
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
