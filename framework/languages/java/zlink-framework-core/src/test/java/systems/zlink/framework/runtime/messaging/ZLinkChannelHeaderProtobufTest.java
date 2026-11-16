package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.io.InputStream;
import java.nio.ByteBuffer;
import java.util.HexFormat;
import java.util.LinkedHashMap;
import java.util.Map;

final class ZLinkChannelHeaderProtobufTest {
    private static final ObjectMapper JSON = new ObjectMapper();

    @Test
    void matchesEverySharedGoldenHeader() throws Exception {
        JsonNode golden = golden();
        for (JsonNode item : golden.path("valid")) {
            byte[] expected = HexFormat.of().parseHex(item.path("hex").asText());
            ZLinkChannelEnvelope.Header expectedHeader = header(item.path("header"));

            assertEquals(
                    expectedHeader,
                    ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(expected), true),
                    item.path("name").asText());
            assertArrayEquals(
                    expected,
                    ZLinkChannelEnvelope.encodeHeaderBytes(expectedHeader),
                    item.path("name").asText());
        }
    }

    @Test
    void rejectsEverySharedGoldenInvalidHeader() throws Exception {
        for (JsonNode item : golden().path("reject")) {
            byte[] bytes = HexFormat.of().parseHex(item.path("hex").asText());
            ZLinkFrameworkException failure =
                    assertThrows(
                            ZLinkFrameworkException.class,
                            () -> ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), true),
                            item.path("name").asText());
            assertEquals(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    failure.kind(),
                    item.path("name").asText());
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

    private static JsonNode golden() throws Exception {
        try (InputStream input =
                ZLinkChannelHeaderProtobufTest.class.getResourceAsStream(
                        "/channel-header-v1.json")) {
            if (input == null) throw new IllegalStateException("channel header golden is missing");
            return JSON.readTree(input);
        }
    }

    private static ZLinkChannelEnvelope.Header header(JsonNode value) {
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
                optional(value, "flow_id"),
                origin == null ? null : ZLinkChannelEnvelope.flowOriginFromWire(origin.asInt()));
    }

    private static String optional(JsonNode value, String name) {
        JsonNode field = value.get(name);
        return field == null || field.isNull() ? null : field.asText();
    }
}
