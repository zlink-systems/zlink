package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assertions.fail;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.io.InputStream;
import java.nio.ByteBuffer;
import java.nio.file.Path;
import java.time.Duration;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.concurrent.TimeUnit;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

final class ZLinkChannelHeaderProtobufTest {
    private static final ObjectMapper JSON = new ObjectMapper();
    private static final Pattern ALLOCATION_RESULT = Pattern.compile("^(\\d+) (\\d+)$");

    @Test
    void matchesEverySharedGoldenHeader() throws Exception {
        JsonNode golden = golden();
        for (JsonNode item : golden.path("valid")) {
            byte[] expected = HexFormat.of().parseHex(item.path("hex").asText());
            for (boolean tracing : tracingModes(item)) {
                ZLinkChannelEnvelope.Header expectedHeader = header(item.path("header"), tracing);
                String name = item.path("name").asText() + " tracing=" + tracing;
                assertEquals(
                        expectedHeader,
                        ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(expected), tracing),
                        name);
                if (tracing && item.path("encode").asBoolean(true))
                    assertArrayEquals(
                            expected, ZLinkChannelEnvelope.encodeHeaderBytes(expectedHeader), name);
            }
        }
    }

    @Test
    void rejectsEverySharedGoldenInvalidHeader() throws Exception {
        for (JsonNode item : golden().path("reject")) {
            byte[] bytes = HexFormat.of().parseHex(item.path("hex").asText());
            for (boolean tracing : tracingModes(item)) {
                String name = item.path("name").asText() + " tracing=" + tracing;
                ByteBuffer input = ByteBuffer.wrap(bytes);
                ZLinkFrameworkException failure =
                        assertThrows(
                                ZLinkFrameworkException.class,
                                () -> ZLinkChannelEnvelope.decodeHeader(input, tracing),
                                name);
                assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind(), name);
                assertEquals(0, input.position(), name + " preserves failed buffer position");
            }
        }
    }

    @Test
    void allowsEmptyChannelMessageAndContentTypeValues() {
        var header =
                new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_COMMAND,
                        "",
                        "",
                        "",
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of(),
                        null,
                        null);
        byte[] encoded = ZLinkChannelEnvelope.encodeHeaderBytes(header);

        assertEquals(header, ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(encoded), true));
    }

    @Test
    void decodedMetadataIsAnImmutableSnapshotAndOneWayHeadersHaveNoCorrelation() {
        Map<String, String> callerMetadata = new LinkedHashMap<>();
        callerMetadata.put("tenant", "before");
        var source =
                new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_COMMAND,
                        "orders",
                        "Move",
                        "application/json",
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        callerMetadata,
                        null,
                        null);
        byte[] wire = ZLinkChannelEnvelope.encodeHeaderBytes(source);
        callerMetadata.put("tenant", "after");

        var decoded = ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(wire), false);

        assertNull(decoded.correlationId());
        assertEquals("before", decoded.metadata().get("tenant"));
        assertThrows(
                UnsupportedOperationException.class,
                () -> decoded.metadata().put("tenant", "mutated"));
    }

    @Test
    void rejectedHeaderDoesNotAffectTheNextDecodeAndPreparedNamesKeepWireBytes() throws Exception {
        JsonNode invalid = golden().path("reject").get(0);
        ZLinkFrameworkException failure =
                assertThrows(
                        ZLinkFrameworkException.class,
                        () ->
                                ZLinkChannelEnvelope.decodeHeader(
                                        ByteBuffer.wrap(
                                                HexFormat.of()
                                                        .parseHex(invalid.path("hex").asText())),
                                        true));
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());

        var header =
                new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_COMMAND,
                        "orders",
                        "Move",
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
        var names = new ZLinkChannelEnvelope.PreparedNames("orders", "Move", "application/json");
        byte[] expected = ZLinkChannelEnvelope.encodeHeaderBytes(header);

        assertArrayEquals(expected, ZLinkChannelEnvelope.encodeHeaderBytes(header, names));
        assertEquals(header, ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(expected), false));
        ByteBuffer readOnly = ByteBuffer.wrap(expected).asReadOnlyBuffer();
        assertEquals(header, ZLinkChannelEnvelope.decodeHeader(readOnly, false));
        assertEquals(readOnly.limit(), readOnly.position());
    }

    @Test
    void warmHeaderEncodeAndDecodeStayWithinAllocationCaps() throws Exception {
        String java =
                ProcessHandle.current()
                        .info()
                        .command()
                        .orElseGet(
                                () ->
                                        Path.of(System.getProperty("java.home"), "bin", "java")
                                                .toString());
        Process process =
                new ProcessBuilder(
                                java,
                                "-XX:-DoEscapeAnalysis",
                                "-cp",
                                System.getProperty("java.class.path"),
                                ZLinkChannelHeaderAllocationProbe.class.getName())
                        .start();
        if (!process.waitFor(Duration.ofSeconds(60).toMillis(), TimeUnit.MILLISECONDS)) {
            process.destroyForcibly();
            process.waitFor();
            fail("allocation probe child JVM timed out after 60 seconds");
        }
        String output = new String(process.getInputStream().readAllBytes());
        String error = new String(process.getErrorStream().readAllBytes());
        assertEquals(0, process.exitValue(), "allocation probe child JVM failed: " + error);
        Matcher result = ALLOCATION_RESULT.matcher(output.strip());
        assertTrue(result.matches(), "unexpected allocation probe output: " + output);
        long encodeBytes = Long.parseLong(result.group(1));
        long decodeBytes = Long.parseLong(result.group(2));

        // OpenJDK 25.0.4.1 measured 768,000 encode / 1,240,000 decode bytes per 1,000 calls.
        assertTrue(encodeBytes <= 768_000, "encode allocated " + encodeBytes + " bytes");
        assertTrue(decodeBytes <= 1_240_000, "decode allocated " + decodeBytes + " bytes");
    }

    private static JsonNode golden() throws Exception {
        try (InputStream input =
                ZLinkChannelHeaderProtobufTest.class.getResourceAsStream(
                        "/channel-header-v1.json")) {
            if (input == null) throw new IllegalStateException("channel header golden is missing");
            return JSON.readTree(input);
        }
    }

    private static List<Boolean> tracingModes(JsonNode item) {
        JsonNode tracing = item.get("tracing");
        if (tracing == null) return List.of(true, false);
        return java.util.stream.StreamSupport.stream(tracing.spliterator(), false)
                .map(mode -> mode.asText().equals("on"))
                .toList();
    }

    private static ZLinkChannelEnvelope.Header header(JsonNode value, boolean tracing) {
        Map<String, String> metadata = new LinkedHashMap<>();
        for (JsonNode entry : value.path("metadata")) {
            metadata.put(entry.get(0).asText(), entry.get(1).asText());
        }
        JsonNode origin = value.get("flow_origin");
        return new ZLinkChannelEnvelope.Header(
                value.path("kind").asInt(),
                value.path("channel_name").asText(""),
                value.path("message_name").asText(""),
                value.path("content_type").asText(""),
                optional(value, "correlation_id"),
                optional(value, "deadline"),
                optional(value, "topic"),
                optional(value, "error_code"),
                optional(value, "error_message"),
                optional(value, "source"),
                metadata,
                tracing ? optional(value, "flow_id") : null,
                !tracing || origin == null
                        ? null
                        : ZLinkChannelEnvelope.flowOriginFromWire(origin.asInt()));
    }

    private static String optional(JsonNode value, String name) {
        JsonNode field = value.get(name);
        return field == null || field.isNull() ? null : field.asText();
    }
}
