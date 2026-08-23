package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.List;
import org.junit.jupiter.api.Test;
import systems.zlink.framework.runtime.protocol.ServiceWirePilotCodec;

final class GeneratedServiceWireCommandCodecConformanceTest {
    private static final ObjectMapper JSON = new ObjectMapper();

    @Test
    void batch3RuntimeAndGeneratedCodecsMatchCanonicalGoldens()
        throws Exception {
        for (byte[] bytes : canonicalArray("reply-relay-v1.json")) {
            assertArrayEquals(bytes, runtimeCommandRoundTrip(bytes));
            assertArrayEquals(bytes, generatedCommandRoundTrip(bytes));
        }
        for (byte[] bytes : canonicalArray("relocation-control-v1.json")) {
            assertArrayEquals(bytes, runtimeCommandRoundTrip(bytes));
            assertArrayEquals(bytes, generatedCommandRoundTrip(bytes));
        }
        for (byte[] bytes : canonicalArray(
            "session-relocation-barrier-v1.json")) {
            assertArrayEquals(bytes, runtimeCommandRoundTrip(bytes));
            assertArrayEquals(bytes, generatedCommandRoundTrip(bytes));
        }
    }

    @Test
    void batch3RuntimeAndGeneratedCodecsRejectTheSameMalformedBytes()
        throws Exception {
        List<byte[]> malformed = new ArrayList<>(
            array("relocation-control-v1.json", "malformed"));
        for (byte[] bytes : canonicalArray("reply-relay-v1.json")) {
            malformed.addAll(mutations(bytes));
        }
        for (byte[] bytes : canonicalArray(
            "session-relocation-barrier-v1.json")) {
            malformed.addAll(mutations(bytes));
        }
        for (byte[] bytes : malformed) {
            assertTrue(runtimeCommandRejects(bytes));
            assertThrows(Exception.class,
                () -> generatedCommandRoundTrip(bytes));
        }
    }

    @Test
    void batch4RuntimeAndGeneratedCodecsMatchCanonicalGoldens()
        throws Exception {
        for (String file : List.of(
            "user-spot-create-v1.json",
            "user-spot-close-v1.json",
            "actor-create-v1.json")) {
            byte[] bytes = canonicalObject(file);
            assertArrayEquals(bytes, runtimeCommandRoundTrip(bytes));
            assertArrayEquals(bytes, generatedCommandRoundTrip(bytes));
        }
    }

    @Test
    void batch4RuntimeAndGeneratedCodecsRejectTheSameMalformedGoldens()
        throws Exception {
        for (String file : List.of(
            "user-spot-create-v1.json",
            "user-spot-close-v1.json",
            "actor-create-v1.json")) {
            for (byte[] bytes : array(file, "malformed")) {
                assertTrue(runtimeCommandRejects(bytes));
                assertThrows(Exception.class,
                    () -> generatedCommandRoundTrip(bytes));
            }
        }
    }

    @Test
    void zljrRuntimeAndGeneratedCodecsMatchCanonicalGolden()
        throws Exception {
        byte[] bytes = canonicalObject("zljr-v1.json");
        var runtime = ZLinkActorJoinRecoveryCodec.decodeSavedWork(bytes)
            .orElseThrow();
        var generated = ServiceWirePilotCodec.decodeZljrRecordV1(bytes);

        assertArrayEquals(bytes,
            ZLinkActorJoinRecoveryCodec.encodeSavedWork(runtime));
        assertArrayEquals(bytes,
            ServiceWirePilotCodec.encodeZljrRecordV1(generated));
    }

    @Test
    void zljrRuntimeAndGeneratedCodecsRejectTheSameMalformedGoldens()
        throws Exception {
        for (byte[] bytes : array("zljr-v1.json", "malformed")) {
            assertTrue(runtimeZljrRejects(bytes));
            assertThrows(Exception.class,
                () -> ServiceWirePilotCodec.decodeZljrRecordV1(bytes));
        }
    }

    private static byte[] runtimeCommandRoundTrip(byte[] bytes) {
        var codec = new ZLinkServiceM6BWireCodec();
        return switch (Byte.toUnsignedInt(bytes[3])) {
            case 30, 31, 34, 40, 52, 53 -> {
                var control = new ZLinkCanonicalRelocationControlCodec();
                yield control.encode(control.decode(bytes));
            }
            case 33 -> {
                var relocation = new ZLinkServiceRelocationWireCodec();
                yield relocation.encodeReplyRelay(
                    relocation.decodeReplyRelay(bytes));
            }
            case 42 -> codec.encodeSessionRelocationSeal(
                codec.decodeSessionRelocationSeal(bytes));
            case 43 -> codec.encodeSessionRelocationSealed(
                codec.decodeSessionRelocationSealed(bytes));
            case 44 -> codec.encodeSessionRelocationRoute(
                codec.decodeSessionRelocationRoute(bytes));
            case 46 -> {
                var relocation = new ZLinkServiceRelocationWireCodec();
                yield relocation.encodeReplyRelayAck(
                    relocation.decodeReplyRelayAck(bytes));
            }
            case 47 -> codec.encodeUserSpotCreateHeader(
                codec.decodeUserSpotCreateHeader(bytes));
            case 48 -> codec.encodeUserSpotCloseHeader(
                codec.decodeUserSpotCloseHeader(bytes));
            case 49 -> codec.encodeActorCreateHeader(
                codec.decodeActorCreateHeader(bytes));
            default -> throw new IllegalArgumentException("unknown command");
        };
    }

