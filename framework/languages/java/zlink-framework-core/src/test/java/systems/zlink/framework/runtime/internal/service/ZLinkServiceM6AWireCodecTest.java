package systems.zlink.framework.runtime.internal.service;
import java.util.Arrays;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

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

    //  GOLDEN — service-wire-v1.schema.json reply(20) byte layout.
    //  `request-specific-tail` is a conditional-union WITHOUT `bodyLengthType`,
    //  so the tail is written inline: prefix(5) + u64 correlation +
    //  u32 terminalResult + u32 failureCode + tail. An empty tail therefore
    //  yields exactly 21 bytes with no u16 tail-length field. This vector is
    //  byte-identical across C++/Java/Node/.NET.
    @Test
    void goldenReplyHeaderPinsInlineSchemaTailByteLayout() {
        byte[] reply = codec.encodeReplyHeader(7, 0, 0);
        assertEquals(21, reply.length);
        assertArrayEquals(
            hex("5a4d01140000000000000000070000000000000000"),
            reply);
        assertArrayEquals(
            hex("5a4d0114000000000000000008000000660000000e"),
            codec.encodeReplyHeader(8, 102, 14));
    }

    //  GOLDEN - reply(20).tail is an inline `request-specific-tail`
    //  conditional-union with no bodyLengthType. decodeReplyHeader cannot
    //  know the original operation kind (it is external correlation
    //  context), so it MUST accept trailing tail bytes instead of rejecting
    //  them -- the operation-specific decoder validates the tail's
    //  structure. This mirrors Node's decodeReplyHeader (`frame.byteLength <
    //  21` bound, no exact-length check) and .NET's equivalent.
    @Test
    void decodeReplyHeaderAcceptsFramesWithAndWithoutInlineTail() {
        byte[] noTail = codec.encodeReplyHeader(43, 0, 0);
        ZLinkServiceM6AWireCodec.Reply decodedNoTail =
            codec.decodeReplyHeader(noTail);
        assertEquals(43, decodedNoTail.correlation());
        assertEquals(0, decodedNoTail.terminalResult());
        assertEquals(0, decodedNoTail.failureCode());

        byte[] withTail = Arrays.copyOf(noTail, noTail.length + 8);
        System.arraycopy(
            hex("0102030405060708"), 0, withTail, noTail.length, 8);
        ZLinkServiceM6AWireCodec.Reply decodedWithTail =
            codec.decodeReplyHeader(withTail);
        assertEquals(43, decodedWithTail.correlation());
        assertEquals(0, decodedWithTail.terminalResult());
        assertEquals(0, decodedWithTail.failureCode());

        assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeReplyHeader(truncated(noTail)));
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
        return descriptor(
            source, ZLinkServiceNodeDescriptor.State.SERVING);
    }

    private static ZLinkServiceNodeDescriptor descriptor(
        RoutingId source,
        ZLinkServiceNodeDescriptor.State state) {
        return new ZLinkServiceNodeDescriptor(
            "mesh",
            source,
            7,
            11,
            "tcp://127.0.0.1:3001",
            List.of(
                new ZLinkServiceNodeDescriptor.Channel("chat", 50),
                new ZLinkServiceNodeDescriptor.Channel("orders", 100)),
            state,
            "service-a",
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

    //  GOLDEN — service-wire-v1.schema.json `runtime-state` (u8) is
    //  preparing=0, serving=1, draining=2, stopped=3, error=4, with NO
    //  `retiring` value: it is a host-internal transition whose remote
    //  admission meaning is `draining`. C++ (runtime_state_wire) and Node
    //  (stateToWire) encode exactly these values. Deriving the wire value
    //  from the Java enum ordinal shifted every state by one, so a SERVING
    //  Java node advertised itself to every peer as `draining` and stayed
    //  out of the peer's ready-peer set forever. These vectors are the
    //  descriptor extension's TLV 1 payload byte.
    @Test
    void goldenRuntimeStatePinsTheSchemaWireValues() {
        record Case(ZLinkServiceNodeDescriptor.State state, int wire) {
        }
        for (Case expected : List.of(
            new Case(ZLinkServiceNodeDescriptor.State.PREPARING, 0),
            new Case(ZLinkServiceNodeDescriptor.State.SERVING, 1),
            new Case(ZLinkServiceNodeDescriptor.State.RETIRING, 2),
            new Case(ZLinkServiceNodeDescriptor.State.DRAINING, 2),
            new Case(ZLinkServiceNodeDescriptor.State.STOPPED, 3),
            new Case(ZLinkServiceNodeDescriptor.State.ERROR, 4))) {
            var source = RoutingId.from("node-a");
            byte[] frame = codec.encodeAdmission(
                ServiceWireConstants.COMMAND_HELLO,
                descriptor(source, expected.state()));
            //  TLV 1 is the first descriptor-extension field: u8 id, u32
            //  length, then the one-byte runtime state.
            int stateIndex = indexOfExtensionTlv(frame, 1);
            assertEquals(
                expected.wire(),
                Byte.toUnsignedInt(frame[stateIndex]),
                "runtime state wire value for " + expected.state());
        }

        //  `retiring` is not a wire value: it never decodes back.
        var source = RoutingId.from("node-a");
        assertEquals(
            ZLinkServiceNodeDescriptor.State.DRAINING,
            codec.decodeAdmission(
                codec.encodeAdmission(
                    ServiceWireConstants.COMMAND_HELLO,
                    descriptor(
                        source,
                        ZLinkServiceNodeDescriptor.State.RETIRING)),
                ServiceWireConstants.COMMAND_HELLO,
                source).state());
    }

    //  spec 13 §7.1: the lifecycle generation is an OPAQUE CSPRNG equality
    //  token -- "which lifecycle is newer isn't judged by numeric
    //  magnitude". C++/Node/.NET carry the full unsigned 64-bit range, so
    //  about half of all peer-generated tokens have bit 63 set. Rejecting
    //  those made admission against a peer fail at random, about half the
    //  time, for the lifetime of that peer process.
    @Test
    void admissionAcceptsAnOpaqueLifecycleGenerationWithBit63Set() {
        var source = RoutingId.from("node-a");
        for (long generation : new long[] {
            1L,
            Long.MAX_VALUE,
            Long.MIN_VALUE,          // 0x8000000000000000
            -1L,                     // 0xffffffffffffffff
            0x9e3779b97f4a7c15L}) {
            var descriptor = new ZLinkServiceNodeDescriptor(
                "mesh",
                source,
                generation,
                11,
                "tcp://127.0.0.1:3001",
                List.of(new ZLinkServiceNodeDescriptor.Channel("chat", 50)),
                ZLinkServiceNodeDescriptor.State.SERVING,
                ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY,
                3,
                List.of(ZLinkServiceNodeDescriptor.REQUIRED_CAPABILITY),
                ZLinkServiceNodeDescriptor.ObjectRole.SERVER,
                80,
                1000,
                100,
                4,
                2);
            assertEquals(
                descriptor,
                codec.decodeAdmission(
                    codec.encodeAdmission(
                        ServiceWireConstants.COMMAND_HELLO, descriptor),
                    ServiceWireConstants.COMMAND_HELLO,
                    source),
                "lifecycle generation " + Long.toUnsignedString(generation));
        }
    }

    //  The descriptor's securityIdentity is the shared plaintext-transport
    //  placeholder every language encodes ("default"), never a routing id.
    //  A node that advertised its routing id here was rejected by every peer
    //  that fences an expected security identity.
    @Test
    void plaintextSecurityIdentityIsTheSharedCrossLanguagePlaceholder() {
        assertEquals(
            "default",
            ZLinkServiceNodeDescriptor.PLAINTEXT_SECURITY_IDENTITY);
    }

    /** Offset of the value byte of the first descriptor-extension TLV `id`. */
    private static int indexOfExtensionTlv(byte[] frame, int id) {
        //  prefix(5) + topologyKind(1) + routeLength(4) then the route body;
        //  the extension follows its own u32 length. Scanning for the TLV
        //  header from the end of the frame is enough for this fixture: TLV
        //  1 is the first extension field and the extension is the frame
        //  tail.
        for (int index = 10; index + 5 < frame.length; index++) {
            if (Byte.toUnsignedInt(frame[index]) == id
                && frame[index + 1] == 0
                && frame[index + 2] == 0
                && frame[index + 3] == 0
                && frame[index + 4] == 1) {
                return index + 5;
            }
        }
        throw new AssertionError("descriptor extension TLV is missing: " + id);
    }

    @Test
    void terminalFailurePairsFollowTheSchemaIntegrityTable() {
        //  Schema terminal-failure-integrity (spec 51:43-47,
        //  service-wire-v1.schema.json): exact typed pairs and boundary
        //  terminals with none are valid; mismatched or unknown pairs are not.
        assertTrue(ServiceWireConstants.validTerminalFailure(102, 9));
        assertTrue(ServiceWireConstants.validTerminalFailure(105, 17));
        assertTrue(ServiceWireConstants.validTerminalFailure(106, 18));
        assertTrue(ServiceWireConstants.validTerminalFailure(104, 16));
        assertTrue(ServiceWireConstants.validTerminalFailure(108, 0));
        assertFalse(ServiceWireConstants.validTerminalFailure(104, 3));
        assertFalse(ServiceWireConstants.validTerminalFailure(102, 18));
        assertFalse(ServiceWireConstants.validTerminalFailure(108, 5));
        assertFalse(ServiceWireConstants.validTerminalFailure(102, 23));
        assertFalse(ServiceWireConstants.validTerminalFailure(102, 99));
        //  The codec enforces the same rule on encode and decode.
        assertThrows(RuntimeException.class,
            () -> codec.encodeReplyHeader(1, 104, 3));
        assertThrows(RuntimeException.class,
            () -> codec.encodeReplyHeader(1, 102, 18));
    }

    @Test
    void malformedReplyHeaderThrowsIllegalArgumentForProtocolError() {
        //  The Instance Spot request completion converts any
        //  IllegalArgumentException (which every codec malformed-wire
        //  ZLinkServiceWireException derives from) into
        //  ZLinkFrameworkException(PROTOCOL_ERROR): a reply that can't be
        //  processed is a ProtocolError (spec 32-framework-error-model:91-92).
        //  This pins the inheritance that conversion relies on.
        assertTrue(IllegalArgumentException.class.isAssignableFrom(
            ZLinkServiceWireException.class));
        byte[] reply = codec.encodeReplyHeader(43, 0, 0);
        ZLinkServiceWireException failure = assertThrows(
            ZLinkServiceWireException.class,
            () -> codec.decodeReplyHeader(truncated(reply)));
        assertTrue(failure instanceof IllegalArgumentException);
    }

    private static byte[] truncated(byte[] value) {
        return Arrays.copyOf(value, value.length - 1);
    }

    private static byte[] withTrailingByte(byte[] value) {
        return Arrays.copyOf(value, value.length + 1);
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
