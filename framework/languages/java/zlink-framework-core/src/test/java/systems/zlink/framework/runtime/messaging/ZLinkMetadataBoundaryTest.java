package systems.zlink.framework.runtime.messaging;

import static org.junit.jupiter.api.Assertions.*;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.function.Executable;

import systems.zlink.framework.errors.ZLinkFrameworkErrorKind;
import systems.zlink.framework.errors.ZLinkFrameworkException;

import zlink.framework.channel.v1.ChannelHeaderV1.ChannelHeader;
import zlink.framework.channel.v1.ChannelHeaderV1.MetadataEntry;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.Map;

final class ZLinkMetadataBoundaryTest {
    @Test
    void rejectsOverheadBeyondLimit() {
        check(Map.of("a", "x".repeat(505), "b", "y".repeat(505)), false);
    }

    @Test
    void acceptsExactly1024FieldBytes() {
        check(Map.of("k", "x".repeat(1015)), true);
    }

    @Test
    void rejects1025FieldBytes() {
        check(Map.of("k", "x".repeat(1016)), false);
    }

    private static void check(Map<String, String> metadata, boolean accepted) {
        var header =
                new ZLinkChannelEnvelope.Header(
                        3, "c", "m", "x", null, null, null, null, null, null, metadata, null, null);
        var proto =
                ChannelHeader.newBuilder()
                        .setKindValue(3)
                        .setChannelName("c")
                        .setMessageName("m")
                        .setContentType("x");
        var frame = new ByteArrayOutputStream();
        frame.write(1);
        frame.write(metadata.size());
        metadata.forEach(
                (key, value) -> {
                    proto.addMetadata(MetadataEntry.newBuilder().setKey(key).setValue(value));
                    byte[] k = key.getBytes(StandardCharsets.UTF_8);
                    byte[] v = value.getBytes(StandardCharsets.UTF_8);
                    frame.write(k.length);
                    frame.writeBytes(k);
                    frame.write(v.length >>> 8);
                    frame.write(v.length);
                    frame.writeBytes(v);
                });
        if (accepted) {
            ZLinkChannelEnvelope.encodeHeaderBytes(header);
            assertEquals(
                    metadata,
                    ZLinkChannelEnvelope.decodeHeader(
                                    ByteBuffer.wrap(proto.build().toByteArray()), false)
                            .metadata());
            ZLinkApplicationMetadata.copyOf(metadata).encode();
            assertEquals(metadata, ZLinkApplicationMetadata.decode(frame.toByteArray()));
        } else {
            assertProtocolError(() -> ZLinkChannelEnvelope.encodeHeaderBytes(header));
            assertProtocolError(
                    () ->
                            ZLinkChannelEnvelope.decodeHeader(
                                    ByteBuffer.wrap(proto.build().toByteArray()), false));
            assertThrows(
                    IllegalArgumentException.class,
                    () -> ZLinkApplicationMetadata.copyOf(metadata).encode());
            assertThrows(
                    IllegalArgumentException.class,
                    () -> ZLinkApplicationMetadata.decode(frame.toByteArray()));
        }
    }

    private static void assertProtocolError(Executable call) {
        var error = assertThrows(ZLinkFrameworkException.class, call);
        assertEquals(ZLinkFrameworkErrorKind.PROTOCOL_ERROR, error.kind());
    }
}