    private static byte[] generatedCommandRoundTrip(byte[] bytes)
        throws Exception {
        return switch (Byte.toUnsignedInt(bytes[3])) {
            case 30 -> ServiceWirePilotCodec.encodeRelocationReady30(
                ServiceWirePilotCodec.decodeRelocationReady30(bytes));
            case 31 -> ServiceWirePilotCodec.encodeRelocationData31(
                ServiceWirePilotCodec.decodeRelocationData31(bytes));
            case 33 -> ServiceWirePilotCodec.encodeReplyRelay33(
                ServiceWirePilotCodec.decodeReplyRelay33(List.of(bytes))).get(0);
            case 34 -> ServiceWirePilotCodec.encodeRelocationCutover34(
                ServiceWirePilotCodec.decodeRelocationCutover34(bytes));
            case 40 -> ServiceWirePilotCodec.encodeRelocationPrepare40(
                ServiceWirePilotCodec.decodeRelocationPrepare40(bytes));
            case 42 -> ServiceWirePilotCodec.encodeSessionRelocationSeal42(
                ServiceWirePilotCodec.decodeSessionRelocationSeal42(bytes));
            case 43 -> ServiceWirePilotCodec.encodeSessionRelocationSealed43(
                ServiceWirePilotCodec.decodeSessionRelocationSealed43(bytes));
            case 44 -> ServiceWirePilotCodec.encodeSessionRelocationRoute44(
                ServiceWirePilotCodec.decodeSessionRelocationRoute44(bytes));
            case 46 -> ServiceWirePilotCodec.encodeReplyRelayAck46(
                ServiceWirePilotCodec.decodeReplyRelayAck46(bytes));
            case 47 -> ServiceWirePilotCodec.encodeUserSpotCreate47(
                ServiceWirePilotCodec.decodeUserSpotCreate47(bytes));
            case 48 -> ServiceWirePilotCodec.encodeUserSpotClose48(
                ServiceWirePilotCodec.decodeUserSpotClose48(bytes));
            case 49 -> ServiceWirePilotCodec.encodeActorCreate49(
                ServiceWirePilotCodec.decodeActorCreate49(bytes));
            case 52 -> ServiceWirePilotCodec.encodeRelocationState52(
                ServiceWirePilotCodec.decodeRelocationState52(bytes));
            case 53 -> ServiceWirePilotCodec.encodeRelocationFailed53(
                ServiceWirePilotCodec.decodeRelocationFailed53(bytes));
            default -> throw new IllegalArgumentException("unknown command");
        };
    }

    private static boolean runtimeCommandRejects(byte[] bytes) {
        try {
            runtimeCommandRoundTrip(bytes);
            return false;
        } catch (Exception expected) {
            return true;
        }
    }

    private static boolean runtimeZljrRejects(byte[] bytes) {
        try {
            return ZLinkActorJoinRecoveryCodec.decodeSavedWork(bytes).isEmpty();
        } catch (Exception expected) {
            return true;
        }
    }

    private static List<byte[]> mutations(byte[] bytes) {
        return List.of(
            Arrays.copyOf(bytes, bytes.length - 1),
            Arrays.copyOf(bytes, bytes.length + 1));
    }

    private static byte[] canonicalObject(String file) throws Exception {
        return hex(fixture(file).path("canonical"));
    }

    private static List<byte[]> canonicalArray(String file) throws Exception {
        return array(file, "canonical");
    }

    private static List<byte[]> array(String file, String section)
        throws Exception {
        List<byte[]> result = new ArrayList<>();
        for (JsonNode entry : fixture(file).path(section)) {
            result.add(hex(entry));
        }
        return result;
    }

    private static byte[] hex(JsonNode entry) {
        return HexFormat.of().parseHex(entry.path("hex").asText());
    }

    private static JsonNode fixture(String file) throws Exception {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve("runtime/protocol/golden/" + file);
            if (Files.isRegularFile(candidate)) {
                return JSON.readTree(Files.readString(candidate));
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared fixture was not found: " + file);
    }
}
