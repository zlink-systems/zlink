package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.*;

import com.fasterxml.jackson.databind.ObjectMapper;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import java.nio.charset.StandardCharsets;
import java.nio.file.Path;
import java.util.LinkedHashMap;
import java.util.Map;

final class ZLinkClientServerMetadataGoldenTest {
    @Test
    void receivedHeaderRejectsInvalidUtf8AsProtocolError() {
        byte[] prefix =
                "{\"formatMarker\":242,\"kind\":3,\"channelName\":\"api\",\"messageName\":\""
                        .getBytes(StandardCharsets.UTF_8);
        byte[] suffix =
                "\",\"contentType\":\"application/json\",\"metadata\":{}}"
                        .getBytes(StandardCharsets.UTF_8);
        byte[] bytes = new byte[prefix.length + 2 + suffix.length];
        System.arraycopy(prefix, 0, bytes, 0, prefix.length);
        bytes[prefix.length] = (byte) 0xc3;
        bytes[prefix.length + 1] = 0x28;
        System.arraycopy(suffix, 0, bytes, prefix.length + 2, suffix.length);
        try (var wire = Message.from(bytes)) {
            assertEquals(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    assertThrows(
                                    ZLinkFrameworkException.class,
                                    () -> ZLinkChannelEnvelope.decodeHeader(wire, false))
                            .kind());
        }
    }

    @Test
    void metadataWritesUnicodeAsUtf8AndRejectsLoneSurrogates() {
        var header =
                ZLinkChannelEnvelope.create(
                        3,
                        "work",
                        "Send",
                        "application/json",
                        null,
                        Map.of("k", "snowman-☃ emoji-😀"),
                        null,
                        null);
        try (var encoded = ZLinkChannelEnvelope.encodeHeader(header)) {
            assertTrue(
                    encoded.toUtf8String().contains("\"metadata\":{\"k\":\"snowman-☃ emoji-😀\"}"));
            assertEquals(
                    header.metadata(),
                    ZLinkChannelEnvelope.decodeHeader(encoded, false).metadata());
        }
        for (String invalid : java.util.List.of("\uD800", "\uDC00")) {
            var outgoing =
                    ZLinkChannelEnvelope.create(
                            3,
                            "work",
                            "Send",
                            "application/json",
                            null,
                            Map.of("k", invalid),
                            null,
                            null);
            assertEquals(
                    ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                    assertThrows(
                                    ZLinkFrameworkException.class,
                                    () -> ZLinkChannelEnvelope.encodeHeader(outgoing))
                            .kind());
        }
    }

    @Test
    void sharedMetadataFixturesEncodeAndDecodeWithCanonicalSize() throws Exception {
        var json = new ObjectMapper();
        var cases =
                json.readTree(
                                Path.of(
                                                "../../../runtime/protocol/fixtures/client-server-metadata.json")
                                        .toFile())
                        .get("cases");
        for (var fixture : cases) {
            String name = fixture.get("name").asText();
            var header =
                    json.createObjectNode()
                            .put("formatMarker", 242)
                            .put("kind", 3)
                            .put("channelName", "work")
                            .put("messageName", "Send")
                            .put("contentType", "application/json");
            if (fixture.has("metadata")) header.set("metadata", fixture.get("metadata"));
            try (var part =
                    Message.from(
                            fixture.has("receivedEncoded")
                                    ? (header.toString()
                                                            .substring(
                                                                    0,
                                                                    header.toString()
                                                                            .lastIndexOf(
                                                                                    "\"metadata\":"))
                                                    + "\"metadata\":"
                                                    + fixture.get("receivedEncoded").asText()
                                                    + "}")
                                            .getBytes(StandardCharsets.UTF_8)
                                    : json.writeValueAsBytes(header))) {
                if (!fixture.get("valid").asBoolean()) {
                    var error =
                            assertThrows(
                                    ZLinkFrameworkException.class,
                                    () -> ZLinkChannelEnvelope.decodeHeader(part, false),
                                    name);
                    assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, error.kind(), name);
                    if (fixture.has("metadata")
                            && fixture.get("metadata").isObject()
                            && java.util.stream.StreamSupport.stream(
                                            java.util.Spliterators.spliteratorUnknownSize(
                                                    fixture.get("metadata").elements(), 0),
                                            false)
                                    .allMatch(com.fasterxml.jackson.databind.JsonNode::isTextual)) {
                        Map<String, String> metadata = new LinkedHashMap<>();
                        fixture.get("metadata")
                                .fields()
                                .forEachRemaining(
                                        entry ->
                                                metadata.put(
                                                        entry.getKey(), entry.getValue().asText()));
                        var outgoing =
                                ZLinkChannelEnvelope.create(
                                        3,
                                        "work",
                                        "Send",
                                        "application/json",
                                        null,
                                        metadata,
                                        null,
                                        null);
                        assertThrows(
                                ZLinkFrameworkException.class,
                                () -> ZLinkChannelEnvelope.encodeHeader(outgoing),
                                name);
                    }
                    continue;
                }
                var decoded = ZLinkChannelEnvelope.decodeHeader(part, false);
                try (var encoded = ZLinkChannelEnvelope.encodeHeader(decoded)) {
                    String text = encoded.toUtf8String();
                    String metadata =
                            text.substring(text.indexOf("\"metadata\":") + 11, text.length() - 1);
                    if (decoded.metadata().size() <= 1) {
                        assertEquals(fixture.get("encoded").asText(), metadata, name);
                    } else {
                        assertEquals(
                                json.readTree(fixture.get("encoded").asText()),
                                json.readTree(metadata),
                                name);
                        for (var entry : decoded.metadata().entrySet()) {
                            assertTrue(
                                    metadata.contains(
                                            json.writeValueAsString(entry.getKey())
                                                    + ":"
                                                    + json.writeValueAsString(entry.getValue())),
                                    name);
                        }
                    }
                    assertEquals(
                            fixture.get("encodedSize").asInt(),
                            metadata.getBytes(StandardCharsets.UTF_8).length,
                            name);
                }
                if (name.equals("utf8-1024")) {
                    try (var escaped = Message.from(part.toUtf8String().replace("é", "\\u00e9"))) {
                        assertEquals(
                                decoded.metadata(),
                                ZLinkChannelEnvelope.decodeHeader(escaped, false).metadata());
                    }
                }
            }
        }
    }
}
