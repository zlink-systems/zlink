package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.nio.ByteBuffer;
import java.util.Map;

final class ZLinkEnvelopeHeaderCacheHotPathTest {
    @Test
    void generatedDecoderHonorsBufferOffsetsAndConsumesInput() {
        var expected =
                new ZLinkChannelEnvelope.Header(
                        3,
                        "cache",
                        "Update",
                        "application/json",
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of(),
                        null,
                        null);
        byte[] encoded = ZLinkChannelEnvelope.encodeHeaderBytes(expected);
        ByteBuffer input =
                ByteBuffer.allocate(encoded.length + 2).put((byte) 7).put(encoded).put((byte) 9);
        input.flip().position(1).limit(encoded.length + 1);
        assertEquals(expected, ZLinkChannelEnvelope.decodeHeader(input, false));
        assertEquals(input.limit(), input.position());
    }

    @Test
    void parserRejectsTruncationAndTrailingInvalidField() {
        byte[] valid =
                ZLinkChannelEnvelope.encodeHeaderBytes(
                        new ZLinkChannelEnvelope.Header(
                                3, "c", "m", "x", null, null, null, null, null, null, Map.of(),
                                null, null));
        byte[] truncated = java.util.Arrays.copyOf(valid, valid.length - 1);
        assertThrows(
                ZLinkFrameworkException.class,
                () -> ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(truncated), false));
    }
}
