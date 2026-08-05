package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.Optional;
import org.junit.jupiter.api.Test;

final class ZLinkStreamFrameCodecTest {
    @Test
    void tryDecodeReturnsHeaderAndBodyFromEncodedFrame() {
        byte[] header = "header".getBytes(java.nio.charset.StandardCharsets.UTF_8);
        byte[] body = "body".getBytes(java.nio.charset.StandardCharsets.UTF_8);

        Optional<ZLinkStreamFrameCodec.DecodedFrame> decoded =
            ZLinkStreamFrameCodec.tryDecode(ZLinkStreamFrameCodec.encode(header, body));

        assertTrue(decoded.isPresent());
        assertArrayEquals(header, decoded.get().header());
        assertArrayEquals(body, decoded.get().body());
    }

    @Test
    void tryDecodeRejectsPartialFrame() {
        byte[] header = "header".getBytes(java.nio.charset.StandardCharsets.UTF_8);
        byte[] body = "body".getBytes(java.nio.charset.StandardCharsets.UTF_8);
        byte[] frame = ZLinkStreamFrameCodec.encode(header, body);
        byte[] partial = java.util.Arrays.copyOf(frame, frame.length - 1);

        assertTrue(ZLinkStreamFrameCodec.tryDecode(partial).isEmpty());
    }
}
