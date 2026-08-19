package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.charset.StandardCharsets;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
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
        canonical.put(ZLinkFrameworkErrorKind.CAPACITY_EXCEEDED, "capacity_exceeded");
        canonical.put(ZLinkFrameworkErrorKind.DEADLINE_EXCEEDED, "deadline_exceeded");
        canonical.put(ZLinkFrameworkErrorKind.SHUTTING_DOWN, "shutting_down");
        canonical.put(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, "protocol_error");
        canonical.put(ZLinkFrameworkErrorKind.INVALID_OPERATION, "invalid_operation");
        canonical.put(ZLinkFrameworkErrorKind.DATA_LOST, "data_lost");
        canonical.put(ZLinkFrameworkErrorKind.INTERNAL_FAILURE, "internal_failure");
        assertEquals(13, canonical.size());
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
}
