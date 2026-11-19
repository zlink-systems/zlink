package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.nio.ByteBuffer;
import java.util.Map;

final class ZLinkEnvelopeCanonicalReaderTest {
    @Test
    void generatedParserRejectsMalformedInputAndPreservesStartPosition() {
        ByteBuffer input = ByteBuffer.wrap(new byte[] {42, 0x08, (byte) 0x80}).position(1);
        assertThrows(
                ZLinkFrameworkException.class,
                () -> ZLinkChannelEnvelope.decodeHeader(input, false));
        assertEquals(1, input.position());
    }

    @Test
    void generatedParserAcceptsReadOnlyDirectAndSliceBuffers() {
        var header =
                new ZLinkChannelEnvelope.Header(
                        1,
                        "probe",
                        "Request",
                        "application/json",
                        "corr",
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of(),
                        null,
                        null);
        byte[] bytes = ZLinkChannelEnvelope.encodeHeaderBytes(header);
        ByteBuffer direct = ByteBuffer.allocateDirect(bytes.length + 4).putInt(9).put(bytes);
        direct.flip().position(4);
        assertEquals(header, ZLinkChannelEnvelope.decodeHeader(direct.asReadOnlyBuffer(), false));
    }
}
