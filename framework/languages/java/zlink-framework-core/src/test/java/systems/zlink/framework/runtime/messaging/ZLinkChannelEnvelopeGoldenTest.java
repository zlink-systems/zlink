package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.UUID;
import java.util.concurrent.Executors;
import java.util.concurrent.Future;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;

/**
 * Pins the Java envelope encoder/decoder to the canonical C++ wire form
 * ({@code runtime/messaging/envelope_codec.cpp},
 * {@code runtime/channels/channel_reply_writer.cpp}): exact JSON field names,
 * the 0xF2 format marker, the message kind values, the flow origin wire
 * integers and the 13 snake_case error code names.
 */
final class ZLinkChannelEnvelopeGoldenTest {
    private static final ObjectMapper JSON = new ObjectMapper();
    private static final String FLOW_ID = "018f2f1d-5d52-7b70-8f08-13fecf6f6abc";

    @Test
    void encodesCanonicalHeaderFieldNamesAndValues() throws Exception {
        ZLinkChannelEnvelope.Header header = new ZLinkChannelEnvelope.Header(
            ZLinkChannelEnvelope.KIND_REQUEST,
            "orders-route",
            "PlaceOrder",
            "application/json",
            "abc123",
            null,
            null,
            null,
            null,
            null,
            Map.of("tenant", "blue"),
            FLOW_ID,
            ZLinkFlowOrigin.APPLICATION);
        try (Message encoded = ZLinkChannelEnvelope.encodeHeader(header)) {
            assertArrayEquals(
                ("{\"formatMarker\":242,\"flowId\":\"" + FLOW_ID
                    + "\",\"flowOrigin\":3,\"kind\":1,\"channelName\":\"orders-route\","
                    + "\"messageName\":\"PlaceOrder\",\"contentType\":\"application/json\","
                    + "\"correlationId\":\"abc123\",\"deadline\":null,\"topic\":null,"
                    + "\"errorCode\":null,\"errorMessage\":null,\"source\":null,"
                    + "\"metadata\":{\"tenant\":\"blue\"}}")
                    .getBytes(StandardCharsets.UTF_8),
                encoded.toByteArray());
            JsonNode json = JSON.readTree(encoded.toByteArray());
            assertEquals(0xF2, json.get("formatMarker").asInt());
            assertEquals(242, json.get("formatMarker").asInt());
            assertEquals(1, json.get("kind").asInt());
            assertEquals("orders-route", json.get("channelName").asText());
            assertEquals("PlaceOrder", json.get("messageName").asText());
            assertEquals("application/json", json.get("contentType").asText());
            assertEquals("abc123", json.get("correlationId").asText());
            assertTrue(json.get("deadline").isNull());
            assertTrue(json.get("topic").isNull());
            assertTrue(json.get("errorCode").isNull());
            assertTrue(json.get("errorMessage").isNull());
            assertTrue(json.get("source").isNull());
            assertEquals("blue", json.get("metadata").get("tenant").asText());
            assertEquals(FLOW_ID, json.get("flowId").asText());
            assertEquals(3, json.get("flowOrigin").asInt());
        }
    }

    @Test
    void requestWithoutFlowKeepsTheExactCanonicalBytes() {
        var header = ZLinkChannelEnvelope.create(
            ZLinkChannelEnvelope.KIND_REQUEST, "orders", "Request", "application/json",
            null, Map.of(), null,
            UUID.fromString("01234567-89ab-cdef-fedc-ba9876543210"));
        try (Message encoded = ZLinkChannelEnvelope.encodeHeader(header)) {
            assertArrayEquals((
                "{\"formatMarker\":242,\"flowId\":null,\"flowOrigin\":null,\"kind\":1,"
                    + "\"channelName\":\"orders\",\"messageName\":\"Request\","
                    + "\"contentType\":\"application/json\","
                    + "\"correlationId\":\"0123456789abcdeffedcba9876543210\","
                    + "\"deadline\":null,\"topic\":null,\"errorCode\":null,"
                    + "\"errorMessage\":null,\"source\":null,\"metadata\":{}}")
                .getBytes(StandardCharsets.UTF_8), encoded.toByteArray());
        }
    }

