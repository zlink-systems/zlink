package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.RepeatedTest;

import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Map;

final class ZLinkEnvelopeCanonicalReaderTest {
    private static ZLinkChannelEnvelope.Header request() {
        return new ZLinkChannelEnvelope.Header(
                1,
                "probe-channel",
                "probe-message",
                "application/json",
                "canonical-counter",
                null,
                null,
                null,
                null,
                null,
                Map.of(),
                null,
                null);
    }

    @RepeatedTest(100)
    void canonicalHeapStringsAvoidIntermediateCharacterArrays() {
        byte[] header = ZLinkChannelEnvelope.encodeHeaderBytes(request());
        ByteBuffer offset = ByteBuffer.allocate(header.length + 13);
        offset.position(7);
        offset.put(header).flip().position(7);
        ByteBuffer direct = ByteBuffer.allocateDirect(header.length);
        direct.put(header).flip();
        for (ByteBuffer input :
                new ByteBuffer[] {
                    ByteBuffer.wrap(header),
                    offset,
                    offset.slice(),
                    direct,
                    direct.asReadOnlyBuffer()
                }) {
            System.setProperty("envelope.characterArrays", "0");
            System.setProperty("envelope.heapStrings", "0");
            System.setProperty("envelope.parsers", "0");
            assertEquals(request(), ZLinkChannelEnvelope.decodeHeader(input, true));
            if (Boolean.getBoolean("envelope.probe")) {
                assertEquals(0, Integer.getInteger("envelope.parsers", 0));
                assertEquals(
                        input.hasArray() ? 0 : 4,
                        Integer.getInteger("envelope.characterArrays", 0));
                assertEquals(
                        input.hasArray() ? 4 : 0, Integer.getInteger("envelope.heapStrings", 0));
            }
        }
    }

