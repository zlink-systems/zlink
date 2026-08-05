package systems.zlink.framework.runtime.internal.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Instant;
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
    private static final ZLinkRelocationStored STORED =
        new ZLinkRelocationStored(
            "sha256:relocation-test",
            17,
            Instant.MAX,
            Instant.now());

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
                ZLinkAuthorityGenerationTransition.PRESERVE,
                STORED,
                true));
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
            ZLinkAuthorityGenerationTransition.NEW_OWNER,
            STORED,
            false);
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
            ZLinkAuthorityGenerationTransition.PRESERVE,
            STORED,
            true);

        assertArrayEquals(
            application,
            ZLinkCanonicalRelocationAuthorityStateCodec
                .applicationPayloadOrOriginal(next));
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
            ZLinkAuthorityGenerationTransition.NEW_OWNER,
            STORED,
            false);

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
                ZLinkAuthorityGenerationTransition.PRESERVE,
                STORED,
                true));
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
            ZLinkAuthorityGenerationTransition.NEW_OWNER,
            STORED,
            false);
        byte[] malformed = canonical.clone();
        malformed[relocationSlotOffset(malformed)] = 2;
        rewriteChecksum(malformed);

        assertThrows(
            IllegalArgumentException.class,
            () -> ZLinkCanonicalRelocationAuthorityStateCodec.publish(
                malformed,
                request,
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                STORED,
                false));
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
                ZLinkAuthorityGenerationTransition.NEW_OWNER,
                STORED,
                false));
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
            new ZLinkLocationOwnerToken("owner-b", 12));
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
                } catch (java.io.IOException failure) {
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
