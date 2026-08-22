package systems.zlink.framework.runtime.locations;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.zip.CRC32C;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;

final class ZLinkActorAuthorityPayloadCodecTest {
    // .NET-shaped authority-relocation-state slot body (frozen wire schema,
    // service-wire-v1.schema.json:6092), phase=captured(2): a source-phase
    // record with targetAttemptGeneration=0 and an empty target fence
    // (optional-rid / optional-text8 / ordinal-or-zero all zero-length or
    // zero), matching ZLinkStandaloneActorRelocationPrecommitCoordinator.cs
    // pre-command-40 authority write and ZLinkCanonicalRelocationAuthorityState
    // .cs EncodeSlot's field order (relocation-id, targetAttemptGeneration,
    // source*, target*, coordinator*, phase, applicationVersion,
    // sourceCleanupState). No trailing runtime-local extension.
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
        writeI64(body, 1L);
        body.write(0);
        return body.toByteArray();
    }

    private static void writeU64(ByteArrayOutputStream out, long value) {
        out.writeBytes(ByteBuffer.allocate(8).order(ByteOrder.BIG_ENDIAN)
            .putLong(value).array());
    }

    private static void writeI64(ByteArrayOutputStream out, long value) {
        writeU64(out, value);
    }

    private static void writeText8(ByteArrayOutputStream out, String value) {
        byte[] bytes = value.getBytes(StandardCharsets.UTF_8);
        out.write(bytes.length);
        out.writeBytes(bytes);
    }

    // Splices a relocation slot into the actor authority envelope's first
    // (currently-empty) conditional32 slot, replacing the outer envelope
    // body length and checksum. The two trailing empty conditional32 slots
    // (relocation, activation-recovery) are always the last 10 bytes of the
    // body, immediately before the 4-byte checksum.
    private static byte[] withRelocationSlot(byte[] encoded, byte[] slotBody) {
        int relocationSlotStart = encoded.length - 14;
        byte[] prefix = Arrays.copyOfRange(encoded, 0, relocationSlotStart);
        byte[] tail = Arrays.copyOfRange(
            encoded, relocationSlotStart + 5, encoded.length - 4);

        ByteArrayOutputStream body = new ByteArrayOutputStream();
        body.writeBytes(Arrays.copyOfRange(prefix, 11, prefix.length));
        body.write(1);
        body.writeBytes(ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN)
            .putInt(slotBody.length).array());
        body.writeBytes(slotBody);
        body.writeBytes(tail);
        byte[] bodyBytes = body.toByteArray();

        ByteArrayOutputStream envelope = new ByteArrayOutputStream();
        envelope.writeBytes(Arrays.copyOfRange(prefix, 0, 7));
        envelope.writeBytes(ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN)
            .putInt(bodyBytes.length).array());
        envelope.writeBytes(bodyBytes);
        byte[] withoutChecksum = envelope.toByteArray();
        var checksum = new CRC32C();
        checksum.update(withoutChecksum, 0, withoutChecksum.length);
        envelope.writeBytes(ByteBuffer.allocate(4).order(ByteOrder.BIG_ENDIAN)
            .putInt((int) checksum.getValue()).array());
        return envelope.toByteArray();
    }

    @Test
    void decodeAcceptsASourcePhaseRelocationSlotWithEmptyTargetFence() {
        var codec = new ZLinkActorAuthorityPayloadCodec();
        byte[] steady = codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A", "B", "C", 2, 1, "D", 3, "E",
            RoutingId.from("F"), 4);
        byte[] withRelocation =
            withRelocationSlot(steady, dotNetSourcePhaseRelocationSlotBody());

        var decoded = codec.decode(withRelocation);

        assertTrue(decoded.isPresent());
        assertEquals("D", decoded.get().ownerId());
        assertEquals(3, decoded.get().ownerLeaseGeneration());
        assertEquals("E", decoded.get().meshName());
        assertEquals(RoutingId.from("F"), decoded.get().nodeRid());
        assertEquals(4, decoded.get().nodeGeneration());

        // The relocation slot must actually be stripped (boundary-only, per
        // the fix), not merely fall back to the original unstripped payload:
        // the recovered application payload is byte-identical to the steady
        // envelope the slot was spliced into.
        assertArrayEquals(steady,
            systems.zlink.framework.runtime.internal.locations
                .ZLinkCanonicalRelocationAuthorityStateCodec
                .applicationPayloadOrOriginal(withRelocation));
    }

    @Test
    void actorAuthorityMatchesTheCrossLanguageByteVector() {
        var codec = new ZLinkActorAuthorityPayloadCodec();
        byte[] encoded = codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A",
            "B",
            "C",
            2,
            1,
            "D",
            3,
            "E",
            RoutingId.from("F"),
            4);

        assertArrayEquals(HexFormat.of().parseHex(
            "5a4c4155010000000000340001001001410142010143"
                + "0000000000000002010144000000000000000301450146"
                + "000000000000000400000000000000000000b2374797"),
            encoded);
        var decoded = codec.decode(encoded).orElseThrow();
        assertEquals(ZLinkActorAuthorityPayloadCodec.State.READY,
            decoded.state());
        assertEquals("A", decoded.stableType());
        assertEquals("B", decoded.actorId());
        assertEquals("C", decoded.currentSpotId());
        assertEquals(2, decoded.currentSpotGeneration());
        assertEquals(1, decoded.currentSpotKind());
        assertEquals("D", decoded.ownerId());
        assertEquals(3, decoded.ownerLeaseGeneration());
        assertEquals("E", decoded.meshName());
        assertEquals(RoutingId.from("F"), decoded.nodeRid());
        assertEquals(4, decoded.nodeGeneration());
    }

    @Test
    void opaqueNodeGenerationAcceptsTheUnsignedHighBitFixedVector() {
        var codec = new ZLinkActorAuthorityPayloadCodec();
        byte[] encoded = codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A", "B", "C", 2, 1, "D", 3, "E",
            RoutingId.from("F"), Long.MIN_VALUE);

        assertArrayEquals(HexFormat.of().parseHex("8000000000000000"),
            Arrays.copyOfRange(encoded, 45, 53));
        assertEquals(Long.MIN_VALUE,
            codec.decode(encoded).orElseThrow().nodeGeneration());
    }

    @Test
    void opaqueCurrentSpotGenerationAcceptsTheUnsignedHighBitFixedVector() {
        var codec = new ZLinkActorAuthorityPayloadCodec();
        long generation = 0xa70186055079275aL;
        byte[] encoded = codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A", "B", "C", generation, 1, "D", 3, "E",
            RoutingId.from("F"), 4);

        assertArrayEquals(HexFormat.of().parseHex("a70186055079275a"),
            Arrays.copyOfRange(encoded, 22, 30));
        assertEquals(generation,
            codec.decode(encoded).orElseThrow().currentSpotGeneration());
    }

    @Test
    void opaqueNodeGenerationRejectsOnlyZero() {
        var codec = new ZLinkActorAuthorityPayloadCodec();
        assertThrows(IllegalArgumentException.class, () -> codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A", "B", "C", 2, 1, "D", 3, "E",
            RoutingId.from("F"), 0));

        byte[] zeroNodeGeneration = codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A", "B", "C", 2, 1, "D", 3, "E",
            RoutingId.from("F"), 1);
        Arrays.fill(zeroNodeGeneration, 45, 53, (byte) 0);
        updateChecksum(zeroNodeGeneration);
        assertTrue(codec.decode(zeroNodeGeneration).isEmpty());
    }

    @Test
    void boundedGenerationsRetainTheirSignedPositiveValidation() {
        var codec = new ZLinkActorAuthorityPayloadCodec();
        assertThrows(IllegalArgumentException.class, () -> codec.encode(
            ZLinkActorAuthorityPayloadCodec.State.READY,
            "A", "B", "C", 2, 1, "D", Long.MIN_VALUE, "E",
            RoutingId.from("F"), 1));
    }

    private static void updateChecksum(byte[] payload) {
        var checksum = new CRC32C();
        int checksumOffset = payload.length - Integer.BYTES;
        checksum.update(payload, 0, checksumOffset);
        long value = checksum.getValue();
        for (int shift = 24; shift >= 0; shift -= 8) {
            payload[checksumOffset++] = (byte) (value >>> shift);
        }
    }
}
