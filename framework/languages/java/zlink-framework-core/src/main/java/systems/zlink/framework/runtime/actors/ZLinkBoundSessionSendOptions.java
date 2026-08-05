package systems.zlink.framework.runtime.actors;

import java.util.EnumSet;
import java.util.HashMap;
import java.util.Map;
import java.util.Optional;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

record ZLinkBoundSessionSendOptions(
    String defaultPacketName,
    Map<String, String> metadata,
    Optional<String> packetName,
    ZLinkStreamCodec codec) {
    static ZLinkBoundSessionSendOptions create(
        String defaultPacketName,
        ZLinkStreamCodec codec) {
        return new ZLinkBoundSessionSendOptions(
            defaultPacketName,
            Map.of(),
            Optional.empty(),
            codec);
    }

    ZLinkBoundSessionSendOptions withPacketName(String packetName) {
        if (packetName == null || packetName.isBlank()) {
            throw new IllegalArgumentException("packetName is required");
        }
        return new ZLinkBoundSessionSendOptions(
            defaultPacketName,
            metadata,
            Optional.of(packetName),
            codec);
    }

    ZLinkBoundSessionSendOptions withMetadata(String key, String value) {
        Map<String, String> next = new HashMap<>(metadata);
        next.put(key, value);
        return new ZLinkBoundSessionSendOptions(
            defaultPacketName,
            Map.copyOf(next),
            packetName,
            codec);
    }

    ZLinkStreamHeader header() {
        return new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            codec,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            packetName.orElse(defaultPacketName),
            metadata);
    }

    byte[] encodeFrame(Message payload) {
        return ZLinkStreamFrameCodec.encode(header(), payload.toByteArray());
    }
}
