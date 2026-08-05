package systems.zlink.framework.runtime.channels;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.util.List;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;

final class ZLinkChannelReplyDecoder {
    private static final ObjectMapper JSON = new ObjectMapper();
    private final ZLinkMessageSerializer serializer;

    ZLinkChannelReplyDecoder(ZLinkMessageSerializer serializer) {
        this.serializer = serializer;
    }

    <TReply> TReply decode(
        List<Message> replies,
        Class<TReply> replyType,
        String failurePrefix) {
        if (replies.isEmpty()) {
            try (Message emptyReply = Message.from(new byte[0])) {
                return ZLinkMessagePayloads.deserialize(serializer, emptyReply, replyType);
            }
        }
        RuntimeException lastError = null;
        for (int index = replies.size() - 1; index >= 0; index--) {
            Message reply = replies.get(index);
            try {
                return ZLinkMessagePayloads.deserialize(serializer, reply, replyType);
            } catch (RuntimeException directError) {
                lastError = directError;
                try {
                    JsonNode root = JSON.readTree(reply.toByteArray());
                    JsonNode ok = root.get("ok");
                    JsonNode response = root.get("response");
                    if (ok == null || !ok.asBoolean(false) || response == null) {
                        continue;
                    }
                    try (Message responseMessage = Message.from(JSON.writeValueAsBytes(response))) {
                        return ZLinkMessagePayloads.deserialize(
                            serializer,
                            responseMessage,
                            replyType);
                    }
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
