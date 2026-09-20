package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
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
    private static final ServiceWireCodec.DecoderContext CONTEXT =
        new ServiceWireCodec.DecoderContext(
            null, null, null, 0xffff_ffffL, 4_294_966_774L);

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

    @Test
    void finalGeneratedCodecConformsToEveryIndexedFixture()
        throws Exception {
        JsonNode fixtures = fixtureIndex().path("fixtures");
        assertEquals(9, fixtures.size());
        int canonicalCount = 0;
        int malformedCount = 0;
        for (JsonNode indexed : fixtures) {
            String file = indexed.path("goldenFixture").asText()
                .substring("golden/".length());
            JsonNode golden = fixture(file);
            String kind = indexed.path("kind").asText();
            for (JsonNode canonical : indexed.path("canonical")) {
                canonicalCount++;
                JsonNode pointers = canonical.path("pointers");
                switch (kind) {
                    case "durable" -> {
                        byte[] bytes = pointedHex(golden, pointers, "encodedHex");
                        String format = indexed.path("surface").path("format").asText();
                        assertArrayEquals(bytes, ServiceWireCodec.encodeDurable(format,
                            ServiceWireCodec.decodeDurable(format, bytes, CONTEXT), CONTEXT), file);
                        byte[] malformed = Arrays.copyOf(bytes, bytes.length);
                        malformed[malformed.length - 1] ^= 1;
                        assertThrows(Exception.class,
                            () -> ServiceWireCodec.decodeDurable(format, malformed, CONTEXT), file);
                    }
                    case "logical" -> {
                        byte[] bytes = pointedHex(golden, pointers, "logicalHex");
                        assertArrayEquals(bytes, ServiceWireCodec.encodeLogicalRelocationEnvelopeV1(
                            ServiceWireCodec.decodeLogicalRelocationEnvelopeV1(bytes, CONTEXT), CONTEXT),
                            file);
                        byte[] malformed = Arrays.copyOf(bytes, bytes.length + 1);
                        assertThrows(Exception.class, () -> ServiceWireCodec
                            .decodeLogicalRelocationEnvelopeV1(malformed, CONTEXT), file);
                    }
                    case "command" -> {
                        List<byte[]> frames = pointedFrames(golden, pointers);
                        assertFramesEqual(frames, ServiceWireCodec.encodeCommandFrames(
                            ServiceWireCodec.decodeCommand(frames, CONTEXT), CONTEXT));
                    }
                    default -> throw new IllegalStateException("unknown fixture kind: " + kind);
                }
            }
            if (kind.equals("command")) {
                for (JsonNode malformed : indexed.path("malformed")) {
                    malformedCount++;
                    List<byte[]> frames = pointedFrames(golden, malformed.path("pointers"));
                    assertThrows(Exception.class,
                        () -> ServiceWireCodec.decodeCommand(frames, CONTEXT),
                        file + ":" + malformed.path("name").asText());
                }
            }
        }
        assertEquals(11, canonicalCount);
        assertEquals(12, malformedCount);
        JsonNode operationCases = fixtureIndex().path("operationCases");
        assertEquals(16, operationCases.size());
        for (JsonNode operationCase : operationCases) {
            String operation = operationCase.path("operation").asText();
            String message = operation + ":" + operationCase.path("name").asText();
            if (operationCase.path("expect").asText().equals("accept")) {
                assertOperationCaseAccepted(operationCase, message);
            } else {
                assertThrows(Exception.class,
                    () -> decodeOperationCase(operationCase), message);
            }
        }
    }

    private static void assertOperationCaseAccepted(JsonNode operationCase,
        String message) throws Exception {
        try {
            decodeOperationCase(operationCase);
        } catch (Exception failure) {
            throw new AssertionError(message, failure);
        }
    }

    private static void decodeOperationCase(JsonNode operationCase)
        throws Exception {
        JsonNode surface = operationCase.path("surface");
        ServiceWireCodec.DecoderContext context = context(operationCase);
        switch (surface.path("format").asText()) {
            case "type" -> {
                byte[] bytes = HexFormat.of().parseHex(operationCase.path("hex").asText());
                switch (surface.path("type").asText()) {
                    case "descriptor-extension" ->
                        ServiceWireCodec.decodeDescriptorExtension(bytes, context);
                    case "text8" -> ServiceWireCodec.decodeText8(bytes, context);
                    case "metadata-frame" ->
                        ServiceWireCodec.decodeMetadataFrame(bytes, context);
                    case "application-payload-bytes" ->
                        ServiceWireCodec.decodeApplicationPayloadBytes(bytes, context);
                    case "application-payload-envelope-v1" ->
                        ServiceWireCodec.decodeApplicationPayloadEnvelopeV1(bytes, context);
                    default -> throw new IllegalStateException("unknown type operation case");
                }
            }
            case "command" -> ServiceWireCodec.decodeCommand(
                hexFrames(operationCase.path("framesHex")), context);
            case "semantic" -> ServiceWireCodec.validateReplyPredicate(
                ServiceWireCodec.RequestTerminalResult.valueOf(
                    enumName(operationCase.path("input").path("terminalResult").asText())),
                ServiceWireCodec.FrameworkErrorCode.valueOf(
                    enumName(operationCase.path("input").path("failureCode").asText())));
            case "relocation-envelope-v1" -> ServiceWireCodec
                .decodeLogicalRelocationEnvelopeV1(
                    HexFormat.of().parseHex(operationCase.path("hex").asText()), CONTEXT);
            case "authority-payload-v1" -> ServiceWireCodec.decodeDurable(
                "authority-payload-v1",
                HexFormat.of().parseHex(operationCase.path("hex").asText()), context);
            default -> throw new IllegalStateException("unknown operation case surface");
        }
    }

    private static ServiceWireCodec.DecoderContext context(JsonNode operationCase) {
        JsonNode values = operationCase.path("decodeContext");
        long messageBytes = values.has("effectiveCompleteMessageBytes")
            ? values.path("effectiveCompleteMessageBytes").asLong()
            : 0xffff_ffffL;
        long payloadBytes = values.has(
            "effectiveCompleteMessageBytesMinusActualEnvelopeOverhead")
            ? values.path(
                "effectiveCompleteMessageBytesMinusActualEnvelopeOverhead").asLong()
            : 4_294_966_774L;
        return new ServiceWireCodec.DecoderContext(
            null, null, null, messageBytes, payloadBytes);
    }

    private static List<byte[]> hexFrames(JsonNode frames) {
        List<byte[]> result = new ArrayList<>();
        for (JsonNode frame : frames) {
            result.add(HexFormat.of().parseHex(frame.asText()));
        }
        return result;
    }

    private static String enumName(String value) {
        return value.replaceAll("([a-z0-9])([A-Z])", "$1_$2").toUpperCase();
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

    private static JsonNode fixtureIndex() throws Exception {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve("runtime/protocol/generated/fixtures/index.json");
            if (Files.isRegularFile(candidate)) {
                return JSON.readTree(Files.readString(candidate));
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared fixture index was not found");
    }

    private static byte[] pointedHex(JsonNode fixture, JsonNode pointers,
        String name) {
        return HexFormat.of().parseHex(fixture.at(pointers.path(name).asText()).asText());
    }

    private static List<byte[]> pointedFrames(JsonNode fixture, JsonNode pointers) {
        JsonNode framesPointer = pointers.path("framesHex");
        if (!framesPointer.isMissingNode()) {
            List<byte[]> frames = new ArrayList<>();
            for (JsonNode frame : fixture.at(framesPointer.asText())) {
                frames.add(HexFormat.of().parseHex(frame.asText()));
            }
            return frames;
        }
        return List.of(pointedHex(fixture, pointers, "hex"));
    }

    private static void assertFramesEqual(List<byte[]> expected, List<byte[]> actual) {
        assertTrue(expected.size() == actual.size());
        for (int index = 0; index < expected.size(); index++) {
            assertArrayEquals(expected.get(index), actual.get(index));
        }
    }
}
