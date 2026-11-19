package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import zlink.framework.channel.v1.ChannelHeaderV1.ChannelHeader;
import zlink.framework.channel.v1.ChannelHeaderV1.MetadataEntry;

import java.nio.ByteBuffer;
import java.util.Map;

final class ZLinkClientServerMetadataGoldenTest {
    @Test
    void metadataUsesUtf8ByteOrderAndRoundTripsUnicode() {
        var header =
                new ZLinkChannelEnvelope.Header(
                        3,
                        "work",
                        "Send",
                        "application/json",
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of("é", "snowman-☃ emoji-😀", "a", "first"),
                        null,
                        null);
        byte[] bytes = ZLinkChannelEnvelope.encodeHeaderBytes(header);
        var decoded = ZLinkChannelEnvelope.decodeHeader(ByteBuffer.wrap(bytes), false);
        assertEquals(header.metadata(), decoded.metadata());
        assertEquals(header, decoded);
    }

    @Test
    void metadataEncodedLengthIsLimitedTo1024Bytes() {
        var header =
                new ZLinkChannelEnvelope.Header(
                        3,
                        "work",
                        "Send",
                        "application/json",
                        null,
                        null,
                        null,
                        null,
                        null,
                        null,
                        Map.of("k", "v".repeat(1020)),
                        null,
                        null);
        assertEquals(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                assertThrows(
                                ZLinkFrameworkException.class,
                                () -> ZLinkChannelEnvelope.encodeHeaderBytes(header))
                        .kind());
    }

    @Test
    void malformedMetadataUtf8IsProtocolError() {
        byte[] bytes = {
            0x08,
            3,
            0x12,
            1,
            'c',
            0x1a,
            1,
            'm',
            0x22,
            1,
            'x',
            0x6a,
            5,
            0x0a,
            1,
            (byte) 0xff,
            0x12,
            1,
            'v'
        };
        assertEquals(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                assertThrows(
                                ZLinkFrameworkException.class,
                                () ->
                                        ZLinkChannelEnvelope.decodeHeader(
                                                ByteBuffer.wrap(bytes), false))
                        .kind());
    }

    @Test
    void repeatedMetadataKeysAreRejected() {
        var entry = MetadataEntry.newBuilder().setKey("k").setValue("v").build();
        byte[] bytes =
                ChannelHeader.newBuilder()
                        .setKindValue(3)
                        .setChannelName("c")
                        .setMessageName("m")
                        .setContentType("x")
                        .addMetadata(entry)
                        .addMetadata(entry.toBuilder().setValue("other"))
                        .build()
                        .toByteArray();
        assertEquals(
                ZLinkFrameworkErrorKind.PROTOCOL_ERROR,
                assertThrows(
                                ZLinkFrameworkException.class,
                                () ->
                                        ZLinkChannelEnvelope.decodeHeader(
                                                ByteBuffer.wrap(bytes), false))
                        .kind());
    }
}
