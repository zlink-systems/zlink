package systems.zlink.framework.runtime.internal.locations;
import java.io.IOException;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNotNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HexFormat;
import java.util.List;
import java.util.Optional;
import java.util.UUID;
import java.util.regex.Pattern;
import java.util.zip.CRC32C;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.locations.ZLinkPlacementObjectKind;
import systems.zlink.framework.runtime.locations.ZLinkServiceAuthorityPayloadCodec;

final class ZLinkCanonicalRelocationAuthorityStateCodecTest {
    @Test
    void preserveRequiresTheApplicationPayloadToMatchTheTargetFence() {
        var request = request(
            new UUID(0, 9),
            7,
            List.of(participant(
                ZLinkAuthorityGenerationTransition.PRESERVE,
                authorityPayload())));

        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                authorityPayload(),
                request,
                ZLinkAuthorityGenerationTransition.PRESERVE));
    }

    @Test
    void preserveKeepsAValidatedCanonicalApplicationPayload() {
        var initial = request(
            new UUID(0, 9),
            7,
            List.of(participant(
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                authorityPayload())));
        byte[] canonical = ZLinkCanonicalRelocationAuthorityStateCodec.publish(
            authorityPayload(),
            initial,
            ZLinkAuthorityGenerationTransition.NEW_OWNER);
        byte[] application =
            ZLinkCanonicalRelocationAuthorityStateCodec
                .applicationPayloadOrOriginal(canonical);

        var successor = request(
            new UUID(0, 9),
            8,
            List.of(participant(
                ZLinkAuthorityGenerationTransition.PRESERVE,
                canonical)));
        byte[] next = ZLinkCanonicalRelocationAuthorityStateCodec.publish(
            canonical,
            successor,
            ZLinkAuthorityGenerationTransition.PRESERVE);

        assertArrayEquals(
            application,
            ZLinkCanonicalRelocationAuthorityStateCodec
                .applicationPayloadOrOriginal(next));
    }

    @Test
    void publicationKeepsAggregateAndTargetAttemptFencesDistinct() {
        var request = request(
            new UUID(0, 9),
            7,
            List.of(participant(
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                authorityPayload())));

        var publication = ZLinkCanonicalRelocationAuthorityStateCodec.decode(
            ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                authorityPayload(),
                request,
                ZLinkAuthorityGenerationTransition.NEW_OWNER));

        assertNotNull(publication);
        assertEquals(7, publication.aggregateGeneration());
        assertEquals(8, publication.targetAttemptGeneration());
        assertEquals("version-1",
            publication.coordinatorExpectedStoreVersion());
        assertEquals(3, publication.phase());
    }

    @Test
    void canonicalStateFromAnotherAggregateIsRejected() {
        var initial = request(
            new UUID(0, 9),
            7,
            List.of(participant(
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                authorityPayload())));
        byte[] canonical = ZLinkCanonicalRelocationAuthorityStateCodec.publish(
            authorityPayload(),
            initial,
            ZLinkAuthorityGenerationTransition.NEW_OWNER);

        UUID differentAggregate = new UUID(0, 10);
        var successor = request(
            differentAggregate,
            8,
            List.of(participant(
                ZLinkAuthorityGenerationTransition.PRESERVE,
                canonical)),
            withRelocationId(goldenRoot(), differentAggregate));

        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                canonical,
                successor,
                ZLinkAuthorityGenerationTransition.PRESERVE));
    }

    @Test
    void crcValidMalformedCanonicalSlotIsRejected() {
        var request = request(
            new UUID(0, 9),
            7,
            List.of(participant(
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                authorityPayload())));
        byte[] canonical = ZLinkCanonicalRelocationAuthorityStateCodec.publish(
            authorityPayload(),
            request,
            ZLinkAuthorityGenerationTransition.NEW_OWNER);
        byte[] malformed = canonical.clone();
        malformed[relocationSlotOffset(malformed)] = 2;
        rewriteChecksum(malformed);

        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                malformed,
                request,
                ZLinkAuthorityGenerationTransition.NEW_OWNER));
    }

    @Test
    void sharedAuthorityRelocationGoldenDecodesAndEncodesExactly()
        throws IOException {
        JsonNode fixture = new ObjectMapper().readTree(
            Files.readString(authorityRelocationFixture()));
        int accepted = 0;
        for (JsonNode vector : fixture.path("valid")) {
            String name = vector.path("name").asText();
            byte[] encoded = HexFormat.of().parseHex(
                vector.path("hex").asText());
            Long rootGeneration = vector.path("rootAggregateGeneration").isNull()
                ? null
                : Long.parseLong(
                    vector.path("rootAggregateGeneration").asText());
            var state = ZLinkCanonicalRelocationAuthorityStateCodec.decodeState(
                encoded,
                rootGeneration);
            assertGoldenState(vector.path("decoded"), state, name);
            assertArrayEquals(
                encoded,
                ZLinkCanonicalRelocationAuthorityStateCodec.encodeState(state),
                "golden re-encode differs: " + name);
            assertNotNull(
                ZLinkCanonicalRelocationAuthorityStateCodec.decode(
                    withRelocationSlot(authorityPayload(), encoded)),
                "authority projection rejected golden: " + name);
            accepted++;
        }
        int rejected = 0;
        for (JsonNode vector : fixture.path("invalid")) {
            String name = vector.path("name").asText();
            byte[] encoded = HexFormat.of().parseHex(
                vector.path("hex").asText());
            Long rootGeneration = vector.path("rootAggregateGeneration").isNull()
                ? null
                : Long.parseLong(
                    vector.path("rootAggregateGeneration").asText());
            assertThrows(
                IllegalArgumentException.class,
                () -> ZLinkCanonicalRelocationAuthorityStateCodec.decodeState(
                    encoded,
                    rootGeneration),
                "golden reject was accepted: " + name);
            rejected++;
        }
        assertEquals(6, accepted);
        assertEquals(6, rejected);
    }

    @Test
    void nulInAuthorityTextIsRejectedBeforeProjection() {
        var request = request(
            new UUID(0, 9),
            7,
            List.of(participant(
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                authorityPayload())));
        byte[] malformed = authorityPayload();
        int ownerLength = ownerTextOffset(malformed);
        malformed[ownerLength + 1] = 0;
        rewriteChecksum(malformed);

        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                malformed,
                request,
                ZLinkAuthorityGenerationTransition.NEW_OWNER));
    }

    private static void assertGoldenState(
        JsonNode expected,
        ZLinkCanonicalRelocationAuthorityStateCodec.State actual,
        String name) {
        assertEquals(expected.path("relocation").path("high").asLong(),
            actual.relocationHigh(), name);
        assertEquals(expected.path("relocation").path("low").asLong(),
            actual.relocationLow(), name);
        assertEquals(Long.parseLong(expected.path("aggregateGeneration").asText()),
            actual.aggregateGeneration(), name);
        assertEquals(Long.parseLong(
                expected.path("targetAttemptGeneration").asText()),
            actual.targetAttemptGeneration(), name);
        assertEquals(expected.path("relocationReference").asText(),
            actual.relocationReference(), name);
        assertEquals(expected.path("relocationChecksumCrc32c").asLong(),
            actual.relocationChecksumCrc32c(), name);
        assertEquals(expected.path("sourceNodeRidHex").asText(),
            HexFormat.of().formatHex(actual.sourceNodeRid().toBytes()), name);
        assertEquals(Long.parseLong(expected.path("sourceNodeGeneration").asText()),
            actual.sourceNodeGeneration(), name);
        assertEquals(expected.path("sourceOwnerId").asText(),
            actual.sourceOwnerId(), name);
        assertEquals(Long.parseLong(
                expected.path("sourceOwnerLeaseGeneration").asText()),
            actual.sourceOwnerLeaseGeneration(), name);
        assertEquals(expected.path("targetNodeRidHex").asText(),
            HexFormat.of().formatHex(actual.targetNodeRid().toBytes()), name);
        assertEquals(Long.parseLong(expected.path("targetNodeGeneration").asText()),
            actual.targetNodeGeneration(), name);
        assertEquals(expected.path("targetOwnerId").asText(),
            actual.targetOwnerId(), name);
        assertEquals(Long.parseLong(
                expected.path("targetOwnerLeaseGeneration").asText()),
            actual.targetOwnerLeaseGeneration(), name);
        assertEquals(expected.path("coordinatorOwnerId").asText(),
            actual.coordinatorOwnerId(), name);
        assertEquals(Long.parseLong(
                expected.path("coordinatorLeaseGeneration").asText()),
            actual.coordinatorLeaseGeneration(), name);
        assertEquals(expected.path("coordinatorNodeRidHex").asText(),
            HexFormat.of().formatHex(actual.coordinatorNodeRid().toBytes()), name);
        assertEquals(Long.parseLong(
                expected.path("coordinatorNodeGeneration").asText()),
            actual.coordinatorNodeGeneration(), name);
        assertEquals(expected.path("coordinatorExpectedStoreVersion").asText(),
            actual.coordinatorExpectedStoreVersion(), name);
        assertEquals(phase(expected.path("phase").asText()), actual.phase(), name);
        assertEquals(Long.parseLong(expected.path("applicationVersion").asText()),
            actual.applicationVersion(), name);
        assertEquals(cleanup(expected.path("sourceCleanupState").asText()),
            actual.sourceCleanupState(), name);
    }

    private static int phase(String value) {
        return switch (value) {
            case "preparing" -> 1;
            case "captured" -> 2;
            case "prepared" -> 3;
            case "committed" -> 4;
            case "activating" -> 5;
            case "activated" -> 6;
            case "cleaning" -> 7;
            case "completed" -> 8;
            case "aborted" -> 9;
            default -> throw new IllegalArgumentException("unknown phase: " + value);
        };
    }

    private static int cleanup(String value) {
        return switch (value) {
            case "pending" -> 0;
            case "completed" -> 1;
            case "sourceLeaseExpired" -> 2;
            default -> throw new IllegalArgumentException(
                "unknown cleanup state: " + value);
        };
    }

    private static byte[] withRelocationSlot(byte[] payload, byte[] state) {
        int slotOffset = relocationSlotOffset(payload);
        byte[] body = new byte[
            payload.length - 11 - 4 - 5 + state.length];
        int slotInBody = slotOffset - 11;
        System.arraycopy(payload, 11, body, 0, slotInBody);
        System.arraycopy(state, 0, body, slotInBody, state.length);
        int tailSource = slotOffset + 5;
        int tailLength = payload.length - 4 - tailSource;
        System.arraycopy(
            payload,
            tailSource,
            body,
            slotInBody + state.length,
            tailLength);
        ByteBuffer envelope = ByteBuffer.allocate(11 + body.length + 4)
            .order(ByteOrder.BIG_ENDIAN);
        envelope.put(payload, 0, 7);
        envelope.putInt(body.length);
        envelope.put(body);
        CRC32C checksum = new CRC32C();
        checksum.update(envelope.array(), 0, envelope.position());
        envelope.putInt((int) checksum.getValue());
        return envelope.array();
    }

    private static Path authorityRelocationFixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path fixture = current.resolve(
                "runtime/protocol/golden/authority-relocation-state-v1.json");
            if (Files.isRegularFile(fixture)) {
                return fixture;
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared authority relocation fixture was not found");
    }

    private static ZLinkAggregateRelocationCoordinator.Request request(
        UUID aggregateId,
        long generation,
        List<ZLinkAggregateRelocationCoordinator.Participant> participants) {
        return request(aggregateId, generation, participants, goldenRoot());
    }

    private static ZLinkAggregateRelocationCoordinator.Request request(
        UUID aggregateId,
        long generation,
        List<ZLinkAggregateRelocationCoordinator.Participant> participants,
        byte[] root) {
        return new ZLinkAggregateRelocationCoordinator.Request(
            aggregateId,
            generation,
            generation + 1,
            participants,
            root,
            new ZLinkMeshNodeDescriptorKey(
                "game",
                RoutingId.from("node-b")),
            4,
            new ZLinkPlacementCapacityBundle(
                0,
                0,
                Optional.empty()),
            new ZLinkLocationOwnerToken("owner-b", 12),
            "version-1");
    }

    private static ZLinkAggregateRelocationCoordinator.Participant participant(
        ZLinkAuthorityGenerationTransition transition,
        byte[] payload) {
        return new ZLinkAggregateRelocationCoordinator.Participant(
            "spot:room-a",
            ZLinkPlacementObjectKind.USER_SPOT,
            3,
            5,
            "version-1",
            transition,
            payload,
            new byte[] {2});
    }

    private static byte[] authorityPayload() {
        return new ZLinkServiceAuthorityPayloadCodec().encodeUser(
            ZLinkServiceAuthorityPayloadCodec.State.READY,
            "RoomSpot",
            "room-a",
            "owner-a",
            6,
            "game",
            RoutingId.from("node-a"),
            3);
    }

    private static byte[] goldenRoot() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path fixture = current.resolve(
                "runtime/protocol/golden/relocation-envelope-v1.json");
            if (Files.isRegularFile(fixture)) {
                try {
                    var match = Pattern.compile(
                        "\\\"logicalHex\\\"\\s*:\\s*\\\"([0-9a-f]+)\\\"")
                        .matcher(Files.readString(fixture));
                    if (match.find()) {
                        return HexFormat.of().parseHex(match.group(1));
                    }
                } catch (IOException failure) {
                    throw new IllegalStateException(failure);
                }
            }
            current = current.getParent();
        }
        throw new IllegalStateException("shared relocation fixture was not found");
    }

    private static byte[] withRelocationId(byte[] root, UUID id) {
        byte[] copy = root.clone();
        ByteBuffer buffer = ByteBuffer.wrap(copy).order(ByteOrder.BIG_ENDIAN);
        buffer.putLong(id.getMostSignificantBits());
        buffer.putLong(id.getLeastSignificantBits());
        return copy;
    }

    private static int ownerTextOffset(byte[] payload) {
        int offset = 11;
        offset += 1;
        offset += 1;
        int objectLength = readU16(payload, offset);
        offset += 2 + objectLength;
        return offset;
    }

    private static int relocationSlotOffset(byte[] payload) {
        int offset = ownerTextOffset(payload);
        offset += 1 + Byte.toUnsignedInt(payload[offset]);
        offset += 8;
        offset += 1 + Byte.toUnsignedInt(payload[offset]);
        offset += 1 + Byte.toUnsignedInt(payload[offset]);
        offset += 8;
        return offset;
    }

    private static int readU16(byte[] bytes, int offset) {
        return Short.toUnsignedInt(ByteBuffer.wrap(bytes, offset, 2)
            .order(ByteOrder.BIG_ENDIAN)
            .getShort());
    }

    private static void rewriteChecksum(byte[] payload) {
        CRC32C checksum = new CRC32C();
        checksum.update(payload, 0, payload.length - 4);
        ByteBuffer.wrap(payload, payload.length - 4, 4)
            .order(ByteOrder.BIG_ENDIAN)
            .putInt((int) checksum.getValue());
    }
}
