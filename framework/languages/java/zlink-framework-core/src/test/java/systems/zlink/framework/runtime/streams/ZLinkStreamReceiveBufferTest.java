package systems.zlink.framework.runtime.streams;

import org.junit.jupiter.api.Assertions;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.internal.backend.ZLinkBackendStreamReceived;
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
    void retainsOneRawReceiveUntilEveryContributingFrameTerminates() {
        byte[] first = ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header("first")),
            new byte[] {1});
        byte[] second = ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header("second")),
            new byte[] {2});
        byte[] combined = new byte[first.length + second.length];
        System.arraycopy(first, 0, combined, 0, first.length);
        System.arraycopy(second, 0, combined, first.length, second.length);
        Message part = Message.from(combined);
        AtomicInteger closes = new AtomicInteger();
        ZLinkBackendStreamReceived received = new ZLinkBackendStreamReceived(
            Optional.empty(),
            List.of(part),
            () -> {
                part.close();
                closes.incrementAndGet();
            });
        ZLinkStreamReceiveBuffer buffer = new ZLinkStreamReceiveBuffer(1024);
        buffer.append(received.parts(), received);

        ZLinkStreamInboundFrame firstFrame = buffer.tryTakeFrame();
        ZLinkStreamInboundFrame secondFrame = buffer.tryTakeFrame();
        assertNotNull(firstFrame);
        assertNotNull(secondFrame);
        assertEquals(0, closes.get());

        firstFrame.close();
        firstFrame.close();
        assertEquals(0, closes.get());
        secondFrame.close();
        secondFrame.close();
        assertEquals(1, closes.get());
        buffer.close();
        assertEquals(1, closes.get());
    }

    @Test
    void retainsEveryRawReceiveThatContributesToOneFrame() {
        byte[] frame = ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header("split")),
            new byte[] {1, 2, 3});
        int split = frame.length / 2;
        AtomicInteger firstCloses = new AtomicInteger();
        AtomicInteger secondCloses = new AtomicInteger();
        ZLinkBackendStreamReceived first = retained(
            Arrays.copyOfRange(frame, 0, split), firstCloses);
        ZLinkBackendStreamReceived second = retained(
            Arrays.copyOfRange(frame, split, frame.length), secondCloses);
        ZLinkStreamReceiveBuffer buffer = new ZLinkStreamReceiveBuffer(1024);
        buffer.append(first.parts(), first);
        buffer.append(second.parts(), second);

        ZLinkStreamInboundFrame decoded = buffer.tryTakeFrame();
        assertNotNull(decoded);
        assertEquals(0, firstCloses.get());
        assertEquals(0, secondCloses.get());
        decoded.close();
        assertEquals(1, firstCloses.get());
        assertEquals(1, secondCloses.get());
    }

    @Test
    void closesAnIncompleteRawReceiveWhenTheBufferIsDropped() {
        byte[] frame = ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header("partial")),
            new byte[] {1});
        AtomicInteger closes = new AtomicInteger();
        ZLinkBackendStreamReceived received = retained(
            Arrays.copyOf(frame, 4), closes);
        ZLinkStreamReceiveBuffer buffer = new ZLinkStreamReceiveBuffer(1024);
        buffer.append(received.parts(), received);
        assertNull(buffer.tryTakeFrame());

        buffer.close();
        buffer.close();
        assertEquals(1, closes.get());
    }

    @Test
    void rejectsAFrameBeyondTheConfiguredMessageLimit() {
        byte[] frame = ZLinkStreamFrameCodec.encode(
            ZLinkStreamHeaderCodec.encode(header("oversize")),
            new byte[] {1, 2, 3, 4});
        // The length is rejected as soon as the complete prefix is available.
        ZLinkStreamReceiveBuffer delayed = new ZLinkStreamReceiveBuffer(4);
        delayed.append(Arrays.copyOf(frame, 6));
        assertThrows(
            ZLinkStreamMessageTooLargeException.class,
            delayed::tryTakeFrame);
    }

    private static String decodedHeaderName(ZLinkStreamInboundFrame frame) {
        return ZLinkStreamHeaderCodec.decodeOrPlain(frame.header().toByteArray())
            .packetName();
    }

    private static ZLinkBackendStreamReceived retained(
        byte[] bytes,
        AtomicInteger closes) {
        Message part = Message.from(bytes);
        return new ZLinkBackendStreamReceived(
            Optional.empty(),
            List.of(part),
            () -> {
                part.close();
                closes.incrementAndGet();
            });
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
        Assertions.assertEquals(expected, actual);
    }
}
