package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import org.junit.jupiter.api.Test;

import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;
import systems.zlink.framework.runtime.messaging.ZLinkChannelEnvelope;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.AbstractList;
import java.util.List;

final class ZLinkFrameworkMultipartViewEnvelopeTest {
    @Test
    void envelopeProbeReadsTheRetainedHeaderOnce() {
        try (var parts = view("{}")) {
            int[] reads = {0};
            var counted =
                    new AbstractList<Message>() {
                        @Override
                        public Message get(int index) {
                            if (index == 0) {
                                reads[0]++;
                            }
                            return parts.get(index);
                        }

                        @Override
                        public int size() {
                            return parts.size();
                        }
                    };
            assertTrue(ZLinkChannelEnvelope.looksLikeEnvelope(counted));
            assertEquals(1, reads[0]);
        }
    }

    @Test
    void emptyAndRawPartsKeepTheirEnvelopeClassification() {
        assertFalse(ZLinkChannelEnvelope.looksLikeEnvelope(null));
        try (var empty = new ZLinkFrameworkMultipartView(new Object[0], null)) {
            assertFalse(ZLinkChannelEnvelope.looksLikeEnvelope(empty));
        }
        try (var single =
                new ZLinkFrameworkMultipartView(
                        new Object[] {ByteBuffer.wrap(new byte[] {'{'})}, null)) {
            assertFalse(ZLinkChannelEnvelope.looksLikeEnvelope(single));
        }
        for (String first : List.of("", " \t\r\n", "probe", " \t[1]", " \n42")) {
            try (var parts = view(first)) {
                assertFalse(ZLinkChannelEnvelope.looksLikeEnvelope(parts), first);
                assertNull(ZLinkChannelEnvelope.decodeDispatchHeader(parts, false), first);
            }
        }
    }

    @Test
    void whitespacePrefixedHeaderStillDecodesStrictly() {
        try (var parts =
                view(
                        " \t\r\n{\"formatMarker\":"
                                + ZLinkChannelEnvelope.FORMAT_MARKER
                                + ",\"kind\":"
                                + ZLinkChannelEnvelope.KIND_COMMAND
                                + ",\"channelName\":\"orders\",\"messageName\":\"probe\"}")) {
            assertTrue(ZLinkChannelEnvelope.looksLikeEnvelope(parts));
            var header = ZLinkChannelEnvelope.decodeDispatchHeader(parts, false);
            assertEquals("orders", header.channelName());
            assertEquals("probe", header.messageName());
        }
    }

    @Test
    void malformedObjectRemainsAStrictProtocolError() {
        try (var parts = view(" \n{bad json")) {
            assertTrue(ZLinkChannelEnvelope.looksLikeEnvelope(parts));
            var failure =
                    assertThrows(
                            ZLinkFrameworkException.class,
                            () -> ZLinkChannelEnvelope.decodeDispatchHeader(parts, false));
            assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
            assertNull(ZLinkChannelEnvelope.tryDecodeHeader(parts, false));
        }
    }

    @Test
    void closedViewStillRejectsHeaderAccess() {
        try (var parts = view("{}")) {
            parts.close();
            assertThrows(
                    IllegalStateException.class,
                    () -> ZLinkChannelEnvelope.looksLikeEnvelope(parts));
        }
    }

    private static ZLinkFrameworkMultipartView view(String first) {
        return new ZLinkFrameworkMultipartView(
                new Object[] {
                    ByteBuffer.wrap(first.getBytes(StandardCharsets.UTF_8)),
                    ByteBuffer.wrap(new byte[] {1, 2, 3})
                },
                null);
    }
}
