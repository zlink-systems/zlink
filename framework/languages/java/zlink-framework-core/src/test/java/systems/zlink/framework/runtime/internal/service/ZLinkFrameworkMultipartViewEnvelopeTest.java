package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

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
    void headerParserReadsTheRetainedHeaderOnce() {
        try (var parts = view(validHeader())) {
            int[] reads = {0};
            var counted =
                    new AbstractList<Message>() {
                        @Override
                        public Message get(int index) {
                            if (index == 0) reads[0]++;
                            return parts.get(index);
                        }

                        @Override
                        public int size() {
                            return parts.size();
                        }
                    };
            var header = ZLinkChannelEnvelope.decodeDispatchHeader(counted, false);
            assertEquals("orders", header.channelName());
            assertEquals("probe", header.messageName());
            assertEquals(1, reads[0]);
        }
    }

    @Test
    void dispatchAlwaysParsesChannelHeaderBytes() {
        for (byte[] first : List.of(new byte[0], "probe".getBytes(StandardCharsets.UTF_8))) {
            try (var parts = view(first)) {
                ZLinkFrameworkException failure =
                        assertThrows(
                                ZLinkFrameworkException.class,
                                () -> ZLinkChannelEnvelope.decodeDispatchHeader(parts, false));
                assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
            }
        }
    }

    @Test
    void malformedChannelHeaderIsAProtocolError() {
        try (var parts = view(new byte[] {0x08})) {
            ZLinkFrameworkException failure =
                    assertThrows(
                            ZLinkFrameworkException.class,
                            () -> ZLinkChannelEnvelope.decodeDispatchHeader(parts, false));
            assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, failure.kind());
            org.junit.jupiter.api.Assertions.assertNull(
                    ZLinkChannelEnvelope.tryDecodeHeader(parts, false));
        }
    }

    @Test
    void closedViewStillRejectsHeaderAccess() {
        var parts = view(validHeader());
        parts.close();
        assertThrows(
                IllegalStateException.class,
                () -> ZLinkChannelEnvelope.decodeDispatchHeader(parts, false));
    }

    private static ZLinkFrameworkMultipartView view(byte[] first) {
        return new ZLinkFrameworkMultipartView(
                new Object[] {ByteBuffer.wrap(first), ByteBuffer.wrap(new byte[] {1, 2, 3})}, null);
    }

    private static byte[] validHeader() {
        return ZLinkChannelEnvelope.encodeHeaderBytes(
                new ZLinkChannelEnvelope.Header(
                        ZLinkChannelEnvelope.KIND_COMMAND,
                        "orders",
                        "probe",
                        ZLinkChannelEnvelope.DEFAULT_CONTENT_TYPE,
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        java.util.Map.of(),
                        null,
                        null));
    }
}
