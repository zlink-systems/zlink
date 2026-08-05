package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkStreamReceiveBufferTest {
    @Test
    void assemblesAFrameAcrossSeveralRawParts() {
        byte[] header = ZLinkStreamHeaderCodec.encode(header("segmented"));
        byte[] payload = "payload".getBytes(StandardCharsets.UTF_8);
        byte[] frame = ZLinkStreamFrameCodec.encode(header, payload);
        ZLinkStreamReceiveBuffer buffer = new ZLinkStreamReceiveBuffer(1024);

        buffer.append(Arrays.copyOfRange(frame, 0, 2));
        assertNull(buffer.tryTakeFrame());
        buffer.append(Arrays.copyOfRange(frame, 2, 9));
        assertNull(buffer.tryTakeFrame());
        buffer.append(Arrays.copyOfRange(frame, 9, frame.length));

        try (ZLinkStreamInboundFrame decoded = buffer.tryTakeFrame()) {
            assertNotNull(decoded);
            assertArrayEquals(header, decoded.header().toByteArray());
            assertArrayEquals(payload, decoded.payload().toByteArray());
        }
        assertNull(buffer.tryTakeFrame());
    }

    @Test
    void drainsSeveralFramesFromOneRawPartInOrder() {
        byte[] first = ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header("first")),
            new byte[] {1});
        byte[] second = ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header("second")),
            new byte[] {2, 3});
        byte[] combined = new byte[first.length + second.length];
        System.arraycopy(first, 0, combined, 0, first.length);
        System.arraycopy(second, 0, combined, first.length, second.length);
        ZLinkStreamReceiveBuffer buffer = new ZLinkStreamReceiveBuffer(1024);
        buffer.append(combined);

        try (ZLinkStreamInboundFrame decoded = buffer.tryTakeFrame()) {
            assertNotNull(decoded);
            assertEquals("first", decodedHeaderName(decoded));
        }
        try (ZLinkStreamInboundFrame decoded = buffer.tryTakeFrame()) {
            assertNotNull(decoded);
            assertEquals("second", decodedHeaderName(decoded));
        }
        assertNull(buffer.tryTakeFrame());
    }

    @Test
    void rejectsAFrameBeyondTheConfiguredMessageLimit() {
        byte[] frame = ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header("oversize")),
            new byte[] {1, 2, 3, 4});
        // The length is rejected as soon as the complete prefix is available.
        ZLinkStreamReceiveBuffer delayed = new ZLinkStreamReceiveBuffer(4);
        delayed.append(Arrays.copyOf(frame, 6));
        assertThrows(IllegalArgumentException.class, delayed::tryTakeFrame);
    }

    private static String decodedHeaderName(ZLinkStreamInboundFrame frame) {
        return ZLinkStreamHeaderCodec.decodeOrPlain(frame.header().toByteArray())
            .packetName();
    }

    private static ZLinkStreamHeader header(String name) {
        return new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            name,
            Map.of());
    }

    private static void assertEquals(Object expected, Object actual) {
        org.junit.jupiter.api.Assertions.assertEquals(expected, actual);
    }
}
