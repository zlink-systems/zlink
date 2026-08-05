package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HexFormat;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.contracts.messaging.Message;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

final class ZLinkServiceM6AWireCodecTest {
    private final ZLinkServiceM6AWireCodec codec =
        new ZLinkServiceM6AWireCodec();

    @Test
    void admissionRoundTripsAllM6ACommandsAndDescriptorFields() {
        var source = RoutingId.from("node-a");
        var descriptor = descriptor(source);

        for (int command : List.of(
            ServiceWireConstants.COMMAND_HELLO,
            ServiceWireConstants.COMMAND_ADMIT,
            ServiceWireConstants.COMMAND_UPDATE)) {
            assertEquals(
                descriptor,
                codec.decodeAdmission(
                    codec.encodeAdmission(command, descriptor),
                    command,
                    source));
        }
        assertEquals(12, codec.decodeReject(codec.encodeReject(12)));
    }

    @Test
    void nodeChannelReplyAndApplicationPayloadRoundTrip() {
        assertEquals(
            ServiceWireConstants.FLAG_METADATA,
            codec.decodeHeader(codec.encodeNodeSendHeader(
                ServiceWireConstants.FLAG_METADATA)).flags());
        assertEquals(
            41,
            codec.decodeNodeRequestHeader(
                codec.encodeNodeRequestHeader(41, 0)));
        assertEquals(
            "orders",
            codec.decodeChannelSendHeader(
                codec.encodeChannelSendHeader("orders", 0)));
        assertEquals(
            new ZLinkServiceM6AWireCodec.ChannelRequest(42, "orders"),
            codec.decodeChannelRequestHeader(
                codec.encodeChannelRequestHeader(
                    42,
                    "orders",
                    ServiceWireConstants.FLAG_METADATA)));
        assertEquals(
            new ZLinkServiceM6AWireCodec.Reply(43, 0, 0),
            codec.decodeReplyHeader(codec.encodeReplyHeader(43, 0, 0)));
        assertEquals(
            new ZLinkServiceM6AWireCodec.Reply(44, 102, 9),
            codec.decodeReplyHeader(codec.encodeReplyHeader(44, 102, 9)));

        var payload = new ZLinkServiceM6AWireCodec.ApplicationPayload(
            "OrderPlaced",
            "application/zlink-framework-json-v1",
            new byte[] {1, 2, 3});
        var decoded = codec.decodeApplicationPayload(
            codec.encodeApplicationPayload(payload));
        assertEquals(payload.packetName(), decoded.packetName());
        assertEquals(payload.contentType(), decoded.contentType());
        assertArrayEquals(payload.payload(), decoded.payload());
    }

    @Test
    void frameworkMultipartMatchesCanonicalProfileFixture() {
        try (Message first = Message.from(new byte[] {1, 2});
             Message second = Message.from(new byte[] {(byte) 0xaa, (byte) 0xbb,
                 (byte) 0xcc})) {
            var payload = codec.encodeFrameworkMultipart(List.of(first, second));
            assertEquals(
                ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME,
                payload.packetName());
            assertEquals(
                ServiceWireConstants.FRAMEWORK_MULTIPART_CONTENT_TYPE,
                payload.contentType());
            assertArrayEquals(
                hex("0000000200000002010200000003aabbcc"),
                payload.payload());

            List<Message> decoded = codec.decodeFrameworkMultipart(
                codec.decodeApplicationPayload(
                    codec.encodeApplicationPayload(payload)));
            try {
                assertEquals(2, decoded.size());
                assertArrayEquals(
                    new byte[] {1, 2}, decoded.get(0).toByteArray());
                assertArrayEquals(
                    new byte[] {(byte) 0xaa, (byte) 0xbb, (byte) 0xcc},
                    decoded.get(1).toByteArray());
            } finally {
                decoded.forEach(Message::close);
            }
        }
    }