    @Test
    void repeatedEncodingKeepsTheExactCanonicalBytes() {
        ZLinkChannelEnvelope.Header header = new ZLinkChannelEnvelope.Header(
            ZLinkChannelEnvelope.KIND_REQUEST,
            "orders",
            "Request",
            "application/json",
            "0123456789abcdeffedcba9876543210",
            null,
            null,
            null,
            null,
            null,
            Map.of(),
            null,
            null);
        byte[] expected = (
            "{\"formatMarker\":242,\"flowId\":null,\"flowOrigin\":null,\"kind\":1,"
                + "\"channelName\":\"orders\",\"messageName\":\"Request\","
                + "\"contentType\":\"application/json\","
                + "\"correlationId\":\"0123456789abcdeffedcba9876543210\","
                + "\"deadline\":null,\"topic\":null,\"errorCode\":null,"
                + "\"errorMessage\":null,\"source\":null,\"metadata\":{}}")
            .getBytes(StandardCharsets.UTF_8);

        for (int attempt = 0; attempt < 5; attempt++) {
            try (Message encoded = ZLinkChannelEnvelope.encodeHeader(header)) {
                assertArrayEquals(expected, encoded.toByteArray(), "attempt=" + attempt);
            }
        }
    }

    @Test
    void streamingEncoderPreservesEscapesUnicodeMetadataAndFlowBytes() {
        ZLinkChannelEnvelope.Header header = new ZLinkChannelEnvelope.Header(
            ZLinkChannelEnvelope.KIND_REQUEST,
            "route\"\\\n☃",
            "Place\tOrder\u0001",
            "application/x-test; profile=\"v1\"",
            "0123456789abcdef0123456789abcdef",
            "2030-01-02T03:04:05Z",
            "topic\u2028next",
            null,
            null,
            null,
            Map.of("meta\"\\\n", "blue\t☃"),
            FLOW_ID,
            ZLinkFlowOrigin.TIMER);
        try (Message encoded = ZLinkChannelEnvelope.encodeHeader(header)) {
            assertArrayEquals(
                ("{\"formatMarker\":242,\"flowId\":\"" + FLOW_ID
                    + "\",\"flowOrigin\":2,\"kind\":1,\"channelName\":\"route\\\"\\\\\\n☃\","
                    + "\"messageName\":\"Place\\tOrder\\u0001\","
                    + "\"contentType\":\"application/x-test; profile=\\\"v1\\\"\","
                    + "\"correlationId\":\"0123456789abcdef0123456789abcdef\","
                    + "\"deadline\":\"2030-01-02T03:04:05Z\",\"topic\":\"topic next\","
                    + "\"errorCode\":null,\"errorMessage\":null,\"source\":null,"
                    + "\"metadata\":{\"meta\\\"\\\\\\n\":\"blue\\t☃\"}}")
                    .getBytes(StandardCharsets.UTF_8),
                encoded.toByteArray());
        }
    }

    @Test
    void cachedStableStringsMatchThePreviousStringWriterForUnicodeAndSurrogates()
        throws Exception {
        for (String value : List.of(
                "plain-ascii",
                "quote\\\" slash\\\\ control\\n\\t",
                "snowman-☃",
                "emoji-😀",
                "lone-high-\uD83D",
                "lone-low-\uDE00")) {
            ZLinkChannelEnvelope.Header header = new ZLinkChannelEnvelope.Header(
                ZLinkChannelEnvelope.KIND_REQUEST,
                value,
                value,
                value,
                value,
                value,
                value,
                value,
                value,
                value,
                Map.of("metadata", value),
                null,
                null);
            byte[] expected = encodeWithPreviousStringWriter(header);
            try (Message encoded = ZLinkChannelEnvelope.encodeHeader(header)) {
                assertArrayEquals(expected, encoded.toByteArray(), value);
            }
        }
    }

