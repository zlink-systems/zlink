package systems.zlink.framework.runtime.spots;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import com.fasterxml.jackson.databind.JsonNode;
import com.fasterxml.jackson.databind.ObjectMapper;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.HexFormat;
import java.util.List;
import java.util.UUID;
import org.junit.jupiter.api.Test;
import systems.zlink.contracts.core.RoutingId;
import systems.zlink.framework.runtime.protocol.ServiceWireConstants;

final class ZLinkCanonicalRelocationProtocolTest {
    @Test
    void semanticProjectionPreservesEveryCanonicalGoldenRecord()
        throws Exception {
        JsonNode fixture = new ObjectMapper().readTree(
            Files.readString(fixture()));
        for (JsonNode item : fixture.path("canonical")) {
            byte[] encoded = HexFormat.of().parseHex(
                item.path("hex").asText());
            byte[] projected = switch (item.path("command").asInt()) {
                case ServiceWireConstants.COMMAND_RELOCATION_READY ->
                    ZLinkCanonicalRelocationProtocol.encodeReady(
                        ZLinkCanonicalRelocationProtocol.decodeReady(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_DATA ->
                    ZLinkCanonicalRelocationProtocol.encodeControlData(
                        ZLinkCanonicalRelocationProtocol.decodeControlData(
                            encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_ACK ->
                    ZLinkCanonicalRelocationProtocol.encodeAck(
                        ZLinkCanonicalRelocationProtocol.decodeAck(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_SEAL ->
                    ZLinkCanonicalRelocationProtocol.encodeSeal(
                        ZLinkCanonicalRelocationProtocol.decodeSeal(encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_COMPLETE ->
                    ZLinkCanonicalRelocationProtocol.encodeComplete(
                        ZLinkCanonicalRelocationProtocol.decodeComplete(
                            encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_PREPARE ->
                    ZLinkCanonicalRelocationProtocol.encodePrepare(
                        ZLinkCanonicalRelocationProtocol.decodePrepare(
                            encoded));
                case ServiceWireConstants.COMMAND_RELOCATION_RESERVED ->
                    ZLinkCanonicalRelocationProtocol.encodeReserved(
                        ZLinkCanonicalRelocationProtocol.decodeReserved(
                            encoded));
                default -> throw new AssertionError("unexpected command");
            };
            assertArrayEquals(
                encoded,
                projected,
                item.path("name").asText());
        }
    }

    @Test
    void boundSessionParticipantRoundTripsLastAcceptedSessionSequence() {
        for (long lastAccepted : new long[] {0, 42}) {
            ZLinkCanonicalRelocationProtocol.Prepare prepare =
                new ZLinkCanonicalRelocationProtocol.Prepare(
                    new UUID(1, 2),
                    3,
                    1,
                    coordinator(),
                    candidate(),
                    ZLinkCanonicalRelocationProtocol.SOURCE,
                    object(),
                    RoutingId.from("source-node"),
                    5,
                    64,
                    128,
                    List.of(boundSessionParticipant(lastAccepted)),
                    new ZLinkCanonicalRelocationProtocol.Root("ref", 13),
                    14);
            ZLinkCanonicalRelocationProtocol.Prepare decoded =
                ZLinkCanonicalRelocationProtocol.decodePrepare(
                    ZLinkCanonicalRelocationProtocol.encodePrepare(prepare));
            assertEquals(prepare, decoded);
            assertEquals(
                lastAccepted,
                decoded.participants().getFirst()
                    .lastAcceptedSessionSequence());
        }

        ZLinkCanonicalRelocationProtocol.Ready acceptance = ready(
            ZLinkCanonicalRelocationProtocol.SOURCE,
            0,
            0,
            List.of(boundSessionParticipant(42)));
        assertEquals(
            acceptance,
            ZLinkCanonicalRelocationProtocol.decodeReady(
                ZLinkCanonicalRelocationProtocol.encodeReady(acceptance)));
    }

    @Test
    void encodeReadyEnforcesTheOfferAndAcceptSentinels() {
        List<ZLinkCanonicalRelocationProtocol.Participant> participants =
            List.of(ZLinkCanonicalRelocationProtocol.Participant.plain(
                1, 64, 128));
        //  Valid shapes pass.
        ZLinkCanonicalRelocationProtocol.encodeReady(
            ready(ZLinkCanonicalRelocationProtocol.TARGET, 64, 128,
                List.of()));
        ZLinkCanonicalRelocationProtocol.encodeReady(
            ready(ZLinkCanonicalRelocationProtocol.SOURCE, 0, 0,
                participants));
        //  Target offer: participants must be EMPTY, offered nonzero.
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkCanonicalRelocationProtocol.encodeReady(
                ready(ZLinkCanonicalRelocationProtocol.TARGET, 64, 128,
                    participants)));
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkCanonicalRelocationProtocol.encodeReady(
                ready(ZLinkCanonicalRelocationProtocol.TARGET, 0, 128,
                    List.of())));
        //  Source accept: participants NONEMPTY, offered zero.
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkCanonicalRelocationProtocol.encodeReady(
                ready(ZLinkCanonicalRelocationProtocol.SOURCE, 64, 128,
                    participants)));
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkCanonicalRelocationProtocol.encodeReady(
                ready(ZLinkCanonicalRelocationProtocol.SOURCE, 0, 0,
                    List.of())));
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkCanonicalRelocationProtocol.encodeReady(
                ready(3, 64, 128, List.of())));
    }

    @Test
    void decodeReadyRejectsWireSentinelViolations() {
        //  offeredMessages carries a unique marker so the role byte (which
        //  immediately precedes it) can be located without re-encoding.
        long marker = 0x0102030405060708L;
        byte[] offer = ZLinkCanonicalRelocationProtocol.encodeReady(
            ready(ZLinkCanonicalRelocationProtocol.TARGET, marker, 128,
                List.of()));
        int roleIndex = markerIndex(offer, marker) - 1;
        assertEquals(
            ZLinkCanonicalRelocationProtocol.TARGET, offer[roleIndex]);
        byte[] invalidOffer = offer.clone();
        invalidOffer[roleIndex] =
            (byte) ZLinkCanonicalRelocationProtocol.SOURCE;
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkCanonicalRelocationProtocol.decodeReady(invalidOffer));

        //  The acceptance shares the identical prefix, so the role byte
        //  lives at the same offset.
        byte[] acceptance = ZLinkCanonicalRelocationProtocol.encodeReady(
            ready(ZLinkCanonicalRelocationProtocol.SOURCE, 0, 0,
                List.of(ZLinkCanonicalRelocationProtocol.Participant.plain(
                    1, 64, 128))));
        assertEquals(
            ZLinkCanonicalRelocationProtocol.SOURCE, acceptance[roleIndex]);
        byte[] invalidAcceptance = acceptance.clone();
        invalidAcceptance[roleIndex] =
            (byte) ZLinkCanonicalRelocationProtocol.TARGET;
        assertThrows(IllegalArgumentException.class, () ->
            ZLinkCanonicalRelocationProtocol.decodeReady(invalidAcceptance));
    }

    private static int markerIndex(byte[] encoded, long marker) {
        byte[] pattern = java.nio.ByteBuffer.allocate(8)
            .putLong(marker).array();
        for (int index = 0; index <= encoded.length - 8; index++) {
            boolean matches = true;
            for (int offset = 0; offset < 8; offset++) {
                if (encoded[index + offset] != pattern[offset]) {
                    matches = false;
                    break;
                }
            }
            if (matches) {
                return index;
            }
        }
        throw new AssertionError("offered marker was not found");
    }

    private static ZLinkCanonicalRelocationProtocol.Ready ready(
        int role,
        long offeredMessages,
        long offeredBytes,
        List<ZLinkCanonicalRelocationProtocol.Participant> participants) {
        return new ZLinkCanonicalRelocationProtocol.Ready(
            new UUID(1, 2),
            3,
            1,
            coordinator(),
            candidate(),
            object(),
            role,
            offeredMessages,
            offeredBytes,
            participants,
            5,
            6,
            12,
            new ZLinkCanonicalRelocationProtocol.Root("ref", 13),
            14,
            List.of(new ZLinkCanonicalRelocationProtocol.Progress(1, 0, 0)));
    }

    private static ZLinkCanonicalRelocationProtocol.Participant
        boundSessionParticipant(long lastAccepted) {
        return new ZLinkCanonicalRelocationProtocol.Participant(
            1,
            2,
            RoutingId.from("session-owner-node"),
            15,
            "session-owner",
            16,
            RoutingId.from("session-rid"),
            17,
            lastAccepted,
            64,
            128);
    }

    private static ZLinkCanonicalRelocationProtocol.Coordinator coordinator() {
        return new ZLinkCanonicalRelocationProtocol.Coordinator(
            "source-owner", 4, RoutingId.from("source-node"), 5, "store-v1");
    }

    private static ZLinkCanonicalRelocationProtocol.Candidate candidate() {
        return new ZLinkCanonicalRelocationProtocol.Candidate(
            RoutingId.from("target-node"), 6, "target-owner", 7);
    }

    private static ZLinkCanonicalRelocationProtocol.ObjectFence object() {
        return new ZLinkCanonicalRelocationProtocol.ObjectFence(
            1, "actor-a", "", 8, 9);
    }

    private static Path fixture() {
        Path current = Path.of(System.getProperty("user.dir")).toAbsolutePath();
        while (current != null) {
            Path candidate = current.resolve(
                "runtime/protocol/golden/relocation-control-v1.json");
            if (Files.isRegularFile(candidate)) {
                return candidate;
            }
            current = current.getParent();
        }
        throw new IllegalStateException(
            "shared relocation fixture was not found");
    }
}
