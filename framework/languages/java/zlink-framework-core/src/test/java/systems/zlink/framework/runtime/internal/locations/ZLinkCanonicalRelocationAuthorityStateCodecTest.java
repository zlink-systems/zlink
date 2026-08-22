package systems.zlink.framework.runtime.internal.locations;
import java.io.IOException;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.Arrays;
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

    // STOP (per Ruling): a .NET-shaped source-phase (phase=captured,
    // targetAttemptGeneration=0, empty target fence) relocation slot per the
    // frozen wire schema (authority-relocation-state,
    // service-wire-v1.schema.json:6092) still does not decode through this
    // codec's own semantic projection: field 12+ diverges from the schema's
    // coordinatorOwnerId/phase/sourceCleanupState layout (this codec instead
    // reads a legacy placementReservationToken/capacityOwner* shape).
    // applicationPayloadOrOriginal (see ZLinkActorAuthorityPayloadCodecTest
    // .decodeAcceptsASourcePhaseRelocationSlotWithEmptyTargetFence) no
    // longer depends on this projection succeeding, so this divergence does
    // not block the Actor authority path — it is recorded here so a reader
    // does not mistake decode()'s continued rejection for a regression.
    @Test
    void decodeStillRejectsADotNetShapedSourcePhaseSlot() {
        byte[] payload = authorityPayload();
        int slotOffset = relocationSlotOffset(payload);
        byte[] slotBody = dotNetSourcePhaseRelocationSlotBody();

        byte[] prefix = Arrays.copyOfRange(payload, 0, slotOffset);
        byte[] tail = Arrays.copyOfRange(payload, slotOffset + 5, payload.length - 4);

        ByteBuffer body = ByteBuffer.allocate(
            prefix.length - 11 + 5 + slotBody.length + tail.length);
        body.put(prefix, 11, prefix.length - 11);
        body.put((byte) 1);
        body.order(ByteOrder.BIG_ENDIAN).putInt(slotBody.length);
        body.put(slotBody);
        body.put(tail);

        ByteBuffer envelope = ByteBuffer.allocate(11 + body.capacity() + 4);
        envelope.put(prefix, 0, 7);
        envelope.order(ByteOrder.BIG_ENDIAN).putInt(body.capacity());
        envelope.put(body.array());
        byte[] withoutChecksum = Arrays.copyOfRange(
            envelope.array(), 0, envelope.position());
        CRC32C checksum = new CRC32C();
        checksum.update(withoutChecksum, 0, withoutChecksum.length);
        envelope.order(ByteOrder.BIG_ENDIAN).putInt((int) checksum.getValue());
        byte[] withRelocation = envelope.array();

        assertNull(ZLinkCanonicalRelocationAuthorityStateCodec.decode(withRelocation));
    }

    // .NET-shaped authority-relocation-state slot body (frozen wire schema),
    // phase=captured(2): a source-phase record with targetAttemptGeneration=0
    // and an empty target fence, matching
    // ZLinkStandaloneActorRelocationPrecommitCoordinator.cs's pre-command-40
    // authority write and ZLinkCanonicalRelocationAuthorityState.cs
    // EncodeSlot's field order. No trailing runtime-local extension.
    private static byte[] dotNetSourcePhaseRelocationSlotBody() {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        writeU64(body, 0L);
        writeU64(body, 9L);
        writeU64(body, 0L);
        writeText8(body, "node-a");
        writeU64(body, 3L);
        writeText8(body, "owner-a");
        writeU64(body, 6L);
        writeText8(body, "");
        writeU64(body, 0L);
        writeText8(body, "");
        writeU64(body, 0L);
        writeText8(body, "coordinator-a");
        writeU64(body, 11L);
        writeText8(body, "node-c");
        writeU64(body, 5L);
        body.write(2);
        writeU64(body, 1L);
        body.write(0);
        return body.toByteArray();
    }

    private static void writeU64(ByteArrayOutputStream out, long value) {
        out.writeBytes(ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN)
            .putLong(value).array());
    }

    private static void writeText8(ByteArrayOutputStream out, String value) {
        byte[] bytes = value.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        out.write(bytes.length);
        out.writeBytes(bytes);
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