    @Test
    void explicitOperationIdentityUsesFixedLowercaseCorrelationHex() {
        UUID operationId = UUID.fromString("01234567-89ab-cdef-fedc-ba9876543210");
        ZLinkChannelEnvelope.Header header = ZLinkChannelEnvelope.create(
            ZLinkChannelEnvelope.KIND_REQUEST,
            "route",
            "Request",
            "application/json",
            null,
            Map.of(),
            null,
            operationId);

        assertEquals("0123456789abcdeffedcba9876543210", header.correlationId());
    }

    @Test
    void encodeRetainsTheCallerOwnedPayloadMessage() {
        ZLinkChannelEnvelope.Header header = new ZLinkChannelEnvelope.Header(
            ZLinkChannelEnvelope.KIND_COMMAND,
            "route",
            "Notify",
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
        try (Message payload = Message.from(new byte[] {7, 8, 9})) {
            List<Message> parts = ZLinkChannelEnvelope.encode(header, payload);
            try {
                assertSame(payload, parts.get(1));
                assertArrayEquals(new byte[] {7, 8, 9}, parts.get(1).toByteArray());
            } finally {
                parts.getFirst().close();
            }
        }
    }

    @Test
    void pinsMessageKindValues() {
        assertEquals(1, ZLinkChannelEnvelope.KIND_REQUEST);
        assertEquals(2, ZLinkChannelEnvelope.KIND_RESPONSE);
        assertEquals(3, ZLinkChannelEnvelope.KIND_COMMAND);
        assertEquals(4, ZLinkChannelEnvelope.KIND_PUBLISH);
        assertEquals(5, ZLinkChannelEnvelope.KIND_ERROR);
    }

    @Test
    void pinsFlowOriginWireIntegers() {
        assertEquals(1, ZLinkChannelEnvelope.flowOriginWireValue(ZLinkFlowOrigin.INBOUND));
        assertEquals(2, ZLinkChannelEnvelope.flowOriginWireValue(ZLinkFlowOrigin.TIMER));
        assertEquals(3, ZLinkChannelEnvelope.flowOriginWireValue(ZLinkFlowOrigin.APPLICATION));
        assertEquals(4, ZLinkChannelEnvelope.flowOriginWireValue(ZLinkFlowOrigin.LIFECYCLE));
    }

    @Test
    void errorCodeTableMatchesCppReplyWriterOneToOne() {
        //  channel_reply_writer.cpp:14-45 — exact snake_case names.
        Map<ZLinkFrameworkErrorKind, String> canonical = new LinkedHashMap<>();
        canonical.put(ZLinkFrameworkErrorKind.NOT_FOUND, "not_found");
        canonical.put(ZLinkFrameworkErrorKind.ALREADY_EXISTS, "already_exists");
        canonical.put(ZLinkFrameworkErrorKind.TYPE_MISMATCH, "type_mismatch");
        canonical.put(ZLinkFrameworkErrorKind.NOT_CONFIGURED, "not_configured");
        canonical.put(ZLinkFrameworkErrorKind.REJECTED, "rejected");
        canonical.put(ZLinkFrameworkErrorKind.UNAVAILABLE, "unavailable");
        canonical.put(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, "deadline_exceeded");
        canonical.put(ZLinkFrameworkErrorKind.SHUTTING_DOWN, "shutting_down");
        canonical.put(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "protocol_error");
        canonical.put(ZLinkFrameworkErrorKind.INVALID_OPERATION, "invalid_operation");
        canonical.put(ZLinkFrameworkErrorKind.DATA_LOST, "data_lost");
        canonical.put(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, "internal_failure");
        assertEquals(12, canonical.size());
        assertEquals(ZLinkFrameworkErrorKind.values().length, canonical.size());
        canonical.forEach((kind, name) -> {
            assertEquals(name, ZLinkChannelEnvelope.errorCodeName(kind));
            assertEquals(kind, ZLinkChannelEnvelope.errorKindFromCode(name));
        });
    }

    @Test
    void errorCodeDecodeRejectsNumericMissingAndUnknownPeerKinds() {
        assertEquals(
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ZLinkChannelEnvelope.errorKindFromCode("0"));
        assertEquals(
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ZLinkChannelEnvelope.errorKindFromCode("12"));
        assertEquals(
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ZLinkChannelEnvelope.errorKindFromCode("no_such_code"));
        assertEquals(
            ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
            ZLinkChannelEnvelope.errorKindFromCode(null));
    }

    @Test
    void errorReplyEnvelopeEchoesRequestAndCarriesOriginMarker() throws Exception {
        ZLinkChannelEnvelope.Header request = new ZLinkChannelEnvelope.Header(
            ZLinkChannelEnvelope.KIND_REQUEST,
            "orders-route",
            "PlaceOrder",
            "application/json",
            "corr-77",
            null, null, null, null, null,
            Map.of(),
            null,
            null);
        List<Message> reply = ZLinkFrameworkErrorReply.create(
            request,
            ZLinkFrameworkErrorKind.NOT_FOUND,
            "route is stale",
            ZLinkFrameworkErrorOrigin.frameworkMetadata());
        try {
            assertEquals(2, reply.size());
            JsonNode json = JSON.readTree(reply.get(0).toByteArray());
            assertEquals(0xF2, json.get("formatMarker").asInt());
            assertEquals(5, json.get("kind").asInt());
            assertEquals("orders-route", json.get("channelName").asText());
            assertEquals("PlaceOrder", json.get("messageName").asText());
            assertEquals("corr-77", json.get("correlationId").asText());
            assertEquals("not_found", json.get("errorCode").asText());
            assertEquals("route is stale", json.get("errorMessage").asText());
            assertEquals(
                "framework",
                json.get("metadata").get("zlink.origin").asText());
            //  Round trip through the decoder used by requesters.
            assertEquals(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                ZLinkFrameworkErrorReply.kind(reply));
            assertEquals("route is stale", ZLinkFrameworkErrorReply.message(reply));
            assertEquals(
                Map.of("zlink.origin", "framework"),
                ZLinkFrameworkErrorReply.metadata(reply));
        } finally {
            reply.forEach(Message::close);
        }
    }

    @Test
    void errorCodeThirteenNamesRoundTripThroughErrorReply() {
        for (ZLinkFrameworkErrorKind kind : ZLinkFrameworkErrorKind.values()) {
            List<Message> reply = ZLinkFrameworkErrorReply.create(kind, "boom");
            try {
                assertEquals(kind, ZLinkFrameworkErrorReply.kind(reply));
            } finally {
                reply.forEach(Message::close);
            }
        }
    }

    @Test
    void decodeRejectsJsonParseFailureAsProtocolError() {
        try (Message malformed = Message.from(
                "{not json".getBytes(StandardCharsets.UTF_8))) {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> ZLinkChannelEnvelope.decodeHeader(malformed, false));
            assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
        }
    }

