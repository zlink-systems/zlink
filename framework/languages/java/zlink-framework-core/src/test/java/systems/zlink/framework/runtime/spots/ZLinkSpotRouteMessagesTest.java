package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertSame;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;
import java.util.List;
import java.util.Optional;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.monitoring.ZLinkFlowOrigin;
import systems.zlink.framework.runtime.internal.diagnostics.ZLinkFlowContext;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorOrigin;
import systems.zlink.framework.runtime.messaging.ZLinkFrameworkErrorReply;
import systems.zlink.framework.runtime.channels.ZLinkChannelContentTypeFrame;
import systems.zlink.framework.runtime.messaging.ZLinkStringMessageSerializer;

final class ZLinkSpotRouteMessagesTest {
    private final ZLinkSpotRouteMessages messages =
        new ZLinkSpotRouteMessages(new ZLinkStringMessageSerializer());

    @Test
    void encodesPacketHeaderWithoutCopyingPayload() {
        try (Message payload = message("payload")) {
            List<Message> parts = messages.encode(Optional.of("packet"), payload);
            try {
                assertEquals("packet", parts.get(0).toUtf8String());
                assertSame(payload, parts.get(1));
            } finally {
                parts.get(0).close();
            }
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

        List<Message> error = List.of(
            message("ZLinkFrameworkError"),
            message("failed"));
        try {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> messages.decodeReply(error, String.class));
            assertEquals(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE, failure.kind());
        } finally {
            Message.closeAll(error);
        }

        List<Message> unavailable = ZLinkFrameworkErrorReply.create(
            ZLinkFrameworkErrorKind.UNAVAILABLE, "route is converging");
        try {
            ZLinkFrameworkException failure = assertThrows(
                ZLinkFrameworkException.class,
                () -> messages.decodeReply(unavailable, String.class));
            assertEquals(ZLinkFrameworkErrorKind.UNAVAILABLE, failure.kind());
        } finally {
            Message.closeAll(unavailable);
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
            List<Message> withFlow = messages.encode(
                Optional.of("packet"), payload, state);
            try {
                assertEquals(3, withFlow.size());
                ZLinkFlowContext.State decoded =
                    ZLinkSpotFlowFrame.decode(withFlow);
                assertEquals(state.flowId(), decoded.flowId());
                assertEquals(ZLinkFlowOrigin.APPLICATION, decoded.origin());
            } finally {
                withFlow.get(0).close();
                withFlow.get(2).close();
            }

            List<Message> withoutFlow = messages.encode(
                Optional.of("packet"), payload, (ZLinkFlowContext.State) null);
            try {
                assertEquals(2, withoutFlow.size());
            } finally {
                withoutFlow.get(0).close();
            }
        }
    }

    @Test
    void encodesExplicitContentTypeForRouteTransport() {
        try (Message payload = message("payload")) {
            List<Message> parts = messages.encode(
                Optional.of("packet"), payload, "application/example");
            try {
                assertEquals(
                    "application/example",
                    ZLinkChannelContentTypeFrame.decode(parts));
            } finally {
                parts.get(0).close();
                parts.get(2).close();
            }
        }
    }

    private static Message message(String value) {
        return Message.from(value.getBytes(StandardCharsets.UTF_8));
    }
}
