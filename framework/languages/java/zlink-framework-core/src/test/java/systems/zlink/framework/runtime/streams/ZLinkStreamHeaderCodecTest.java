package systems.zlink.framework.runtime.streams;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.EnumSet;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.streams.ZLinkStreamCodec;
import systems.zlink.framework.streams.ZLinkStreamMessageKind;

final class ZLinkStreamHeaderCodecTest {
    @Test
    void traceCorrelationDoesNotInventValueFromRequestSequence() throws Exception {
        ZLinkStreamHeader request = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(7L),
            "RequestWithoutCorrelation",
            Map.of());
        Class<?> correlations = Class.forName(
            "systems.zlink.framework.runtime.streams.ZLinkStreamCorrelations");
        var method = correlations.getDeclaredMethod("forTrace", ZLinkStreamHeader.class);
        method.setAccessible(true);

        assertEquals(null, method.invoke(null, request));
    }

    @Test
    void encodeDecodePreservesDotnetHeaderFields() {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.of(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED),
            Optional.of(42L),
            "JsonRelayReq",
            Map.of("trace-id", "abc", "tenant", "sample"));

        ZLinkStreamHeader decoded =
            ZLinkStreamHeaderCodec.decodeOrPlain(ZLinkStreamHeaderCodec.encode(header));

        assertEquals(ZLinkStreamMessageKind.REQUEST, decoded.kind());
        assertEquals(ZLinkStreamCodec.JSON, decoded.codec());
        assertEquals(Optional.of(42L), decoded.requestSequence());
        assertEquals("JsonRelayReq", decoded.packetName());
        assertEquals(header.metadata(), decoded.metadata());
        assertTrue(decoded.flags().contains(ZLinkStreamHeaderFlag.HAS_REQUEST_SEQUENCE));
        assertTrue(decoded.flags().contains(ZLinkStreamHeaderFlag.HAS_METADATA));
        assertTrue(decoded.flags().contains(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED));
    }

    @Test
    void headerProtocol_matchesConnectorGoldenVector() {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(7L),
            "Join",
            Map.of("trace", "abc"));

        byte[] encoded = ZLinkStreamHeaderCodec.encode(header);

        assertArrayEquals(hex(
                "f2 02 01 03 00 00 00 00 00 00 00 07 04 4a 6f 69 6e "
                    + "00 0c 01 05 74 72 61 63 65 00 03 61 62 63"),
            encoded);
        assertEquals(header, ZLinkStreamHeaderCodec.decodeOrPlain(encoded));
    }

    @Test
    void encodeDecodePreservesCorrelationId() {
        ZLinkStreamHeader header = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            "MatchBingoReq",
            Map.of(),
            Optional.of("corr-java-bingo"));

        ZLinkStreamHeader decoded =
            ZLinkStreamHeaderCodec.decodeOrPlain(ZLinkStreamHeaderCodec.encode(header));

        assertEquals(ZLinkStreamMessageKind.SEND, decoded.kind());
        assertEquals(ZLinkStreamCodec.JSON, decoded.codec());
        assertEquals(Optional.empty(), decoded.requestSequence());
        assertEquals("MatchBingoReq", decoded.packetName());
        assertEquals(Optional.of("corr-java-bingo"), decoded.correlationId());
        assertTrue(decoded.flags().contains(ZLinkStreamHeaderFlag.HAS_CORRELATION_ID));
    }

    @Test
    void createResponseEchoesRequestSequenceAndCorrelationId() {
        ZLinkStreamHeader request = new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(7L),
            "RequestPacket",
            Map.of(),
            Optional.of("corr-7"));

        ZLinkStreamHeader response = ZLinkStreamHeader.createResponse(
            request,
            ZLinkStreamCodec.JSON,
            EnumSet.of(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED),
            "ReplyPacket",
            Map.of("trace-id", "abc"));

        assertEquals(ZLinkStreamMessageKind.RESPONSE, response.kind());
        assertEquals(Optional.of(7L), response.requestSequence());
        assertEquals(Optional.of("corr-7"), response.correlationId());
        assertEquals("", response.packetName());
        assertTrue(response.flags().contains(ZLinkStreamHeaderFlag.PAYLOAD_COMPRESSED));
        assertTrue(response.flags().contains(ZLinkStreamHeaderFlag.HAS_METADATA));
    }

    @Test
    void headerRejectsWireRuleViolations() {
        assertThrows(IllegalArgumentException.class, () -> new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(7L),
            "Send",
            Map.of()));
        assertThrows(IllegalArgumentException.class, () -> new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            "Request",
            Map.of()));
        assertThrows(IllegalArgumentException.class, () -> new ZLinkStreamHeader(
            ZLinkStreamMessageKind.RESPONSE,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            "Response",
            Map.of()));
        assertThrows(IllegalArgumentException.class, () -> new ZLinkStreamHeader(
            ZLinkStreamMessageKind.ERROR,
            ZLinkStreamCodec.RAW,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(7L),
            "Error",
            Map.of()));
        assertThrows(IllegalArgumentException.class, () -> new ZLinkStreamHeader(
            ZLinkStreamMessageKind.CONTROL,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            "Ping",
            Map.of()));
    }

    @Test
    void headerRejectsInvalidWireLengths() {
        assertThrows(IllegalArgumentException.class, () -> new ZLinkStreamHeader(
            ZLinkStreamMessageKind.REQUEST,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.of(0L),
            "Request",
            Map.of()));
        assertThrows(IllegalArgumentException.class, () -> new ZLinkStreamHeader(
            ZLinkStreamMessageKind.SEND,
            ZLinkStreamCodec.JSON,
            EnumSet.noneOf(ZLinkStreamHeaderFlag.class),
            Optional.empty(),
            "x".repeat(256),
            Map.of()));
    }

    @Test
    void decodeRejectsHeadersThatViolateWireRules() {
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkStreamHeaderCodec.decodeOrPlain(hex("f2 02 01 00 04 4a 6f 69 6e")));
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkStreamHeaderCodec.decodeOrPlain(hex("f2 04 00 01 00 00 00 00 00 00 00 07 05 45 72 72 6f 72")));
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkStreamHeaderCodec.decodeOrPlain(hex(
                "f2 03 01 01 00 00 00 00 00 00 00 07 01 52")));
    }

    private static byte[] hex(String value) {
        String compact = value.replace(" ", "");
        byte[] bytes = new byte[compact.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
            bytes[i] = (byte) Integer.parseInt(compact.substring(i * 2, i * 2 + 2), 16);
        }
        return bytes;
    }
}