    @Test
    void frameworkMultipartConsumesSharedGoldenFixture() throws Exception {
        JsonNode fixture = new ObjectMapper().readTree(
            Files.readString(sharedMultipartFixturePath()));
        JsonNode canonical = fixture.path("valid").get(0);
        var payload = codec.decodeApplicationPayload(
            HexFormat.of().parseHex(canonical.path("encodedHex").asText()));
        List<Message> decoded = codec.decodeFrameworkMultipart(payload);
        try {
            JsonNode expectedParts = canonical.path("partsHex");
            assertEquals(expectedParts.size(), decoded.size());
            for (int index = 0; index < decoded.size(); index++) {
                assertArrayEquals(
                    HexFormat.of().parseHex(
                        expectedParts.get(index).asText()),
                    decoded.get(index).toByteArray());
            }
        } finally {
            decoded.forEach(Message::close);
        }

        for (JsonNode invalid : fixture.path("invalid")) {
            byte[] encoded = HexFormat.of().parseHex(
                invalid.path("encodedHex").asText());
            assertThrows(
                ZLinkServiceWireException.class,
                () -> codec.decodeFrameworkMultipart(
                    codec.decodeApplicationPayload(encoded)));
        }
    }

    @Test
    void frameworkMultipartRejectsMalformedProfilesBeforeUse() {
        var profile = ServiceWireConstants.FRAMEWORK_MULTIPART_PACKET_NAME;
        var contentType = ServiceWireConstants.FRAMEWORK_MULTIPART_CONTENT_TYPE;
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeFrameworkMultipart(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    profile, contentType, new byte[] {0, 0, 0, 0})));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeFrameworkMultipart(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    profile,
                    contentType,
                    hex("000000010000000301"))));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeFrameworkMultipart(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    profile,
                    contentType,
                    hex("00000001000000000100"))));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeFrameworkMultipart(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    "event", contentType, new byte[] {1})));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeFrameworkMultipart(
                new ZLinkServiceM6AWireCodec.ApplicationPayload(
                    profile, "application/octet-stream", new byte[] {1})));
    }

    @Test
    void malformedAndNonCanonicalRecordsAreRejected() {
        byte[] admission = codec.encodeAdmission(
            ServiceWireConstants.COMMAND_HELLO,
            descriptor(RoutingId.from("node-a")));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeAdmission(
                truncated(admission),
                ServiceWireConstants.COMMAND_HELLO,
                RoutingId.from("node-a")));

        byte[] badMagic = codec.encodeNodeSendHeader(0);
        badMagic[0] = 0;
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeHeader(badMagic));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.encodeNodeRequestHeader(0, 0));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.encodeChannelRequestHeader(0, "orders", 0));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeChannelRequestHeader(
                withTrailingByte(
                    codec.encodeChannelRequestHeader(1, "orders", 0))));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.encodeReplyHeader(1, 0, 9));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.encodeReplyHeader(1, 101, 9));
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeApplicationPayload(
                withTrailingByte(codec.encodeApplicationPayload(
                    new ZLinkServiceM6AWireCodec.ApplicationPayload(
                        "event",
                        "application/octet-stream",
                        new byte[] {1})))));
        byte[] invalidRejectReason = codec.encodeReject(1);
        invalidRejectReason[invalidRejectReason.length - 1] = 0;
        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeReject(invalidRejectReason));
    }

    private static ZLinkServiceNodeDescriptor descriptor(RoutingId source) {
        return new ZLinkServiceNodeDescriptor(
            "mesh",
            source,
            7,
            11,
            "tcp://127.0.0.1:3001",
            List.of(
                new ZLinkServiceNodeDescriptor.Channel("chat", 50),
                new ZLinkServiceNodeDescriptor.Channel("orders", 100)),
            ZLinkServiceNodeDescriptor.State.SERVING,
            "service-a",
            4 * 1024 * 1024,
            3,
            List.of(
                ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY,
                "typed-json-v1"),
            ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
            80,
            1000,
            100,
            4,
            2);
    }

    private static byte[] truncated(byte[] value) {
        return java.util.Arrays.copyOf(value, value.length - 1);
    }

    private static byte[] withTrailingByte(byte[] value) {
        return java.util.Arrays.copyOf(value, value.length + 1);
    }

    private static byte[] hex(String value) {
        byte[] result = new byte[value.length() / 2];
        for (int index = 0; index < result.length; index++) {
            result[index] = (byte) Integer.parseInt(
                value.substring(index * 2, index * 2 + 2), 16);
        }
        return result;
    }

    private static Path sharedMultipartFixturePath() {
        Path current = Path.of(System.getProperty("user.dir"))
            .toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/protocol/golden/framework-multipart-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared framework multipart fixture was not found");
    }
}
