package systems.zlink.framework.runtime.actors;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.ZLinkMessageSerializer;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.internal.configuration.ZLinkCodecRegistration;
import systems.zlink.framework.runtime.internal.streams.ZLinkStreamErrorPayload;
import systems.zlink.framework.runtime.messaging.ZLinkMessagePayloads;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.util.List;
import java.util.Objects;
import java.util.Optional;

/** Owns Actor reply framing and errors for typed replies and terminal-only requests. */
final class ZLinkActorReplyDecoder {
    private ZLinkActorReplyDecoder() {}

    static <TReply> TReply decode(
            List<Message> reply, ZLinkMessageSerializer serializer, Class<TReply> replyType) {
        Objects.requireNonNull(replyType, "replyType");
        return decodeCore(reply, replyType, serializer);
    }

    static void decodeTerminal(List<Message> reply) {
        decodeCore(reply, null, null);
    }

    private static <TReply> TReply decodeCore(
            List<Message> reply, Class<TReply> replyType, ZLinkMessageSerializer serializer) {
        if (reply == null || reply.size() != 1) {
            //  Spec 32-framework-error-model:40,83-92 — an unprocessable reply
            //  contract is a ProtocolError, not a configuration error.
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "Actor request reply must contain one STREAM frame.");
        }
        byte[] frame = reply.get(0).toByteArray();
        Optional<ZLinkStreamFrameCodec.DecodedFrame> decoded =
                ZLinkStreamFrameCodec.tryDecode(frame);
        if (decoded.isEmpty()) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "Actor request reply frame is invalid.");
        }
        ZLinkStreamFrameCodec.DecodedFrame decodedFrame = decoded.get();
        Message payload = Message.from(decodedFrame.body());
        try {
            return decodePayload(
                    ZLinkStreamHeaderCodec.decodeOrPlain(decodedFrame.header()),
                    payload,
                    replyType,
                    serializer);
        } catch (IllegalArgumentException invalidFrame) {
            throw new ZLinkFrameworkException(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    "Actor request reply frame is invalid.",
                    invalidFrame);
        } finally {
            payload.close();
        }
    }

    private static <TReply> TReply decodePayload(
            ZLinkStreamHeader header,
            Message payload,
            Class<TReply> replyType,
            ZLinkMessageSerializer serializer) {
        if (header.kind() == ZLinkStreamMessageKind.ERROR) {
            ZLinkStreamErrorPayload.Decoded error =
                    ZLinkStreamErrorPayload.decode(payload.toByteArray());
            if (error.frameworkKind() == null) {
                throw new ZLinkFrameworkException(
                        ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                        "Actor Error reply uses an unknown error code.");
            }
            throw new ZLinkFrameworkException(error.frameworkKind(), error.message());
        }
        if (replyType == null) return null;
        return ZLinkMessagePayloads.deserialize(
                ZLinkCodecRegistration.serializerForReceivedStreamCodec(serializer, header.codec()),
                payload,
                replyType);
    }
}
