package systems.zlink.framework.runtime.binding;

import java.util.EnumSet;
import java.util.List;
import java.util.Optional;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.contracts.messaging.SendOperation;
import systems.zlink.contracts.sockets.SendFlags;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkJavaStreamFraming {
    private ZLinkJavaStreamFraming() {
    }

    static boolean submit(
        SendOperation operation,
        int kind,
        Long requestSeq,
        String packetName,
        List<Message> parts,
        SendFlags flags) {
        StreamPayload payload = streamPayload(packetName, parts);
        Message frame = Message.from(ZLinkStreamFrameCodec.encode(
            new ZLinkStreamHeader(
                ZLinkStreamMessageKind.fromValue(kind),
                ZLinkStreamCodec.RAW,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                Optional.ofNullable(requestSeq),
                payload.packetName(),
                java.util.Map.of()),
            payload.body()));
        try {
            return operation.message(frame).flags(flags).submit();
        } finally {
            frame.close();
        }
    }

    static boolean submit(
        SendOperation operation,
        ZLinkStreamHeader header,
        List<Message> parts,
        SendFlags flags) {
        if (parts == null || parts.size() != 1) {
            throw new IllegalArgumentException("stream frame requires exactly one payload part");
        }
        Message frame = Message.from(ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header),
            parts.get(0).toByteArray()));
        try {
            return operation.message(frame).flags(flags).submit();
        } finally {
            frame.close();
        }
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

    private record StreamPayload(String packetName, byte[] body) { }
}