    @Test
    void decodeRejectsFormatMarkerMismatchAsProtocolError() {
        try (Message wrongMarker = Message.from(
                ("{\"formatMarker\":1,\"kind\":1,\"channelName\":\"c\","
                    + "\"messageName\":\"m\",\"contentType\":\"application/json\"}")
                    .getBytes(StandardCharsets.UTF_8))) {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> ZLinkChannelEnvelope.decodeHeader(wrongMarker, false));
            assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
        }
    }

    @Test
    void decodeValidatesFlowPairOnlyWhenCapturing() {
        String header = "{\"formatMarker\":242,\"kind\":1,\"channelName\":\"c\","
            + "\"messageName\":\"m\",\"contentType\":\"application/json\","
            + "\"flowId\":\"not-a-uuid\",\"flowOrigin\":3}";
        try (Message frame = Message.from(header.getBytes(StandardCharsets.UTF_8))) {
            //  Spec 27 §4: at Off the flow fields are ignored entirely.
            assertNull(ZLinkChannelEnvelope.decodeHeader(frame, false).flowId());
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> ZLinkChannelEnvelope.decodeHeader(frame, true));
            assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
        }
        String orphanOrigin = "{\"formatMarker\":242,\"kind\":1,\"channelName\":\"c\","
            + "\"messageName\":\"m\",\"contentType\":\"application/json\","
            + "\"flowOrigin\":3}";
        try (Message frame = Message.from(orphanOrigin.getBytes(StandardCharsets.UTF_8))) {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> ZLinkChannelEnvelope.decodeHeader(frame, true));
            assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
        }
    }

    @Test
    void decodeReadsValidFlowPairWhenCapturing() {
        String header = "{\"formatMarker\":242,\"kind\":2,\"channelName\":\"c\","
            + "\"messageName\":\"m\",\"contentType\":\"application/json\","
            + "\"correlationId\":\"1\",\"flowId\":\"" + FLOW_ID + "\",\"flowOrigin\":1}";
        try (Message frame = Message.from(header.getBytes(StandardCharsets.UTF_8))) {
            ZLinkChannelEnvelope.Header decoded =
                ZLinkChannelEnvelope.decodeHeader(frame, true);
            assertEquals(FLOW_ID, decoded.flowId());
            assertEquals(ZLinkFlowOrigin.INBOUND, decoded.flowOrigin());
            assertEquals("1", decoded.correlationId());
        }
    }

    @Test
    void decodeAcceptsTheCanonicalFieldsInAnotherJsonOrder() {
        String header = "{\"metadata\":{\"tenant\":\"blue\"},\"source\":null,"
            + "\"errorMessage\":null,\"errorCode\":null,\"topic\":null,"
            + "\"deadline\":null,\"correlationId\":\"corr\","
            + "\"contentType\":\"application/json\",\"messageName\":\"Request\","
            + "\"channelName\":\"orders\",\"kind\":1,\"flowOrigin\":null,"
            + "\"flowId\":null,\"formatMarker\":242}";
        try (Message frame = Message.from(header.getBytes(StandardCharsets.UTF_8))) {
            ZLinkChannelEnvelope.Header decoded =
                ZLinkChannelEnvelope.decodeHeader(frame, true);
            assertEquals(ZLinkChannelEnvelope.KIND_REQUEST, decoded.kind());
            assertEquals("orders", decoded.channelName());
            assertEquals("Request", decoded.messageName());
            assertEquals("corr", decoded.correlationId());
            assertEquals(Map.of("tenant", "blue"), decoded.metadata());
            assertNull(decoded.flowId());
            assertNull(decoded.flowOrigin());
        }
    }

    @Test
    void decodeUsesTheLastDuplicateTopLevelValue() {
        String header = "{\"formatMarker\":0,\"formatMarker\":242,"
            + "\"kind\":\"not-an-int\",\"kind\":1,"
            + "\"channelName\":\"orders\",\"messageName\":\"Request\","
            + "\"contentType\":7,\"contentType\":\"application/json\"}";

        ZLinkChannelEnvelope.Header decoded = decode(header, false);

        assertEquals(ZLinkChannelEnvelope.KIND_REQUEST, decoded.kind());
        assertEquals("orders", decoded.channelName());
        assertEquals("Request", decoded.messageName());
        assertEquals("application/json", decoded.contentType());
    }

    @Test
    void decodePreservesOptionalStringAndDefaultContentTypeSemantics() {
        assertEquals(
            ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
            decode("{\"formatMarker\":242,\"kind\":1,\"channelName\":\"c\","
                + "\"messageName\":\"m\"}", false).contentType());
        assertEquals(
            ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
            decode("{\"formatMarker\":242,\"kind\":1,\"channelName\":\"c\","
                + "\"messageName\":\"m\",\"contentType\":null}", false).contentType());

        for (String field : List.of(
                "contentType", "correlationId", "deadline", "topic", "errorCode",
                "errorMessage", "source")) {
            assertProtocolError("{\"formatMarker\":242,\"kind\":1,\"channelName\":\"c\","
                + "\"messageName\":\"m\",\"" + field + "\":7}", false);
        }
    }

    @Test
    void decodeKeepsJacksonIntNodeBoundariesForMarkerAndKind() {
        assertEquals(
            Integer.MAX_VALUE,
            decode("{\"formatMarker\":242,\"kind\":2147483647,\"channelName\":\"c\","
                + "\"messageName\":\"m\"}", false).kind());
        assertEquals(
            Integer.MIN_VALUE,
            decode("{\"formatMarker\":242,\"kind\":-2147483648,\"channelName\":\"c\","
                + "\"messageName\":\"m\"}", false).kind());

        for (String marker : List.of("242.0", "2147483648", "-2147483649")) {
            assertProtocolError("{\"formatMarker\":" + marker + ",\"kind\":1,"
                + "\"channelName\":\"c\",\"messageName\":\"m\"}", false);
        }
        for (String kind : List.of("1.0", "2147483648", "-2147483649")) {
            assertProtocolError("{\"formatMarker\":242,\"kind\":" + kind + ","
                + "\"channelName\":\"c\",\"messageName\":\"m\"}", false);
        }
    }

    @Test
    void decodeIgnoresUnknownNestedFieldsAndMalformedFlowWhileOff() {
        String header = "{\"unknown\":{\"nested\":[1,{\"value\":true}]},"
            + "\"formatMarker\":242,\"kind\":1,\"channelName\":\"c\","
            + "\"messageName\":\"m\",\"flowId\":{\"nested\":true},"
            + "\"flowOrigin\":[3]}";

        ZLinkChannelEnvelope.Header decoded = decode(header, false);

        assertEquals("c", decoded.channelName());
        assertNull(decoded.flowId());
        assertNull(decoded.flowOrigin());
        assertProtocolError(header, true);
    }

    @Test
    void decodeKeepsMetadataDomSemantics() {
        for (String metadata : List.of("null", "7", "[]", "\"text\"")) {
            assertEquals(
                Map.of(),
                decode("{\"formatMarker\":242,\"kind\":1,\"channelName\":\"c\","
                    + "\"messageName\":\"m\",\"metadata\":" + metadata + "}", false)
                    .metadata());
        }
        assertEquals(
            Map.of("tenant", "new"),
            decode("{\"formatMarker\":242,\"kind\":1,\"channelName\":\"c\","
                + "\"messageName\":\"m\",\"metadata\":{\"tenant\":\"old\","
                + "\"tenant\":\"new\"}}", false).metadata());
        assertProtocolError("{\"formatMarker\":242,\"kind\":1,\"channelName\":\"c\","
            + "\"messageName\":\"m\",\"metadata\":{\"tenant\":7}}", false);
    }

    @Test
    void sameStableTupleDoesNotRetainAlternatingDynamicFields() {
        for (int attempt = 0; attempt < 6; attempt++) {
            String correlationId = "corr-" + attempt;
            String deadline = attempt % 2 == 0 ? null : "2030-01-02T03:04:05Z";
            String flowId = attempt % 2 == 0 ? null : FLOW_ID;
            ZLinkFlowOrigin flowOrigin = flowId == null ? null : ZLinkFlowOrigin.APPLICATION;
            Map<String, String> metadata = attempt % 3 == 0
                ? Map.of()
                : Map.of("attempt", String.valueOf(attempt));
            ZLinkChannelEnvelope.Header header = new ZLinkChannelEnvelope.Header(
                ZLinkChannelEnvelope.KIND_REQUEST,
                "orders",
                "Request",
                "application/json",
                correlationId,
                deadline,
                null,
                null,
                null,
                null,
                metadata,
                flowId,
                flowOrigin);

            try (Message encoded = ZLinkChannelEnvelope.encodeHeader(header)) {
                assertArrayEquals(
                    canonicalRequestBytes(correlationId, deadline, metadata, flowId, flowOrigin),
                    encoded.toByteArray(),
                    "attempt=" + attempt);
            }
        }
    }

    @Test
    void aLaterHeaderDoesNotMutateAnEarlierReturnedMessage() {
        Map<String, String> firstMetadata = Map.of("tenant", "blue");
        ZLinkChannelEnvelope.Header first = new ZLinkChannelEnvelope.Header(
            ZLinkChannelEnvelope.KIND_REQUEST,
            "orders",
            "Request",
            "application/json",
            "corr-first",
            null,
            null,
            null,
            null,
            null,
            firstMetadata,
            null,
            null);
        ZLinkChannelEnvelope.Header second = new ZLinkChannelEnvelope.Header(
            ZLinkChannelEnvelope.KIND_REQUEST,
            "orders",
            "Request",
            "application/json",
            "corr-second",
            "2030-01-02T03:04:05Z",
            null,
            null,
            null,
            null,
            Map.of(),
            FLOW_ID,
            ZLinkFlowOrigin.APPLICATION);

        try (Message encodedFirst = ZLinkChannelEnvelope.encodeHeader(first);
             Message encodedSecond = ZLinkChannelEnvelope.encodeHeader(second)) {
            assertArrayEquals(
                canonicalRequestBytes("corr-second", "2030-01-02T03:04:05Z", Map.of(),
                    FLOW_ID, ZLinkFlowOrigin.APPLICATION),
                encodedSecond.toByteArray());
            assertArrayEquals(
                canonicalRequestBytes("corr-first", null, firstMetadata, null, null),
                encodedFirst.toByteArray());
        }
    }

    @Test
    void parallelHeadersWithTheSameStableTupleRemainIndependent() throws Exception {
        List<Future<EncodedHeader>> calls = new ArrayList<>();
        try (var workers = Executors.newVirtualThreadPerTaskExecutor()) {
            for (int sequence = 0; sequence < 32; sequence++) {
                int current = sequence;
                calls.add(workers.submit(() -> {
                    String correlationId = "corr-" + current;
                    String flowId = current % 2 == 0 ? null : FLOW_ID;
                    ZLinkFlowOrigin flowOrigin = flowId == null
                        ? null
                        : ZLinkFlowOrigin.APPLICATION;
                    Map<String, String> metadata = Map.of("sequence", String.valueOf(current));
                    ZLinkChannelEnvelope.Header header = new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_REQUEST,
                        "orders",
                        "Request",
                        "application/json",
                        correlationId,
                        null,
                        null,
                        null,
                        null,
                        null,
                        metadata,
                        flowId,
                        flowOrigin);
                    try (Message encoded = ZLinkChannelEnvelope.encodeHeader(header)) {
                        return new EncodedHeader(
                            canonicalRequestBytes(
                                correlationId, null, metadata, flowId, flowOrigin),
                            encoded.toByteArray());
                    }
                }));
            }
            for (Future<EncodedHeader> call : calls) {
                EncodedHeader result = call.get();
                assertArrayEquals(result.expected(), result.actual());
            }
        }
    }

    @Test
    void invalidHeaderDoesNotContaminateTheNextValidHeader() {
        ZLinkChannelEnvelope.Header invalid = new ZLinkChannelEnvelope.Header(
            ZLinkChannelEnvelope.KIND_REQUEST,
            "orders",
            "Request",
            "application/json",
            "corr-invalid",
            null,
            null,
            null,
            null,
            null,
            Map.of(),
            "not-a-uuid",
            ZLinkFlowOrigin.APPLICATION);

        assertThrows(ZLinkFrameworkException.class,
            () -> ZLinkChannelEnvelope.encodeHeader(invalid));

        try (Message encoded = ZLinkChannelEnvelope.encodeHeader(new ZLinkChannelEnvelope.Header(
                ZLinkChannelEnvelope.KIND_REQUEST,
                "orders",
                "Request",
                "application/json",
                "corr-valid",
                null,
                null,
                null,
                null,
                null,
                Map.of(),
                null,
                null))) {
            assertArrayEquals(
                canonicalRequestBytes("corr-valid", null, Map.of(), null, null),
                encoded.toByteArray());
        }
    }

    private static byte[] canonicalRequestBytes(
        String correlationId,
        String deadline,
        Map<String, String> metadata,
        String flowId,
        ZLinkFlowOrigin flowOrigin) {
        String flow = flowId == null ? "null" : "\"" + flowId + "\"";
        String origin = flowOrigin == null
            ? "null"
            : String.valueOf(ZLinkChannelEnvelope.flowOriginWireValue(flowOrigin));
        String metadataJson = metadata.isEmpty()
            ? "{}"
            : "{\"" + metadata.keySet().iterator().next() + "\":\""
                + metadata.values().iterator().next() + "\"}";
        return ("{\"formatMarker\":242,\"flowId\":" + flow + ",\"flowOrigin\":" + origin
            + ",\"kind\":1,\"channelName\":\"orders\",\"messageName\":\"Request\","
            + "\"contentType\":\"application/json\",\"correlationId\":\"" + correlationId
            + "\",\"deadline\":" + (deadline == null ? "null" : "\"" + deadline + "\"")
            + ",\"topic\":null,\"errorCode\":null,\"errorMessage\":null,\"source\":null,"
            + "\"metadata\":" + metadataJson + "}").getBytes(StandardCharsets.UTF_8);
    }

    private static byte[] encodeWithPreviousStringWriter(ZLinkChannelEnvelope.Header header)
        throws IOException {
        try (ByteArrayOutputStream bytes = new ByteArrayOutputStream();
             JsonGenerator json = JSON.getFactory().createGenerator(bytes)) {
            json.writeStartObject();
            json.writeNumberField("formatMarker", ZLinkChannelEnvelope.FORMAT_MARKER);
            writeNullableString(json, "flowId", header.flowId());
            if (header.flowId() == null) {
                json.writeNullField("flowOrigin");
            } else {
                json.writeNumberField(
                    "flowOrigin", ZLinkChannelEnvelope.flowOriginWireValue(header.flowOrigin()));
            }
            json.writeNumberField("kind", header.kind());
            writeNullableString(json, "channelName", header.channelName());
            writeNullableString(json, "messageName", header.messageName());
            writeNullableString(json, "contentType", header.contentType());
            writeNullableString(json, "correlationId", header.correlationId());
            writeNullableString(json, "deadline", header.deadline());
            writeNullableString(json, "topic", header.topic());
            writeNullableString(json, "errorCode", header.errorCode());
            writeNullableString(json, "errorMessage", header.errorMessage());
            writeNullableString(json, "source", header.source());
            json.writeObjectFieldStart("metadata");
            for (Map.Entry<String, String> entry : header.metadata().entrySet()) {
                json.writeStringField(entry.getKey(), entry.getValue());
            }
            json.writeEndObject();
            json.writeEndObject();
            json.flush();
            return bytes.toByteArray();
        }
    }

    private static void writeNullableString(JsonGenerator json, String field, String value)
        throws IOException {
        json.writeFieldName(field);
        if (value == null) {
            json.writeNull();
        } else {
            json.writeString(value);
        }
    }

    private static ZLinkChannelEnvelope.Header decode(String header, boolean captureFlow) {
        try (Message frame = Message.from(header.getBytes(StandardCharsets.UTF_8))) {
            return ZLinkChannelEnvelope.decodeHeader(frame, captureFlow);
        }
    }

    private static void assertProtocolError(String header, boolean captureFlow) {
        ZLinkFrameworkException failure = assertThrows(
            ZLinkFrameworkException.class,
            () -> decode(header, captureFlow));
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
    }

    private record EncodedHeader(byte[] expected, byte[] actual) {
    }
}
