package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.streams.ZLinkStreamFrameCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeader;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderCodec;
import systems.zlink.framework.runtime.streams.ZLinkStreamHeaderFlag;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkStreamCoreWireInteropTest {
    @Test
    void connectorDecodesAndReencodesCoreHeaderAndFrame() {
        ZLinkStreamHeader coreHeader = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.of(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED),
            Optional.of(7L),
            "Join",
            Map.of("trace", "abc"),
            Optional.of("corr-7"));
        byte[] payload = "{\"ok\":true}".getBytes(StandardCharsets.UTF_8);

        byte[] encodedHeader = ZLinkStreamHeaderCodec.encode(coreHeader);
        ZLinkStreamWireProtocol.Header connectorHeader =
            ZLinkStreamWireProtocol.decodeHeader(encodedHeader);

        assertEquals(ZLinkStreamWireProtocol.KIND_REQUEST, connectorHeader.kind());
        assertEquals(ZLinkStreamWireProtocol.CODEC_JSON, connectorHeader.codec());
        assertTrue((connectorHeader.flags() & ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED) != 0);
        assertEquals(7L, connectorHeader.requestSeq());
        assertEquals("Join", connectorHeader.name());
        assertEquals("abc", connectorHeader.metadata().get("trace"));
        assertEquals("corr-7", connectorHeader.correlationId());
        assertArrayEquals(encodedHeader, ZLinkStreamWireProtocol.encodeHeader(connectorHeader));

        byte[] coreFrame = ZLinkStreamFrameCodec.encode(encodedHeader, payload);
        ZLinkStreamWireProtocol.Frame connectorFrame =
            ZLinkStreamWireProtocol.decodeFrame(coreFrame);

        assertArrayEquals(encodedHeader, connectorFrame.header());
        assertArrayEquals(payload, connectorFrame.payload());
    }

    @Test
    void coreDecodesAndReencodesConnectorHeaderAndFrame() {
        ZLinkStreamWireProtocol.Header connectorHeader = new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_REQUEST,
            ZLinkStreamWireProtocol.CODEC_JSON,
            ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                | ZLinkStreamWireProtocol.FLAG_HAS_METADATA
                | ZLinkStreamWireProtocol.FLAG_PAYLOAD_COMPRESSED,
            7L,
            "Join",
            Map.of("trace", "abc"),
            "corr-7");
        byte[] payload = "{\"ok\":true}".getBytes(StandardCharsets.UTF_8);

        byte[] encodedHeader = ZLinkStreamWireProtocol.encodeHeader(connectorHeader);
        ZLinkStreamHeader coreHeader =
            ZLinkStreamHeaderCodec.decodeOrPlain(encodedHeader);

        assertEquals(ZLinkStreamMessageKind.REQUEST, coreHeader.kind());
        assertEquals(ZLinkStreamCodec.JSON, coreHeader.codec());
        assertTrue(coreHeader.flags().contains(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED));
        assertEquals(Optional.of(7L), coreHeader.requestSequence());
        assertEquals("Join", coreHeader.packetName());
        assertEquals("abc", coreHeader.metadata().get("trace"));
        assertEquals(Optional.of("corr-7"), coreHeader.correlationId());
        assertArrayEquals(encodedHeader, ZLinkStreamHeaderCodec.encode(coreHeader));

        byte[] connectorFrame =
            ZLinkStreamWireProtocol.encodeFrame(encodedHeader, payload, 64 * 1024);
        ZLinkStreamFrameCodec.DecodedFrame coreFrame = ZLinkStreamFrameCodec
            .tryDecode(connectorFrame)
            .orElseThrow();

        assertArrayEquals(encodedHeader, coreFrame.header());
        assertArrayEquals(payload, coreFrame.body());
    }
}
