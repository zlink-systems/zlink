package systems.zlink.framework.runtime.messaging;
import java.nio.charset.StandardCharsets;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;

final class ZLinkFrameworkErrorReplyTest {
    @Test
    void ownsFrameworkErrorReplyEncodingAndDetection() {
        List<Message> parts = ZLinkFrameworkErrorReply.create("route failed");
        try {
            assertEquals(2, parts.size());
            assertTrue(ZLinkFrameworkErrorReply.isReply(parts));
            assertEquals("route failed", ZLinkFrameworkErrorReply.message(parts));
            assertEquals(
                ZLinkFrameworkErrorKind.INTERNAL_FAILURE,
                ZLinkFrameworkErrorReply.kind(parts));
            ZLinkChannelEnvelope.Header header =
                ZLinkChannelEnvelope.decodeHeader(parts.get(0), false);
            assertEquals(ZLinkChannelEnvelope.KIND_ERROR, header.kind());
            assertEquals("internal_failure", header.errorCode());
        } finally {
            parts.forEach(Message::close);
        }
    }

    @Test
    void preservesTypedRequestRejection() {
        List<Message> parts = ZLinkFrameworkErrorReply.create(
            ZLinkFrameworkErrorKind.REJECTED,
            "filter rejected");
        try {
            assertEquals(
                ZLinkFrameworkErrorKind.REJECTED,
                ZLinkFrameworkErrorReply.kind(parts));
            assertEquals(
                "filter rejected",
                ZLinkFrameworkErrorReply.message(parts));
        } finally {
            parts.forEach(Message::close);
        }
    }

    @Test
    void rejectsIncompleteReply() {
        try (Message marker = Message.from("ZLinkFrameworkError".getBytes(
            StandardCharsets.UTF_8))) {
            assertFalse(ZLinkFrameworkErrorReply.isReply(List.of(marker)));
        }
    }

    @Test
    void carriesFrameworkOriginMarkerInReplyMetadata() {
        List<Message> parts = ZLinkFrameworkErrorReply.create(
            ZLinkFrameworkErrorKind.NOT_FOUND,
            "route is stale",
            ZLinkFrameworkErrorOrigin.frameworkMetadata());
        try {
            assertEquals(2, parts.size());
            assertEquals(
                ZLinkFrameworkErrorKind.NOT_FOUND,
                ZLinkFrameworkErrorReply.kind(parts));
            assertEquals(
                java.util.Map.of(
                    ZLinkFrameworkErrorOrigin.METADATA_KEY,
                    ZLinkFrameworkErrorOrigin.FRAMEWORK),
                ZLinkFrameworkErrorReply.metadata(parts));
        } finally {
            parts.forEach(Message::close);
        }
    }

    @Test
    void metadataIsEmptyWhenAbsentOrUnreadable() {
        List<Message> withoutMetadata = ZLinkFrameworkErrorReply.create(
            ZLinkFrameworkErrorKind.NOT_FOUND, "application not found");
        try {
            assertEquals(2, withoutMetadata.size());
            assertTrue(ZLinkFrameworkErrorReply.metadata(withoutMetadata).isEmpty());
        } finally {
            withoutMetadata.forEach(Message::close);
        }

        List<Message> malformed = List.of(
            Message.from("ZLinkFrameworkError".getBytes(StandardCharsets.UTF_8)),
            Message.from("failed".getBytes(StandardCharsets.UTF_8)),
            Message.from("NOT_FOUND".getBytes(StandardCharsets.UTF_8)),
            Message.from(new byte[] {(byte) 0xFF, 0x01}));
        try {
            assertTrue(ZLinkFrameworkErrorReply.metadata(malformed).isEmpty());
        } finally {
            malformed.forEach(Message::close);
        }
    }

    @Test
    void originMarkerDistinguishesFrameworkErrors() {
        assertTrue(ZLinkFrameworkErrorOrigin.isFramework(
            ZLinkFrameworkErrorOrigin.framework(
                ZLinkFrameworkErrorKind.NOT_FOUND, "stale route")));
        assertFalse(ZLinkFrameworkErrorOrigin.isFramework(
            new systems.zlink.framework.errors.ZLinkFrameworkException(
                ZLinkFrameworkErrorKind.NOT_FOUND, "application not found")));
        assertFalse(ZLinkFrameworkErrorOrigin.isFramework(
            new IllegalStateException("not a framework exception")));
    }
}
