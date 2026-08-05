package systems.zlink.framework.runtime.streams;

import java.nio.ByteBuffer;
import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

public final class ZLinkStreamFrameCodec {
    private ZLinkStreamFrameCodec() {
    }

    public static byte[] encode(ZLinkStreamHeader header, byte[] body) {
        return encode(ZLinkStreamHeaderCodec.encode(header), body);
    }

    public static byte[] encode(byte[] header, byte[] body) {
        if (header == null) {
            throw new IllegalArgumentException("header is required");
        }
        if (body == null) {
            throw new IllegalArgumentException("body is required");
        }
        if (header.length > 0xFFFF) {
            throw new IllegalArgumentException("stream header exceeds u16 header size");
        }
        ByteBuffer frame = ByteBuffer.allocate(6 + header.length + body.length);
        frame.putShort((short) header.length);
        frame.putInt(body.length);
        frame.put(header);
        frame.put(body);
        return frame.array();
    }

    public static Optional<DecodedFrame> tryDecode(byte[] frame) {
        if (frame == null || frame.length < 6) {
            return Optional.empty();
        }
        ByteBuffer buffer = ByteBuffer.wrap(frame);
        int headerSize = Short.toUnsignedInt(buffer.getShort());
        int payloadSize = buffer.getInt();
        if (payloadSize < 0 || frame.length != 6 + headerSize + payloadSize) {
            return Optional.empty();
        }
        byte[] header = new byte[headerSize];
        buffer.get(header);
        byte[] body = new byte[payloadSize];
        buffer.get(body);
        return Optional.of(new DecodedFrame(header, body));
    }

    public static byte[] encode(
        ZLinkStreamMessageKind kind,
        ZLinkStreamCodec codec,
        Optional<Long> requestSeq,
        String packetName,
        byte[] body) {
        return encode(
            new ZLinkStreamHeader(
                kind,
                codec,
                EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
                requestSeq,
                packetName,
                Map.of()),
            body);
    }

    public record DecodedFrame(byte[] header, byte[] body) {
        public DecodedFrame {
            header = header == null ? null : header.clone();
            body = body == null ? null : body.clone();
        }

        @Override
        public byte[] header() {
            return header.clone();
        }

        @Override
        public byte[] body() {
            return body.clone();
        }
    }
}
