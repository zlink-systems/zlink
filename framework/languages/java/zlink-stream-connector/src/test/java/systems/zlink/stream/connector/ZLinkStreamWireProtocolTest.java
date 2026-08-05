package systems.zlink.stream.connector;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.Map;
import org.junit.jupiter.api.Test;

final class ZLinkStreamWireProtocolTest {
    @Test
    void headerProtocol_matchesDotnetAndNodeGoldenVector() {
        Map<String, String> metadata = new LinkedHashMap<>();
        metadata.put("trace", "abc");
        ZLinkStreamWireProtocol.Header header = new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_REQUEST,
            ZLinkStreamWireProtocol.CODEC_JSON,
            ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                | ZLinkStreamWireProtocol.FLAG_HAS_METADATA,
            7L,
            "Join",
            metadata,
            null);

        byte[] encoded = ZLinkStreamWireProtocol.encodeHeader(header);

        assertArrayEquals(hex(
                "f2 02 01 03 00 00 00 00 00 00 00 07 04 4a 6f 69 6e "
                    + "00 0c 01 05 74 72 61 63 65 00 03 61 62 63"),
            encoded);

        ZLinkStreamWireProtocol.Header decoded = ZLinkStreamWireProtocol.decodeHeader(encoded);

        assertEquals(ZLinkStreamWireProtocol.KIND_REQUEST, decoded.kind());
        assertEquals(ZLinkStreamWireProtocol.CODEC_JSON, decoded.codec());
        assertEquals(7L, decoded.requestSeq());
        assertEquals("Join", decoded.name());
        assertEquals("abc", decoded.metadata().get("trace"));
    }

    // MFLOW-009: correlation id is a first-class header trailer (flag 0x08), wire layout
    // = after metadata, u8 length + UTF-8 bytes. Round-trips and is byte-exact.
    @Test
    void headerProtocol_roundTripsCorrelationIdAfterMetadata() {
        Map<String, String> metadata = new LinkedHashMap<>();
        metadata.put("k", "v");
        ZLinkStreamWireProtocol.Header header = new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_REQUEST,
            ZLinkStreamWireProtocol.CODEC_JSON,
            ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ
                | ZLinkStreamWireProtocol.FLAG_HAS_METADATA,
            7L,
            "order.place",
            metadata,
            "a1b2");

        byte[] encoded = ZLinkStreamWireProtocol.encodeHeader(header);
        ZLinkStreamWireProtocol.Header decoded = ZLinkStreamWireProtocol.decodeHeader(encoded);

        assertEquals("a1b2", decoded.correlationId());
        assertEquals((byte) 4, encoded[encoded.length - 5]);
        assertArrayEquals(
            "a1b2".getBytes(StandardCharsets.UTF_8),
            java.util.Arrays.copyOfRange(encoded, encoded.length - 4, encoded.length));
    }

    @Test
    void frameProtocol_matchesDotnetAndNodePrefixLayout() {
        byte[] header = hex("01 00 00 05 52 65 61 64 79");
        byte[] payload = "{\"ok\":true}".getBytes(StandardCharsets.UTF_8);

        byte[] frame = ZLinkStreamWireProtocol.encodeFrame(header, payload, 64 * 1024);

        assertArrayEquals(hex("00 09 00 00 00 0b"), java.util.Arrays.copyOf(frame, 6));
        ZLinkStreamWireProtocol.Frame decoded = ZLinkStreamWireProtocol.decodeFrame(frame);
        assertArrayEquals(header, decoded.header());
        assertArrayEquals(payload, decoded.payload());
    }

    @Test
    void frameProtocol_rejectsPayloadAboveReceiveLimitBeforeBodyAllocation() {
        byte[] prefixOnly = hex("00 00 00 00 00 02");

        assertThrows(IllegalArgumentException.class, () ->
            ZLinkStreamWireProtocol.decodeFrame(prefixOnly, 1));
    }

    @Test
    void headerProtocol_rejectsRequestWithoutRequestSeq() {
        ZLinkStreamWireProtocol.Header header = new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_REQUEST,
            ZLinkStreamWireProtocol.CODEC_JSON,
            ZLinkStreamWireProtocol.FLAG_HAS_METADATA,
            null,
            "Join",
            Map.of("trace", "abc"),
            null);

        assertThrows(IllegalArgumentException.class, () ->
            ZLinkStreamWireProtocol.encodeHeader(header));
    }

    @Test
    void headerProtocol_acceptsMetadataAtTotalLimitAndRejectsOneByteMore() {
        assertEquals(1024, encodedMetadataLength(1019));
        ZLinkStreamWireProtocol.encodeHeader(headerWithMetadataValue("v".repeat(1019)));

        assertEquals(1025, encodedMetadataLength(1020));
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkStreamWireProtocol.encodeHeader(headerWithMetadataValue("v".repeat(1020))));
    }

    @Test
    void headerProtocol_omitsReplyNameAndRejectsLegacyReplyName() {
        ZLinkStreamWireProtocol.Header response = new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_RESPONSE,
            ZLinkStreamWireProtocol.CODEC_JSON,
            ZLinkStreamWireProtocol.FLAG_HAS_REQUEST_SEQ,
            7L,
            "",
            Map.of(),
            null);

        byte[] encoded = ZLinkStreamWireProtocol.encodeHeader(response);
        ZLinkStreamWireProtocol.Header decoded = ZLinkStreamWireProtocol.decodeHeader(encoded);

        assertEquals("", decoded.name());

        byte[] legacy = hex("f2 03 01 01 00 00 00 00 00 00 00 07 01 52");
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkStreamWireProtocol.decodeHeader(legacy));
    }

    private static byte[] hex(String value) {
        String compact = value.replace(" ", "");
        byte[] bytes = new byte[compact.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
            bytes[i] = (byte) Integer.parseInt(compact.substring(i * 2, i * 2 + 2), 16);
        }
        return bytes;
    }

    private static ZLinkStreamWireProtocol.Header headerWithMetadataValue(String value) {
        return new ZLinkStreamWireProtocol.Header(
            ZLinkStreamWireProtocol.KIND_SEND,
            ZLinkStreamWireProtocol.CODEC_JSON,
            ZLinkStreamWireProtocol.FLAG_HAS_METADATA,
            null,
            "MetadataLimit",
            Map.of("k", value),
            null);
    }

    private static int encodedMetadataLength(int valueLength) {
        return 1 + 1 + 1 + 2 + valueLength;
    }
}
