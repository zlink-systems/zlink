package systems.zlink.framework.runtime.internal.service;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.io.ByteArrayOutputStream;
import java.nio.file.Files;
import java.nio.file.Path;
import java.security.MessageDigest;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.HashMap;
import java.util.HashSet;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;
import java.util.Set;
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
        assertEquals(79, operationCases.size());
        Map<String, Set<String>> boundaryPairs = new HashMap<>();
        for (JsonNode operationCase : operationCases) {
            String operation = operationCase.path("operation").asText();
            if (operationCase.has("boundaryPair")) {
                boundaryPairs.computeIfAbsent(
                    operationCase.path("boundaryPair").asText(), ignored -> new HashSet<>())
                    .add(operationCase.path("expect").asText());
            }
            for (String direction : directions(operationCase)) {
                String message = operation + ":"
                    + operationCase.path("name").asText() + ":" + direction;
                if (operationCase.path("expect").asText().equals("accept")) {
                    assertOperationCaseAccepted(
                        operationCase, direction, message);
                } else {
                    assertThrows(Exception.class,
                        () -> applyOperationCase(operationCase, direction), message);
                }
            }
        }
        assertEquals(25, boundaryPairs.size());
        for (Map.Entry<String, Set<String>> pair : boundaryPairs.entrySet()) {
            assertEquals(Set.of("accept", "reject"), pair.getValue(), pair.getKey());
        }
    }

    private static void assertOperationCaseAccepted(JsonNode operationCase,
        String direction, String message) throws Exception {
        try {
            applyOperationCase(operationCase, direction);
        } catch (Exception failure) {
            throw new AssertionError(message, failure);
        }
    }

    private static List<String> directions(JsonNode operationCase) {
        List<String> result = new ArrayList<>();
        for (JsonNode direction : operationCase.path("directions")) {
            result.add(direction.asText());
        }
        return result.isEmpty() ? List.of("decode") : result;
    }

    private static void applyOperationCase(JsonNode operationCase,
        String direction)
        throws Exception {
        if (!direction.equals("encode") && !direction.equals("decode")) {
            throw new IllegalStateException("unknown operation direction: " + direction);
        }
        JsonNode surface = operationCase.path("surface");
        ServiceWireCodec.DecoderContext context = context(operationCase);
        switch (surface.path("format").asText()) {
            case "type" -> {
                byte[] bytes = operationBytes(operationCase);
                String type = surface.path("type").asText();
                if (direction.equals("decode")) {
                    decodeType(type, bytes, context);
                } else {
                    Object value = operationCase.has("input")
                        ? inputValue(type, operationCase.path("input"))
                        : decodeType(type, bytes, CONTEXT);
                    assertArrayEquals(bytes, encodeType(type, value, context));
                }
            }
            case "command" -> {
                List<byte[]> frames = hexFrames(operationCase.path("framesHex"));
                if (direction.equals("decode")) {
                    ServiceWireCodec.decodeCommand(frames, context);
                } else {
                    assertFramesEqual(frames, ServiceWireCodec.encodeCommandFrames(
                        ServiceWireCodec.decodeCommand(frames, CONTEXT), context));
                }
            }
            case "semantic" -> ServiceWireCodec.validateReplyPredicate(
                ServiceWireCodec.RequestTerminalResult.valueOf(
                    enumName(operationCase.path("input").path("terminalResult").asText())),
                ServiceWireCodec.FrameworkErrorCode.valueOf(
                    enumName(operationCase.path("input").path("failureCode").asText())));
            case "relocation-envelope-v1" -> {
                byte[] bytes = operationBytes(operationCase);
                if (direction.equals("decode")) {
                    ServiceWireCodec.decodeLogicalRelocationEnvelopeV1(bytes, context);
                } else {
                    assertArrayEquals(bytes, ServiceWireCodec
                        .encodeLogicalRelocationEnvelopeV1(ServiceWireCodec
                            .decodeLogicalRelocationEnvelopeV1(bytes, CONTEXT), context));
                }
            }
            case "authority-payload-v1" -> {
                byte[] bytes = operationBytes(operationCase);
                if (direction.equals("decode")) {
                    ServiceWireCodec.decodeDurable(
                        "authority-payload-v1", bytes, context);
                } else {
                    assertArrayEquals(bytes, ServiceWireCodec.encodeDurable(
                        "authority-payload-v1", ServiceWireCodec.decodeDurable(
                            "authority-payload-v1", bytes, CONTEXT), context));
                }
            }
            default -> throw new IllegalStateException("unknown operation case surface");
        }
    }

    private static ServiceWireCodec.DecoderContext context(JsonNode operationCase) {
        JsonNode values = operationCase.path("context");
        boolean missing = operationCase.path("operation").asText()
            .equals("negotiated-bound") && values.isMissingNode();
        Long messageBytes = values.has("effectiveCompleteMessageBytes")
            ? values.path("effectiveCompleteMessageBytes").asLong()
            : missing ? null : 0xffff_ffffL;
        Long payloadBytes = values.has(
            "effectiveCompleteMessageBytesMinusActualEnvelopeOverhead")
            ? values.path(
                "effectiveCompleteMessageBytesMinusActualEnvelopeOverhead").asLong()
            : missing ? null : 4_294_966_774L;
        return new ServiceWireCodec.DecoderContext(
            null, null, null, messageBytes, payloadBytes);
    }

    private static byte[] operationBytes(JsonNode operationCase) throws Exception {
        if (operationCase.has("hex")) {
            return HexFormat.of().parseHex(operationCase.path("hex").asText());
        }
        ByteArrayOutputStream bytes = new ByteArrayOutputStream();
        if (operationCase.has("chunksHex")) {
            for (JsonNode chunk : operationCase.path("chunksHex")) {
                bytes.writeBytes(HexFormat.of().parseHex(chunk.asText()));
            }
            return bytes.toByteArray();
        }
        JsonNode recipe = operationCase.path("byteRecipe");
        for (JsonNode segment : recipe.path("segments")) {
            if (segment.has("hex")) {
                bytes.writeBytes(HexFormat.of().parseHex(segment.path("hex").asText()));
            } else {
                byte[] repeated = new byte[segment.path("count").asInt()];
                Arrays.fill(repeated, (byte) segment.path("repeatByte").asInt());
                bytes.writeBytes(repeated);
            }
        }
        byte[] result = bytes.toByteArray();
        assertEquals(recipe.path("encodedBytes").asInt(), result.length);
        assertEquals(recipe.path("sha256").asText(), HexFormat.of().formatHex(
            MessageDigest.getInstance("SHA-256").digest(result)));
        return result;
    }

    private static Object decodeType(String type, byte[] bytes,
        ServiceWireCodec.DecoderContext context) throws Exception {
        return switch (type) {
            case "application-version" ->
                ServiceWireCodec.decodeApplicationVersion(bytes, context);
            case "bool8" -> ServiceWireCodec.decodeBool8(bytes, context);
            case "rid" -> ServiceWireCodec.decodeRid(bytes, context);
            case "text8" -> ServiceWireCodec.decodeText8(bytes, context);
            case "optional-actor-ref" ->
                ServiceWireCodec.decodeOptionalActorRef(bytes, context);
            case "actor-ref" -> ServiceWireCodec.decodeActorRef(bytes, context);
            case "sorted-text8-vector" ->
                ServiceWireCodec.decodeSortedText8Vector(bytes, context);
            case "metadata-frame" ->
                ServiceWireCodec.decodeMetadataFrame(bytes, context);
            case "application-payload-envelope-v1" ->
                ServiceWireCodec.decodeApplicationPayloadEnvelopeV1(bytes, context);
            case "relocation-object-identity" ->
                ServiceWireCodec.decodeRelocationObjectIdentity(bytes, context);
            case "descriptor-extension" ->
                ServiceWireCodec.decodeDescriptorExtension(bytes, context);
            case "aggregate-participant-vector" ->
                ServiceWireCodec.decodeAggregateParticipantVector(bytes, context);
            case "application-payload-bytes" ->
                ServiceWireCodec.decodeApplicationPayloadBytes(bytes, context);
            case "creation-operation-terminal-v1" ->
                ServiceWireCodec.decodeCreationOperationTerminalV1(bytes, context);
            default -> throw new IllegalStateException("unknown fixture type: " + type);
        };
    }

    private static byte[] encodeType(String type, Object value,
        ServiceWireCodec.DecoderContext context) throws Exception {
        return switch (type) {
            case "application-version" -> ServiceWireCodec.encodeApplicationVersion(
                (ServiceWireCodec.ApplicationVersion) value, context);
            case "bool8" -> ServiceWireCodec.encodeBool8(
                (ServiceWireCodec.Bool8) value, context);
            case "rid" -> ServiceWireCodec.encodeRid(
                (ServiceWireCodec.Rid) value, context);
            case "text8" -> ServiceWireCodec.encodeText8(
                (ServiceWireCodec.Text8) value, context);
            case "optional-actor-ref" -> ServiceWireCodec.encodeOptionalActorRef(
                (ServiceWireCodec.OptionalActorRef) value, context);
            case "actor-ref" -> ServiceWireCodec.encodeActorRef(
                (ServiceWireCodec.ActorRef) value, context);
            case "sorted-text8-vector" -> ServiceWireCodec.encodeSortedText8Vector(
                (ServiceWireCodec.SortedText8Vector) value, context);
            case "metadata-frame" -> ServiceWireCodec.encodeMetadataFrame(
                (ServiceWireCodec.MetadataFrame) value, context);
            case "application-payload-envelope-v1" -> ServiceWireCodec
                .encodeApplicationPayloadEnvelopeV1(
                    (ServiceWireCodec.ApplicationPayloadEnvelopeV1) value, context);
            case "relocation-object-identity" -> ServiceWireCodec
                .encodeRelocationObjectIdentity(
                    (ServiceWireCodec.RelocationObjectIdentity) value, context);
            case "descriptor-extension" -> ServiceWireCodec.encodeDescriptorExtension(
                (ServiceWireCodec.DescriptorExtension) value, context);
            case "aggregate-participant-vector" -> ServiceWireCodec
                .encodeAggregateParticipantVector(
                    (ServiceWireCodec.AggregateParticipantVector) value, context);
            case "application-payload-bytes" -> ServiceWireCodec
                .encodeApplicationPayloadBytes(
                    (ServiceWireCodec.ApplicationPayloadBytes) value, context);
            case "creation-operation-terminal-v1" -> ServiceWireCodec
                .encodeCreationOperationTerminalV1(
                    (ServiceWireCodec.CreationOperationTerminalV1) value, context);
            default -> throw new IllegalStateException("unknown fixture type: " + type);
        };
    }

    private static Object inputValue(String type, JsonNode input) {
        return switch (type) {
            case "text8" -> new ServiceWireCodec.Text8(input.asText());
            case "optional-actor-ref" -> new ServiceWireCodec.OptionalActorRef(
                new ServiceWireCodec.OptionalText8(input.path("actorId").isNull()
                    ? null : input.path("actorId").asText()),
                input.path("generation").isNull() ? null
                    : new ServiceWireCodec.NonzeroU64(
                        input.path("generation").asLong()));
            case "sorted-text8-vector" -> new ServiceWireCodec.SortedText8Vector(
                nodes(input).stream().map(value ->
                    new ServiceWireCodec.Text8(value.asText())).toList());
            case "relocation-object-identity" -> relocationIdentity(input);
            case "descriptor-extension" -> descriptorExtension(input);
            case "aggregate-participant-vector" ->
                new ServiceWireCodec.AggregateParticipantVector(nodes(input).stream()
                    .map(GeneratedServiceWireCommandCodecConformanceTest::participant)
                    .toList());
            case "application-payload-bytes" -> {
                byte[] bytes = new byte[input.path("count").asInt()];
                Arrays.fill(bytes, (byte) input.path("repeatByte").asInt());
                yield new ServiceWireCodec.ApplicationPayloadBytes(bytes);
            }
            case "creation-operation-terminal-v1" ->
                new ServiceWireCodec.CreationOperationTerminalV1(
                    ServiceWireCodec.RequestTerminalResult.valueOf(
                        enumName(input.path("terminalResult").asText())),
                    ServiceWireCodec.FrameworkErrorCode.valueOf(
                        enumName(input.path("failureCode").asText())),
                    ServiceWireCodec.Bool8.valueOf(enumName(input.path("hasCreation").asText())),
                    null,
                    ServiceWireCodec.Bool8.valueOf(
                        enumName(input.path("hasApplicationPayload").asText())),
                    null);
            default -> throw new IllegalStateException("unsupported fixture input: " + type);
        };
    }

    private static ServiceWireCodec.RelocationObjectIdentity relocationIdentity(
        JsonNode input) {
        JsonNode actor = input.path("actor");
        return new ServiceWireCodec.RelocationObjectIdentityActor(
            ServiceWireCodec.StatefulObjectKind.valueOf(
                enumName(input.path("objectKind").asText())),
            new ServiceWireCodec.ActorRef(
                new ServiceWireCodec.Text8(actor.path("actorId").asText()),
                new ServiceWireCodec.NonzeroU64(
                    actor.path("objectGeneration").asLong())),
            new ServiceWireCodec.NonzeroU64(
                input.path("expectedAuthorityOwnerGeneration").asLong()));
    }

    private static ServiceWireCodec.MaintenanceAggregateParticipantV1 participant(
        JsonNode input) {
        return new ServiceWireCodec.MaintenanceAggregateParticipantV1(
            relocationIdentity(input.path("object")),
            new ServiceWireCodec.AuthorityStoreVersion(
                input.path("expectedStoreVersion").asText()),
            new ServiceWireCodec.AggregateParticipantMutationBytes(
                HexFormat.of().parseHex(input.path("mutationHex").asText())));
    }

    private static ServiceWireCodec.DescriptorExtension descriptorExtension(
        JsonNode input) {
        List<ServiceWireCodec.Text8> capabilities = nodes(
            input.path("protocolCapabilities")).stream()
            .map(value -> new ServiceWireCodec.Text8(value.asText())).toList();
        return new ServiceWireCodec.DescriptorExtension(
            input.path("runtimeState").isNull() ? null
                : ServiceWireCodec.RuntimeState.valueOf(
                    enumName(input.path("runtimeState").asText())),
            new ServiceWireCodec.ApplicationVersion(
                input.path("applicationVersion").asLong()),
            null,
            null,
            null,
            new ServiceWireCodec.SortedText8Vector(capabilities),
            ServiceWireCodec.ObjectRole.valueOf(
                enumName(input.path("objectRole").asText())),
            new ServiceWireCodec.U32(input.path("placementWeight").asInt()),
            new ServiceWireCodec.ObjectCapacityLimit(
                input.path("activeCapacityLimit").asInt()),
            new ServiceWireCodec.ObjectPendingCapacityLimit(
                input.path("pendingCapacityLimit").asInt()),
            new ServiceWireCodec.U32(input.path("activeCapacityUsed").asInt()),
            new ServiceWireCodec.U32(input.path("pendingCapacityUsed").asInt()));
    }

    private static List<JsonNode> nodes(JsonNode array) {
        List<JsonNode> result = new ArrayList<>();
        array.forEach(result::add);
        return result;
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
