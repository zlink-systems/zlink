package systems.zlink.framework.runtime.binding;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

import java.util.EnumSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;

final class ZLinkJavaStreamFraming {
    private static final byte[] HEARTBEAT_PING_FRAME =
            ZLinkStreamFrameCodec.encode(ZLinkStreamHeader.HEARTBEAT_PING, new byte[0]);
    private static final byte[] HEARTBEAT_PONG_FRAME =
            ZLinkStreamFrameCodec.encode(ZLinkStreamHeader.HEARTBEAT_PONG, new byte[0]);

    private ZLinkJavaStreamFraming() {}

    static boolean submit(
            SendOperation operation,
            int kind,
            Long requestSeq,
            String packetName,
            List<Message> parts,
            SendFlags flags) {
        StreamPayload payload = streamPayload(packetName, parts);
        Message frame =
                Message.from(
                        ZLinkStreamFrameCodec.encode(
                                new ZLinkStreamHeader(
                                        ZLinkStreamMessageKind.fromValue(kind),
                                        ZLinkStreamCodec.RAW,
                                        EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                                        Optional.ofNullable(requestSeq),
                                        payload.packetName(),
                                        Map.of()),
                                payload.body()));
        try {
            return ZLinkJavaSocketSupport.submitSync(operation, List.of(frame));
        } finally {
            frame.close();
        }
    }

    static boolean submit(
            SendOperation operation,
            ZLinkStreamHeader header,
            List<Message> parts,
            SendFlags flags) {
        Message frame = frame(header, parts);
        try {
            return ZLinkJavaSocketSupport.submitSync(operation, List.of(frame));
        } finally {
            frame.close();
        }
    }

    static Message frame(ZLinkStreamHeader header, List<Message> parts) {
        if (parts == null || parts.size() != 1) {
            throw new IllegalArgumentException("stream frame requires exactly one payload part");
        }
        Message body = parts.getFirst();
        if (body.size() == 0) {
            if (header == ZLinkStreamHeader.HEARTBEAT_PING)
                return Message.from(HEARTBEAT_PING_FRAME);
            if (header == ZLinkStreamHeader.HEARTBEAT_PONG)
                return Message.from(HEARTBEAT_PONG_FRAME);
        }
        return Message.from(
                ZLinkStreamFrameCodec.encode(
                        ZLinkStreamHeaderCodec.encode(header), body.toByteArray()));
    }

    static Message withActorSlot(List<Message> parts, int actorSlot) {
        if (parts == null || parts.size() != 1) {
            throw new IllegalArgumentException("bound Actor push requires one STREAM frame");
        }
        ZLinkStreamFrameCodec.DecodedFrame decoded =
                ZLinkStreamFrameCodec.tryDecode(parts.getFirst().toByteArray())
                        .orElseThrow(
                                () ->
                                        new IllegalArgumentException(
                                                "bound Actor push is not a STREAM frame"));
        ZLinkStreamHeader header =
                ZLinkStreamHeaderCodec.decodeOrPlain(decoded.header()).withActorSlot(actorSlot);
        return Message.from(ZLinkStreamFrameCodec.encode(header, decoded.body()));
    }

    private static StreamPayload streamPayload(String packetName, List<Message> parts) {
        if (parts == null || parts.isEmpty()) {
            throw new IllegalArgumentException("stream payload requires at least one part");
        }
        if (packetName != null) {
            return new StreamPayload(packetName, parts.get(0).toByteArray());
        }
        if (parts.size() == 1) {
            return new StreamPayload("", parts.get(0).toByteArray());
        }
        return new StreamPayload(parts.get(0).toUtf8String(), parts.get(1).toByteArray());
    }

    private record StreamPayload(String packetName, byte[] body) {}
}