    @RepeatedTest(100)
    void canonicalRequestDoesNotCreateJacksonParser() {
        byte[] bytes = ZLinkChannelEnvelope.encodeHeaderBytes(request());
        System.setProperty("envelope.parsers", "0");
        assertEquals(request(), ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), true));
        if (Boolean.getBoolean("envelope.probe"))
            assertEquals(0, Integer.getInteger("envelope.parsers", 0));
    }

    @RepeatedTest(100)
    void fallbackKeepsWriterSemantics() {
        byte[] bytes = ZLinkChannelEnvelope.encodeHeaderBytes(request());
        String canonical = new String(bytes, StandardCharsets.UTF_8);
        for (String input :
                new String[] {
                    " \n" + canonical,
                    canonical
                            .replace("{\"formatMarker\":242,", "{")
                            .replace("\"metadata\":{}", "\"metadata\":{},\"formatMarker\":242"),
                    canonical.replace("\"metadata\":{}", "\"metadata\":{},\"unknown\":[1,{}]"),
                    canonical.replace(
                            "\"metadata\":{}",
                            "\"metadata\":{},\"unknown\":\"" + "x".repeat(1100) + "😀\""),
                    canonical.replace("probe-message", "probe-\\u006dessage"),
                    canonical.replace(
                            "\"contentType\":\"application/json\"", "\"contentType\":null")
                }) {
            System.setProperty("envelope.parsers", "0");
            assertEquals(
                    request(),
                    ZLinkChannelEnvelope.decodeHeader(
                            ByteBuffer.wrap(input.getBytes(StandardCharsets.UTF_8)), true));
            if (Boolean.getBoolean("envelope.probe"))
                assertEquals(1, Integer.getInteger("envelope.parsers", 0));
        }
    }

    @RepeatedTest(100)
    void bothReadersRejectMalformedHeadersAndRestorePosition() {
        String canonical =
                new String(
                        ZLinkChannelEnvelope.encodeHeaderBytes(request()), StandardCharsets.UTF_8);
        String[] invalid = {
            canonical.substring(0, canonical.length() - 1),
            canonical + "\0",
            canonical.replace("\"kind\":1", "\"kind\":7"),
            canonical.replace("\"formatMarker\":242", "\"formatMarker\":1"),
            canonical.replace("\"messageName\":\"probe-message\"", "\"messageName\":42"),
            canonical.replace("\"channelName\":\"probe-channel\"", "\"channelName\":null"),
            canonical.replace("\"messageName\":\"probe-message\"", "\"messageName\":null"),
            canonical.replace("\"metadata\":{}", "\"metadata\":{\"a\":\"x\",\"a\":\"y\"}"),
            canonical.replace("\"metadata\":{}", "\"metadata\":{},\"kind\":1"),
            canonical.replace("probe-message", "\\uD800"),
            canonical.replace("probe-message", "\\uDC00")
        };
        for (String input : invalid) {
            for (String prefix : new String[] {"", " \n"}) {
                for (int mode = 0; mode < 3; mode++) {
                    ByteBuffer storage =
                            mode == 0
                                    ? ByteBuffer.allocate(input.length() * 4 + 8)
                                    : ByteBuffer.allocateDirect(input.length() * 4 + 8);
                    storage.put((byte) 9)
                            .put((prefix + input).getBytes(StandardCharsets.UTF_8))
                            .flip()
                            .position(1);
                    ByteBuffer bytes =
                            mode == 0
                                    ? storage.slice()
                                    : mode == 1 ? storage : storage.asReadOnlyBuffer();
                    int start = bytes.position();
                    assertThrows(
                            ZLinkFrameworkException.class,
                            () -> ZLinkChannelEnvelope.decodeHeader(bytes, true),
                            input);
                    assertEquals(start, bytes.position(), input);
                }
            }
        }
    }

    @RepeatedTest(100)
    void bufferViewsAndOwnedArraysRemainIsolated() {
        byte[] encoded = ZLinkChannelEnvelope.encodeHeaderBytes(request());
        for (int mode = 0; mode < 3; mode++) {
            ByteBuffer storage =
                    mode == 0
                            ? ByteBuffer.allocate(encoded.length + 4)
                            : ByteBuffer.allocateDirect(encoded.length + 4);
            storage.putInt(123).put(encoded).flip().position(4);
            ByteBuffer input =
                    mode == 0 ? storage.slice() : mode == 1 ? storage : storage.asReadOnlyBuffer();
            assertEquals(request(), ZLinkChannelEnvelope.decodeHeader(input, false));
            assertEquals(input.limit(), input.position());
        }
        encoded[0] = 0;
        assertEquals(
                request(),
                ZLinkChannelEnvelope.decodeHeader(
                        ByteBuffer.wrap(ZLinkChannelEnvelope.encodeHeaderBytes(request())), false));
    }

    @RepeatedTest(100)
    void rejectsInvalidUnicodeEvenInIgnoredFields() {
        String canonical =
                new String(
                        ZLinkChannelEnvelope.encodeHeaderBytes(request()), StandardCharsets.UTF_8);
        for (String bad : new String[] {"\\uD800", "\\uDC00"}) {
            for (String input :
                    new String[] {
                        canonical.replace("probe-message", bad),
                        canonical.replace(
                                "\"metadata\":{}",
                                "\"metadata\":{},\"extension\":[{\"k\":\"" + bad + "\"}]"),
                        canonical.replace("\"flowId\":null", "\"flowId\":\"" + bad + "\"")
                    }) {
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                ZLinkChannelEnvelope.decodeHeader(
                                        ByteBuffer.wrap(input.getBytes(StandardCharsets.UTF_8)),
                                        false),
                        input);
            }
        }
        byte[] valid = canonical.getBytes(StandardCharsets.UTF_8);
        int at = canonical.indexOf("probe-message");
        for (byte[] bad :
                new byte[][] {
                    {(byte) 0xc0, (byte) 0xaf},
                    {(byte) 0xed, (byte) 0xa0, (byte) 0x80},
                    {(byte) 0xf4, (byte) 0x90, (byte) 0x80, (byte) 0x80},
                    {(byte) 0xff}
                }) {
            byte[] bytes = valid.clone();
            System.arraycopy(bad, 0, bytes, at, bad.length);
            assertThrows(
                    ZLinkFrameworkException.class,
                    () -> ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), false));
        }
    }

    @RepeatedTest(100)
    void preparedNamesAvoidImmutableEscapesAndKeepOwnedBytes() {
        var names =
                new ZLinkChannelEnvelope.PreparedNames(
                        "probe-channel", "probe-message", "application/json");
        System.setProperty("envelope.escapes", "0");
        byte[] prepared = ZLinkChannelEnvelope.encodeHeaderBytes(request(), names);
        if (Boolean.getBoolean("envelope.probe"))
            assertEquals(0, Integer.getInteger("envelope.escapes", 0));
        System.setProperty("envelope.escapes", "0");
        assertArrayEquals(ZLinkChannelEnvelope.encodeHeaderBytes(request()), prepared);
        if (Boolean.getBoolean("envelope.probe"))
            assertEquals(6, Integer.getInteger("envelope.escapes", 0));
        prepared[0] = 0;
        assertEquals(
                request(),
                ZLinkChannelEnvelope.decodeHeader(
                        ByteBuffer.wrap(ZLinkChannelEnvelope.encodeHeaderBytes(request(), names)),
                        true));
        var differentNames =
                new ZLinkChannelEnvelope.PreparedNames(
                        "another-channel", "another-message", "application/custom");
        assertArrayEquals(
                ZLinkChannelEnvelope.encodeHeaderBytes(request()),
                ZLinkChannelEnvelope.encodeHeaderBytes(request(), differentNames));
        for (String text : new String[] {"quote\"slash\\", "한글/😀", "\uD800", "\uDC00"}) {
            var header =
                    new ZLinkChannelEnvelope.Header(
                            1,
                            text,
                            text,
                            text,
                            "prepared-escapes",
                            null,
                            null,
                            null,
                            null,
                            null,
                            Map.of(),
                            null,
                            null);
            assertArrayEquals(
                    ZLinkChannelEnvelope.encodeHeaderBytes(header),
                    ZLinkChannelEnvelope.encodeHeaderBytes(
                            header, new ZLinkChannelEnvelope.PreparedNames(text, text, text)));
        }
    }

    @RepeatedTest(100)
    void cppGoldenInputsUseFallbackWithoutChangingTheirHeaders() throws Exception {
        for (String fixture : new String[] {"empty", "full"}) {
            byte[] bytes;
            try (var input = getClass().getResourceAsStream("/envelope/cpp-" + fixture + ".json")) {
                assertNotNull(input);
                bytes = input.readAllBytes();
            }
            var decoded = ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), true);
            var expected =
                    fixture.equals("empty")
                            ? new ZLinkChannelEnvelope.Header(
                                    1,
                                    "",
                                    "",
                                    "application/json",
                                    null,
                                    null,
                                    null,
                                    null,
                                    null,
                                    null,
                                    Map.of(),
                                    null,
                                    null)
                            : new ZLinkChannelEnvelope.Header(
                                    5,
                                    "a\"\\\n\t\b\f\r",
                                    "한글/😀",
                                    "application/octet-stream",
                                    "corr",
                                    "2026-09-10T00:00:00Z",
                                    "topic",
                                    "internal_failure",
                                    "N\0\u001f",
                                    "",
                                    Map.of("a\"", "한글", "z", ""),
                                    "01890a5d-ac96-774b-bcce-b302099a8057",
                                    ZLinkFlowOrigin.INBOUND);
            assertEquals(expected, decoded);
            assertEquals(
                    decoded,
                    ZLinkChannelEnvelope.decodeHeader(
                            ByteBuffer.wrap(ZLinkChannelEnvelope.encodeHeaderBytes(decoded)),
                            true));
            if (fixture.equals("full")) {
                System.setProperty("envelope.parsers", "0");
                ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), false);
                if (Boolean.getBoolean("envelope.probe"))
                    assertEquals(1, Integer.getInteger("envelope.parsers", 0));
            }
        }
    }

    @RepeatedTest(100)
    void flowCaptureAndRejectedReplyIdentifiersShareFinish() {
        var header =
                new ZLinkChannelEnvelope.Header(
                        1,
                        "probe-channel",
                        "probe-message",
                        "application/json",
                        "flow-counter",
                        "deadline",
                        null,
                        null,
                        null,
                        null,
                        Map.of("tenant", "blue"),
                        "018f2f1d-5d52-7b70-8f08-13fecf6f6abc",
                        ZLinkFlowOrigin.APPLICATION);
        String canonical =
                new String(ZLinkChannelEnvelope.encodeHeaderBytes(header), StandardCharsets.UTF_8);
        for (String prefix : new String[] {"", " \n"}) {
            String input = prefix + canonical;
            assertEquals(
                    header,
                    ZLinkChannelEnvelope.decodeHeader(
                            ByteBuffer.wrap(input.getBytes(StandardCharsets.UTF_8)), true));
            var ignored =
                    ZLinkChannelEnvelope.decodeHeader(
                            ByteBuffer.wrap(input.getBytes(StandardCharsets.UTF_8)), false);
            assertNull(ignored.flowId());
            assertNull(ignored.flowOrigin());
            String invalid = input.replace("\"kind\":1", "\"kind\":0");
            var failure =
                    assertThrows(
                            ZLinkChannelEnvelope.HeaderDecodeException.class,
                            () ->
                                    ZLinkChannelEnvelope.decodeHeader(
                                            ByteBuffer.wrap(
                                                    invalid.getBytes(StandardCharsets.UTF_8)),
                                            true));
            assertNotNull(failure.header());
            assertEquals(header.correlationId(), failure.header().correlationId());
            assertEquals(header.channelName(), failure.header().channelName());
            assertEquals(header.messageName(), failure.header().messageName());
            assertNull(failure.header().flowId());
            assertEquals(Map.of(), failure.header().metadata());
            String badFlow = input.replace(header.flowId(), "bad-flow");
            assertThrows(
                    ZLinkFrameworkException.class,
                    () ->
                            ZLinkChannelEnvelope.decodeHeader(
                                    ByteBuffer.wrap(badFlow.getBytes(StandardCharsets.UTF_8)),
                                    true));
            assertNull(
                    ZLinkChannelEnvelope.decodeHeader(
                                    ByteBuffer.wrap(badFlow.getBytes(StandardCharsets.UTF_8)),
                                    false)
                            .flowId());
        }
    }
}
