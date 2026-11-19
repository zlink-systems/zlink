package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;

import org.junit.jupiter.api.Test;

import java.nio.ByteBuffer;
import java.util.Map;

final class ZLinkEnvelopeCanonicalAllocationTest {
    @Test
    void generatedParserPreservesValuesFromDirectReadOnlyBuffers() {
        var expected =
                new ZLinkChannelEnvelope.Header(
                        2,
                        "allocation-channel",
                        "AllocationMessage",
                        "application/x-protobuf",
                        "00112233445566778899aabbccddeeff",
                        null,
                        "topic-☃",
                        null,
                        null,
                        null,
                        Map.of("tenant", "north"),
                        null,
                        null);
        byte[] encoded = ZLinkChannelEnvelope.encodeHeaderBytes(expected);
        ByteBuffer storage = ByteBuffer.allocateDirect(encoded.length + 11);
        storage.position(7).put(encoded).flip().position(7);
        assertEquals(
                expected, ZLinkChannelEnvelope.decodeHeader(storage.asReadOnlyBuffer(), false));
    }
}
