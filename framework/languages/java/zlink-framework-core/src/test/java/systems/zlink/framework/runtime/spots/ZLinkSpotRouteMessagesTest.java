package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

final class ZLinkSpotRouteMessagesTest {
    private final ZLinkSpotRouteMessages messages =
        new ZLinkSpotRouteMessages(new ZLinkStringMessageSerializer());

    @Test
    void encodesSharedEnvelopeWithoutCopyingPayload() {
        try (Message payload = message("payload")) {
            List<Message> parts = messages.encodeSend(
                "route-channel", Optional.of("packet"), payload,
                ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE, Map.of(), null);
            try {
                assertEquals(2, parts.size());
                assertSame(payload, parts.get(1));
                ZLinkChannelEnvelope.Header header =
                    ZLinkChannelEnvelope.decodeHeader(parts.get(0), true);
                assertEquals(ZLinkChannelEnvelope.KIND_COMMAND, header.kind());
                assertEquals("route-channel", header.channelName());
                assertEquals("packet", header.messageName());
                assertEquals(
                    ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                    header.contentType());
                assertNull(header.correlationId());
                assertNull(header.flowId());
            } finally {
                parts.get(0).close();
            }
        }
    }

    @Test
    void requestEnvelopeCarriesGeneratedCorrelationId() {
        try (Message payload = message("payload")) {
            List<Message> parts = messages.encodeRequest(
                "route-channel", Optional.of("packet"), payload,
                ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE, Map.of(), null);
            try {
                ZLinkChannelEnvelope.Header header =
                    ZLinkChannelEnvelope.decodeHeader(parts.get(0), true);
                assertEquals(ZLinkChannelEnvelope.KIND_REQUEST, header.kind());
                assertFalse(header.correlationId() == null
                    || header.correlationId().isBlank());
            } finally {
                parts.get(0).close();
            }
        }
    }

    @Test
    void emptyPacketNameStaysBareSinglePartPayload() {
        try (Message payload = message("payload")) {
            List<Message> parts = messages.encodeSend(
                "route-channel", Optional.empty(), payload,
                ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE, Map.of(), null);
            assertEquals(1, parts.size());
            assertSame(payload, parts.get(0));
        }
    }

    @Test
    void decodesReplyAndFrameworkError() {
        List<Message> reply = List.of(message("reply"));
        try {
            assertEquals("reply", messages.decodeReply(reply, String.class));
        } finally {
            Message.closeAll(reply);
        }

        List<Message> unavailable = ZLinkFrameworkErrorReply.create(
            ZLinkFrameworkErrorKind.UNAVAILABLE, "route is converging");
        try {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> messages.decodeReply(unavailable, String.class));
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, failure.kind());
            assertEquals("route is converging", failure.getMessage());
        } finally {
            Message.closeAll(unavailable);
        }
    }

    @Test
    void decodesEnvelopeResponseBody() {
        try (Message payload = message("request")) {
            List<Message> request = messages.encodeRequest(
                "route-channel", Optional.of("packet"), payload,
                ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE, Map.of(), null);
            ZLinkChannelEnvelope.Header requestHeader =
                ZLinkChannelEnvelope.decodeHeader(request.get(0), false);
            List<Message> reply = List.of(
                ZLinkChannelEnvelope.encodeHeader(
                    ZLinkChannelEnvelope.reply(requestHeader)),
                message("reply"));
            try {
                assertEquals("reply", messages.decodeReply(reply, String.class));
            } finally {
                Message.closeAll(reply);
                Message.closeAll(List.of(request.get(0)));
            }
        }
    }

    @Test
    void decodedReplyCarriesFrameworkOriginMarkerMetadata() {
        List<Message> stale = ZLinkFrameworkErrorReply.create(
            ZLinkFrameworkErrorKind.NOT_FOUND,
            "route is stale",
            ZLinkFrameworkErrorOrigin.frameworkMetadata());
        try {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> messages.decodeReply(stale, String.class));
            assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, failure.kind());
            assertTrue(ZLinkFrameworkErrorOrigin.isFramework(failure));
        } finally {
            Message.closeAll(stale);
        }

        //  An application handler's preserved NotFound kind must not carry the
        //  framework-origin marker (D5: stale-route control needs both).
        List<Message> application = ZLinkFrameworkErrorReply.create(
            ZLinkFrameworkErrorKind.NOT_FOUND,
            "application entity not found");
        try {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> messages.decodeReply(application, String.class));
            assertEquals(ZLinkFrameworkErrorKind.NOT_FOUND, failure.kind());
            assertFalse(ZLinkFrameworkErrorOrigin.isFramework(failure));
        } finally {
            Message.closeAll(application);
        }
    }

    @Test
    void encodesExplicitFlowStateOnlyWhenPresent() {
        ZLinkFlowContext.State state =
            ZLinkFlowContext.create(ZLinkFlowOrigin.APPLICATION);
        try (Message payload = message("payload")) {
            List<Message> withFlow = messages.encodeSend(
                "route-channel", Optional.of("packet"), payload,
                ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE, Map.of(), state);
            try {
                assertEquals(2, withFlow.size());
                ZLinkFlowContext.State decoded =
                    ZLinkSpotFlowFrame.decode(withFlow);
                assertEquals(state.flowId(), decoded.flowId());
                assertEquals(ZLinkFlowOrigin.APPLICATION, decoded.origin());
            } finally {
                withFlow.get(0).close();
            }

            List<Message> withoutFlow = messages.encodeSend(
                "route-channel", Optional.of("packet"), payload,
                ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE, Map.of(), null);
            try {
                assertEquals(2, withoutFlow.size());
                assertNull(ZLinkSpotFlowFrame.decode(withoutFlow));
            } finally {
                withoutFlow.get(0).close();
            }
        }
    }

    @Test
    void encodesExplicitContentTypeAndMetadataInHeader() {
        try (Message payload = message("payload")) {
            List<Message> parts = messages.encodeSend(
                "route-channel", Optional.of("packet"), payload,
                "application/example", Map.of("tenant", "a"), null);
            try {
                ZLinkChannelEnvelope.Header header =
                    ZLinkChannelEnvelope.decodeHeader(parts.get(0), false);
                assertEquals("application/example", header.contentType());
                assertEquals(Map.of("tenant", "a"), header.metadata());
            } finally {
                parts.get(0).close();
            }
        }
    }

    private static Message message(String value) {
        return Message.from(value.getBytes(StandardCharsets.UTF_8));
    }
}
