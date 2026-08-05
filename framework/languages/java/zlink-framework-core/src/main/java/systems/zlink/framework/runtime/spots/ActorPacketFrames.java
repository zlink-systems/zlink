package systems.zlink.framework.runtime.spots;

import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendActorReceived;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.runtime.internal.streams.ZLinkStreamErrorPayload;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

final class ActorPacketFrames {
    private ActorPacketFrames() {
    }

    static Header decode(ZLinkBackendActorReceived headerPart) {
        byte[] bytes = headerPart.message().toByteArray();
        try {
            return decodeStream(bytes, headerPart.requestSeq());
        } catch (RuntimeException ignored) {
            return new Header(
                headerPart.message().toUtf8String(),
                headerPart.requestSeq(),
                false,
                0,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                Map.of(),
                Optional.empty(),
                Optional.empty(),
                Optional.empty());
        }
    }

    static Message encodeReply(Header packetHeader, Message payload) {
        return encodeReply(
            packetHeader,
            payload,
            packetHeader.packetName(),
            ZLinkStreamCodec.fromValue(packetHeader.codec()));
    }

    static Message encodeReply(
        Header packetHeader,
        Message payload,
        String replyPacketName,
        ZLinkStreamCodec replyCodec) {
        if (!packetHeader.streamHeader() || packetHeader.requestSeq().isEmpty()) {
            return Message.from(payload);
        }
        ZLinkStreamHeader replyHeader = ZLinkStreamHeader.createResponse(
            packetHeader.toRequestHeader(),
            replyCodec,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            replyPacketName,
            Map.of());
        return Message.from(ZLinkStreamFrameCodec.encode(replyHeader, payload.toByteArray()));
    }

    static Message encodeError(Header packetHeader, Throwable error) {
        byte[] body = ZLinkStreamErrorPayload.encode(error);
        if (!packetHeader.streamHeader() || packetHeader.requestSeq().isEmpty()) {
            return Message.from(body);
        }
        ZLinkStreamHeader replyHeader =
            ZLinkStreamHeader.createErrorResponse(packetHeader.toRequestHeader(), packetHeader.packetName());
        return Message.from(ZLinkStreamFrameCodec.encode(replyHeader, body));
    }

    private static Header decodeStream(byte[] bytes, Optional<Long> backendRequestSeq) {
        ZLinkStreamHeader header = ZLinkStreamHeaderCodec.decodeOrPlain(bytes);
        if (header.kind() != ZLinkStreamMessageKind.SEND
            && header.kind() != ZLinkStreamMessageKind.REQUEST) {
            throw new IllegalArgumentException("actor STREAM header is not dispatch kind");
        }
        Optional<Long> requestSeq = header.requestSequence().isPresent()
            ? header.requestSequence()
            : backendRequestSeq;
        return new Header(
            header.packetName(),
            requestSeq,
            true,
            header.codec().value(),
            header.flags(),
            header.metadata(),
            header.correlationId(),
            header.flowId(),
            header.flowOrigin());
    }

    record Header(
        String packetName,
        Optional<Long> requestSeq,
        boolean streamHeader,
        int codec,
        EnumSet<ZLinkStreamHeaderFlag> flags,
        Map<String, String> metadata,
        Optional<String> correlationId,
        Optional<String> flowId,
        Optional<ZLinkFlowOrigin> flowOrigin) {
        Header {
            EnumSet<ZLinkStreamHeaderFlag> normalizedFlags =
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class);
            if (flags != null) {
                normalizedFlags.addAll(flags);
            }
            flags = normalizedFlags;
            metadata = metadata == null ? Map.of() : Map.copyOf(metadata);
            correlationId = correlationId == null ? Optional.empty() : correlationId;
            flowId = flowId == null ? Optional.empty() : flowId;
            flowOrigin = flowOrigin == null ? Optional.empty() : flowOrigin;
        }

        ZLinkStreamHeader toRequestHeader() {
            return new ZLinkStreamHeader(
                ZLinkStreamMessageKind.REQUEST,
                ZLinkStreamCodec.fromValue(codec),
                flags,
                requestSeq,
                packetName,
                metadata,
                correlationId,
                flowId,
                flowOrigin);
        }

        ZLinkStreamHeader toStreamHeader() {
            return new ZLinkStreamHeader(
                requestSeq.isPresent() ? ZLinkStreamMessageKind.REQUEST : ZLinkStreamMessageKind.SEND,
                ZLinkStreamCodec.fromValue(codec),
                flags,
                requestSeq,
                packetName,
                metadata,
                correlationId,
                flowId,
                flowOrigin);
        }
    }
}
