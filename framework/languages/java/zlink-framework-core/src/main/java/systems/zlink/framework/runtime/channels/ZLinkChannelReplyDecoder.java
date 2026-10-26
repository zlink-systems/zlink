package systems.zlink.framework.runtime.channels;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.service.ZLinkFrameworkMultipartView;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;

import java.util.List;

final class ZLinkChannelReplyDecoder {
    private static final ObjectMapper JSON = new ObjectMapper();
    private final ZLinkMessageSerializer serializer;

    ZLinkChannelReplyDecoder(ZLinkMessageSerializer serializer) {
        this.serializer = serializer;
    }

    <TReply> TReply decode(List<Message> replies, Class<TReply> replyType, String failurePrefix) {
        return decode(
                replies,
                systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.tryDecodeHeader(
                        replies, false),
                replyType,
                failurePrefix);
    }

    /** Decodes with the header the caller already read; {@code null} means no envelope. */
    <TReply> TReply decode(
            List<Message> replies,
            systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope.Header header,
            Class<TReply> replyType,
            String failurePrefix) {
        if (header != null) {
            if (header.isError()) {
                throw new ZLinkFrameworkException(
                        systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope
                                .errorKindFromCode(header.errorCode()),
                        header.errorMessage(),
                        null,
                        header.metadata());
            }
            return ZLinkMessagePayloads.deserialize(
                    serializer, ZLinkFrameworkMultipartView.buffer(replies, 1), replyType);
        }
        if (replies.isEmpty()) {
            return ZLinkMessagePayloads.deserialize(
                    serializer, java.nio.ByteBuffer.allocate(0), replyType);
        }
        RuntimeException lastError = null;
        for (int index = replies.size() - 1; index >= 0; index--) {
            Message reply = replies.get(index);
            try {
                return ZLinkMessagePayloads.deserialize(serializer, reply.dataBuffer(), replyType);
            } catch (RuntimeException directError) {
                lastError = directError;
                try {
                    JsonNode root = JSON.readTree(reply.toByteArray());
                    JsonNode ok = root.get("ok");
                    JsonNode response = root.get("response");
                    if (ok == null || !ok.asBoolean(false) || response == null) {
                        continue;
                    }
                    return ZLinkMessagePayloads.deserialize(
                            serializer,
                            java.nio.ByteBuffer.wrap(JSON.writeValueAsBytes(response)),
                            replyType);
                } catch (Exception envelopeError) {
                    directError.addSuppressed(envelopeError);
                }
            }
        }
        throw new ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                failurePrefix + "; first reply frame=" + replies.get(0).toUtf8String(),
                lastError);
    }
}
